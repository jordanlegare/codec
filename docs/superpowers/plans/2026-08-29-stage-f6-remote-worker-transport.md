# Stage F.6 Remote Worker Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded provider-neutral remote-worker transport adapter that implements `DistributedWorker` and composes unchanged through F.2 and F.5.

**Architecture:** Add one caller-supplied `DistributedWorkerTransport` interface and one concrete `RemoteDistributedWorker` adapter. The adapter performs its own bounded transport/request/response checks, dispatches once, preserves provider errors without retry, and returns structured outputs to the existing F.2 `execute_partition()` validator; F.5 needs no production change.

**Tech Stack:** C++20, existing `codec::Result`, `ExtractedRecord`, `ProcessorOutput`, `DistributedWorker`, CMake/CTest, GitHub Actions GCC/Clang/ASan+UBSan matrix.

**Spec:** `docs/superpowers/specs/2026-08-29-stage-f6-remote-worker-transport-design.md`

## Global Constraints

- Base exactly `main` at `f98575cb88ac2115b7960c4fb4d417e9b8a381d0`.
- Additive C++ API only; no CODA, CDP1, C ABI, CLI, Stage E, or S0/S1/D semantic change.
- No concrete socket/HTTP/gRPC transport or CODEC RPC wire format.
- No authentication/authorization/attestation, discovery/health, retry/failover, lease/heartbeat/cancellation/exactly-once, concurrency, persistence, deployment, or scale claim.
- F.2 remains authoritative for partition identity, ordered exact record membership, SHA-256 input verification, and ProcessorOutput semantic validation.
- F.5 scheduler implementation and public API remain unchanged.
- Every provider dispatch occurs at most once per `RemoteDistributedWorker::execute()` call.

---

### Task 1: Establish the RED F.6 public contract

**Files:**
- Create: `tests/test_distributed_remote_worker.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `DistributedWorker`, `execute_partition()`, `schedule_partitions()`, `DistributedLocationIndex`, `ObjectStoreBackend`.
- Produces test expectations for: `DistributedRemoteWorkerLimits`, `DistributedRemoteExecutionResponse`, `DistributedWorkerTransport`, `RemoteDistributedWorker`.

- [ ] **Step 1: Register the new test translation unit**

Add `tests/test_distributed_remote_worker.cpp` beside the other distributed tests in `codec_tests`:

```cmake
    tests/test_distributed_location_index.cpp
    tests/test_distributed_partition.cpp
+    tests/test_distributed_remote_worker.cpp
    tests/test_distributed_retrieval.cpp
    tests/test_distributed_scheduler.cpp
```

- [ ] **Step 2: Add a failing contract test**

Create a test helper transport implementing the wished-for interface:

```cpp
class RecordingTransport final : public codec::DistributedWorkerTransport {
 public:
  std::string name() const override { return transport_name; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    seen_worker = std::string{worker_name};
    seen_processor = std::string{processor_name};
    seen_records.assign(inputs.begin(), inputs.end());
    return codec::DistributedRemoteExecutionResponse{
        .worker_name = seen_worker,
        .processor_name = seen_processor,
        .outputs = {valid_output(inputs.front().record.stream)},
    };
  }

  std::string transport_name{"recording-transport"};
  std::string seen_worker;
  std::string seen_processor;
  std::vector<codec::ExtractedRecord> seen_records;
  std::size_t calls{};
};
```

Then prove F.2 composition:

```cpp
TEST(distributed_remote_worker_executes_verified_partition_once) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/valid", 1, "alpha"),
      exact_record("f6/valid", 2, "beta")};
  const auto partition = one_partition(inputs);
  RecordingTransport transport;
  codec::RemoteDistributedWorker worker{
      transport, "remote-a", "processor-a"};

  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_TRUE(result);
  EXPECT_EQ(transport.calls, std::size_t{1});
  EXPECT_EQ(transport.seen_worker, std::string{"remote-a"});
  EXPECT_EQ(transport.seen_processor, std::string{"processor-a"});
  EXPECT_EQ(transport.seen_records.size(), inputs.size());
  EXPECT_EQ(transport.seen_records[0].payload, inputs[0].payload);
  EXPECT_EQ(transport.seen_records[1].payload, inputs[1].payload);
  EXPECT_EQ(result->worker_name, std::string{"remote-a"});
}
```

- [ ] **Step 3: Commit RED**

Commit only CMake plus the failing F.6 test contract:

```text
test: define Stage F.6 remote worker contract
```

- [ ] **Step 4: Open a non-draft PR and verify RED through CI**

Expected failure: GCC/Clang/sanitizer builds reach `tests/test_distributed_remote_worker.cpp` and fail because `DistributedWorkerTransport`, `DistributedRemoteExecutionResponse`, and `RemoteDistributedWorker` are absent. Any unrelated configure/dependency/test failure must be debugged before implementation.

---

### Task 2: Implement the bounded transport adapter

**Files:**
- Modify: `include/codec/distributed.hpp`
- Create: `src/distributed/remote_worker.cpp`
- Modify: `CMakeLists.txt`
- Extend: `tests/test_distributed_remote_worker.cpp`

**Interfaces:**
- Produces:

```cpp
struct DistributedRemoteWorkerLimits {
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_transport_name_bytes{256};
  std::size_t maximum_worker_name_bytes{256};
  std::size_t maximum_processor_name_bytes{256};
  ProcessorRunLimits processor{};
};

struct DistributedRemoteExecutionResponse {
  std::string worker_name;
  std::string processor_name;
  std::vector<ProcessorOutput> outputs;
};

class DistributedWorkerTransport {
 public:
  virtual ~DistributedWorkerTransport() = default;
  virtual std::string name() const = 0;
  virtual Result<DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const ExtractedRecord> inputs) = 0;
};

class RemoteDistributedWorker final : public DistributedWorker {
 public:
  RemoteDistributedWorker(DistributedWorkerTransport& transport,
                          std::string worker_name,
                          std::string processor_name,
                          DistributedRemoteWorkerLimits limits = {});
  std::string name() const override;
  std::string processor_name() const override;
  Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) override;
 private:
  DistributedWorkerTransport* transport_{};
  std::string worker_name_;
  std::string processor_name_;
  DistributedRemoteWorkerLimits limits_{};
};
```

- [ ] **Step 1: Add the additive public types**

Add `#include <string_view>` to `distributed.hpp`. Place F.6 types immediately after `LocalProcessorWorker` / before `DistributedExecutionResult` so worker implementations remain grouped.

- [ ] **Step 2: Add request-limit validation**

In `src/distributed/remote_worker.cpp`, implement a private validator equivalent to:

```cpp
Result<void> validate_remote_limits(const DistributedRemoteWorkerLimits& limits) {
  if (limits.maximum_input_records == 0 ||
      limits.maximum_input_bytes == 0 ||
      limits.maximum_transport_name_bytes == 0 ||
      limits.maximum_worker_name_bytes == 0 ||
      limits.maximum_processor_name_bytes == 0 ||
      limits.processor.maximum_outputs == 0 ||
      limits.processor.maximum_output_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed remote worker limits must be non-zero");
  }
  return {};
}
```

Validate input records before dispatch:

```cpp
if (inputs.empty())
  return fail<std::vector<ProcessorOutput>>(
      ErrorCode::invalid_argument,
      "distributed remote worker requires at least one input");
if (inputs.size() > limits_.maximum_input_records)
  return fail<std::vector<ProcessorOutput>>(
      ErrorCode::resource_exhausted,
      "distributed remote worker input-count limit exceeded");
```

For each input, require `payload.size() == record.payload_size`, accumulate bytes only after checking subtraction form (`payload_size > maximum_input_bytes - total`), and fail before dispatch on mismatch/exhaustion.

- [ ] **Step 3: Validate descriptive labels before dispatch**

Fetch `transport_->name()` inside the bad-allocation boundary. Require transport/worker/processor labels non-empty and no longer than their configured byte limits. Empty labels return `invalid_argument`; oversized labels return `resource_exhausted`.

- [ ] **Step 4: Dispatch exactly once and preserve provider error**

```cpp
auto response = transport_->dispatch(worker_name_, processor_name_, inputs);
if (!response) return response.error();
```

Do not retry regardless of `response.error().retryable`.

- [ ] **Step 5: Validate response identity and output bounds**

Reject either descriptive identity mismatch:

```cpp
if (response->worker_name != worker_name_ ||
    response->processor_name != processor_name_) {
  return fail<std::vector<ProcessorOutput>>(
      ErrorCode::protocol,
      "distributed remote worker response identity mismatch");
}
```

Then bound output count and aggregate `output.payload.size()` against `limits_.processor`, using overflow-safe subtraction. Return the output vector unchanged after successful validation.

- [ ] **Step 6: Catch `std::bad_alloc`**

Wrap `RemoteDistributedWorker::execute()` in `try/catch (const std::bad_alloc&)` and translate to `resource_exhausted`.

- [ ] **Step 7: Add focused GREEN tests**

Add tests for each boundary:

```cpp
TEST(distributed_remote_worker_rejects_request_before_transport_dispatch);
TEST(distributed_remote_worker_preserves_transport_error_without_retry);
TEST(distributed_remote_worker_rejects_response_identity_mismatch);
TEST(distributed_remote_worker_bounds_response_outputs);
TEST(distributed_remote_worker_leaves_processor_semantics_to_f2);
```

For the F.2 semantic test, return one `ProcessorOutput` with `truth = TruthClass::source_exact`; `execute_partition()` must return `invalid_argument` after exactly one transport dispatch.

- [ ] **Step 8: Add F.5 composition test**

Reuse the existing scheduler pattern with a one-record partition batch and a `RemoteDistributedWorker`. Provide a minimal `ObjectStoreBackend`/location index, schedule through `schedule_partitions()`, and assert success plus exactly one transport dispatch. Do not modify `scheduler.cpp`.

- [ ] **Step 9: Register production source and run full GREEN CI**

Add `src/distributed/remote_worker.cpp` adjacent to other distributed sources. Push and require GCC/Clang/sanitizer CI green before moving to package/docs work.

- [ ] **Step 10: Commit implementation**

```text
feat: add bounded remote worker transport
```

---

### Task 3: Prove installed-package F.6 use

**Files:**
- Modify: `tests/package_consumer/main.cpp`

**Interfaces:**
- Consumes only installed `<codec/distributed.hpp>` public API.
- Produces executable proof that a downstream consumer can implement `DistributedWorkerTransport`, instantiate `RemoteDistributedWorker`, and use it through F.2/F.5.

- [ ] **Step 1: Add a package transport**

Near `PackageDistributedProcessor`, add:

```cpp
class PackageWorkerTransport final : public codec::DistributedWorkerTransport {
 public:
  std::string name() const override { return "package-worker-transport"; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    PackageDistributedProcessor processor;
    auto outputs = processor.process(inputs);
    if (!outputs) return outputs.error();
    return codec::DistributedRemoteExecutionResponse{
        .worker_name = std::string{worker_name},
        .processor_name = std::string{processor_name},
        .outputs = std::move(*outputs),
    };
  }

  std::size_t calls{};
};
```

- [ ] **Step 2: Exercise the remote worker through installed API**

After the existing F.2/F.5 distributed checks:

```cpp
PackageWorkerTransport remote_transport;
codec::RemoteDistributedWorker remote_worker{
    remote_transport, "package-remote-worker", "package-distributed"};
auto remote_execution = codec::execute_partition(
    remote_worker, partitions->front(), retrieved->records);
if (!remote_execution || remote_transport.calls != 1 ||
    remote_execution->worker_name != "package-remote-worker" ||
    remote_execution->processor_name != "package-distributed" ||
    remote_execution->outputs.size() != 1) {
  return 1;
}
```

- [ ] **Step 3: Run exact-head CI again**

Both GCC and Clang package-consumer steps must compile/link/run against the installed package. Sanitizers must remain green.

- [ ] **Step 4: Commit package proof**

```text
test: prove installed Stage F.6 transport API
```

---

### Task 4: Synchronize repository truth and merge

**Files:**
- Modify: `AI_WORKSHEET.md`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Documentation must match final tested code exactly.

- [ ] **Step 1: Update `AI_WORKSHEET.md` active work record**

Record base `f98575cb...`, F.6 branch, exact RED head, proof contract, no truth-class changes, and the non-claims from the design spec.

- [ ] **Step 2: Update README implemented/planned lists**

Add an implemented bullet describing the provider-neutral `DistributedWorkerTransport` + `RemoteDistributedWorker` boundary. Remove only the stale statement that no remote-worker transport boundary exists; continue listing concrete network/RPC transport, authentication, discovery/health, retry/failover, persistence, benchmarks, and deployment as planned.

In the Distributed Processing Profile section, add an F.6 paragraph stating that response label matching is descriptive routing evidence rather than authentication/attestation.

- [ ] **Step 3: Update `CHANGELOG.md` Unreleased**

Add one F.6 bullet with the exact implementation and explicit non-claims. Do not claim network availability or remote execution proof beyond a caller-supplied transport contract.

- [ ] **Step 4: Run final exact-head CI**

Require one fresh PR-triggered run on the documentation-inclusive final head:

```yaml
build (gcc): success including configure/build/full tests/install/package consumer
build (clang): success including configure/build/full tests/install/package consumer
sanitizers: success including build/tests
```

- [ ] **Step 5: Diff/code-review audit**

Compare base `f98575cb...` to final head. Expected scope:

```text
AI_WORKSHEET.md
CHANGELOG.md
CMakeLists.txt
README.md
docs/superpowers/plans/2026-08-29-stage-f6-remote-worker-transport.md
docs/superpowers/specs/2026-08-29-stage-f6-remote-worker-transport-design.md
include/codec/distributed.hpp
src/distributed/remote_worker.cpp
tests/package_consumer/main.cpp
tests/test_distributed_remote_worker.cpp
```

No scheduler production change is expected.

- [ ] **Step 6: Merge only the exact green head**

Use squash merge with `expected_head_sha=<final exact green SHA>`. If the PR head moves, rerun the entire final gate before merge.

- [ ] **Step 7: Record completion and fresh dependency audit in roadmap issue #10**

Include RED SHA, final exact-head CI SHA/run, merge SHA, capability delta, explicit non-claims, and a fresh audit of the remaining Stage F dependency graph. Select the next milestone only from merged repository evidence.
