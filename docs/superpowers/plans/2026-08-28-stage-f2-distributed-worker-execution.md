# Stage F.2 Distributed Worker Execution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded one-partition/one-worker execution that proves F.1 exact membership before running one generic processor and returns only existing Stage-C-validated S1/D outputs.

**Architecture:** Extend `codec/distributed.hpp` with a pluggable `DistributedWorker`, synchronous `LocalProcessorWorker`, execution limits/result types, and `execute_partition()`. Share the exact F.1 `CDP1` identity/link implementation privately, verify partition metadata and payload SHA-256 before provider invocation, then route provider outputs through `invoke_processor()`.

**Tech Stack:** C++20, CMake >= 3.20, `codec::Result`, SHA-256, `StreamProcessor`/`invoke_processor`, repository unit-test harness, GitHub Actions GCC/Clang/ASan+UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-f2-distributed-worker-execution-design.md`

## Global Constraints

- Base: merged F.1 `812f7d90ca994efb1dcbd9b03d5e25fe6f4da445`.
- Preserve F.1 `DistributedPartition` fields and exact `CDP1` bytes.
- Preserve CODA bytes, S0/S1/D definitions, provenance encoding, StreamId semantics, Stage E contracts, C ABI, and CLI behavior.
- Execute exactly one partition through exactly one worker per call.
- Verify ordered links, payload sizes, payload SHA-256, partition payload-byte total, and canonical identity before worker invocation.
- Reuse `invoke_processor()` for output truth/process/resource validation.
- No scheduler/thread pool, RPC/socket protocol, processor discovery, worker authentication/attestation, leases/retries/exactly-once semantics, object storage/remote retrieval, indexes, automatic CODA persistence, deployment integration, or performance/scale claim.

---

### Task 1: Establish F.2 work state and RED API proof

**Files:**
- Modify: `AI_WORKSHEET.md`
- Create: `tests/test_distributed_worker.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: F.1 `DistributedPartition`, `partition_exact_records()`, `StreamProcessor`, `ProcessorOutput`, `ProcessorRunLimits`, `sha256()`.
- Produces: wished-for compile-time F.2 public contract.

- [ ] **Step 1: Update the active worksheet record**

Use this exact work record:

```yaml
task: Add bounded one-partition/one-worker execution over F.1 exact partitions while preserving generic processor truth/provenance semantics.
base_ref: origin/main
base_head_sha: 812f7d90ca994efb1dcbd9b03d5e25fe6f4da445
work_branch: automation/stage-f2-distributed-worker-execution
current_version: 0.2.0
active_roadmap_stage: F — F.1 exact-work partitioning is merged; bounded worker execution against exact partition membership is the next unmet dependency.
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: A caller can verify one F.1 partition against exact in-memory record bytes, execute it through one bounded worker, and receive only existing generic-processor-validated S1/D outputs plus descriptive execution metadata.
change_class: generic_stream_abstraction
```

Record:

```text
BEFORE: F.1 defines deterministic one-stream exact-record partitions, while callers have no worker boundary that verifies partition membership before invoking generic processing.
AFTER: execute_partition validates one exact F.1 partition and its record bytes before one worker invocation, then returns only outputs accepted by invoke_processor; LocalProcessorWorker runs a caller-owned StreamProcessor synchronously.
```

- [ ] **Step 2: Create the RED test fixture**

Create `tests/test_distributed_worker.cpp` with these helpers and fixtures:

```cpp
#include "test.hpp"
#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto chars = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(chars);
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "distributed-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 100,
      .details_type = {},
      .details = {},
  };
}

codec::ProcessorOutput valid_output(codec::StreamId stream) {
  return codec::ProcessorOutput{
      .stream = stream,
      .type = 0x7a01,
      .start_ns = 10,
      .end_ns = 20,
      .truth = codec::TruthClass::derived,
      .payload = bytes("derived"),
      .process = process_identity(),
  };
}

codec::DistributedPartition one_partition(
    const std::vector<codec::ExtractedRecord>& inputs) {
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});
  return partitions->front();
}

class CountingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "counting-processor"; }
  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{
        valid_output(inputs.front().record.stream)};
  }
  std::size_t calls{};
};

class CountingWorker final : public codec::DistributedWorker {
 public:
  std::string name() const override { return worker_name; }
  std::string processor_name() const override { return processor_label; }
  codec::Result<std::vector<codec::ProcessorOutput>> execute(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    if (failure.has_value()) return *failure;
    if (!outputs.empty()) return outputs;
    return std::vector<codec::ProcessorOutput>{
        valid_output(inputs.front().record.stream)};
  }
  std::string worker_name{"worker-a"};
  std::string processor_label{"processor-a"};
  std::vector<codec::ProcessorOutput> outputs;
  std::optional<codec::Error> failure;
  std::size_t calls{};
};

}  // namespace
```

- [ ] **Step 3: Add the successful local-worker case**

```cpp
TEST(distributed_worker_executes_one_verified_partition) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/valid", 1, "alpha"),
      exact_record("f2/valid", 2, "beta")};
  const auto partition = one_partition(inputs);
  CountingProcessor processor;
  codec::LocalProcessorWorker worker{processor, "local-worker"};
  auto result = codec::execute_partition(worker, partition, inputs);

  EXPECT_TRUE(result);
  EXPECT_EQ(processor.calls, std::size_t{1});
  EXPECT_EQ(result->partition_identity, partition.identity);
  EXPECT_EQ(result->stream, partition.stream);
  EXPECT_EQ(result->worker_name, std::string{"local-worker"});
  EXPECT_EQ(result->processor_name, std::string{"counting-processor"});
  EXPECT_EQ(result->outputs.size(), std::size_t{1});
  EXPECT_EQ(result->outputs.front().truth, codec::TruthClass::derived);
}
```

- [ ] **Step 4: Add all fail-before-worker cases**

Add separate tests named exactly:

```text
distributed_worker_rejects_empty_partition_or_inputs
distributed_worker_rejects_tampered_partition_before_execution
distributed_worker_rejects_wrong_or_reordered_inputs_before_execution
distributed_worker_verifies_exact_payload_integrity_before_execution
distributed_worker_validates_limits_and_labels_before_execution
```

Use these concrete mutations and expected results:

```cpp
// empty partition / empty inputs
codec::DistributedPartition empty_partition;
EXPECT_EQ(codec::execute_partition(worker, empty_partition, inputs).error().code,
          codec::ErrorCode::invalid_argument);
EXPECT_EQ(codec::execute_partition(worker, partition,
          std::span<const codec::ExtractedRecord>{}).error().code,
          codec::ErrorCode::invalid_argument);

// partition tampering: each must fail invalid_argument with worker.calls == 0
wrong.identity[0] ^= 0x01U;
++wrong.payload_bytes;
++wrong.records.front().sequence;
wrong.records.front().stream = codec::derive_stream_id("f2/other");

// exact-input binding: each must fail invalid_argument with worker.calls == 0
std::reverse(reordered.begin(), reordered.end());
missing.pop_back();
extra.push_back(exact_record("f2/order", 3, "three"));
wrong_stream.front().record.stream = codec::derive_stream_id("f2/other");

// payload integrity
corrupt.front().payload.front() ^= std::byte{0x01};
EXPECT_EQ(codec::execute_partition(worker, partition, corrupt).error().code,
          codec::ErrorCode::archive_corrupt);
++wrong_size.front().record.payload_size;
EXPECT_EQ(codec::execute_partition(worker, partition, wrong_size).error().code,
          codec::ErrorCode::invalid_argument);

// zero/exceeded limits and empty/overlong names; all before worker invocation
zero.maximum_input_records = 0;                      // invalid_argument
zero_processor.processor.maximum_outputs = 0;        // invalid_argument
count.maximum_input_records = 1;                     // resource_exhausted for 2 inputs
bytes_limit.maximum_input_bytes = 3;                 // resource_exhausted for 4-byte input
worker.worker_name.clear();                           // invalid_argument
worker.processor_label.clear();                       // invalid_argument
worker.worker_name = "worker-name-too-long";         // resource_exhausted at name limit 4
worker.processor_label = "processor-name-too-long";  // resource_exhausted at name limit 4
```

Every case must assert `worker.calls == 0`.

- [ ] **Step 5: Add provider-error and full generic-output-validation cases**

Add:

```cpp
TEST(distributed_worker_propagates_one_provider_error_without_retry) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/failure", 1, "input")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;
  worker.failure = codec::Error{codec::ErrorCode::inference,
                                "worker processor unavailable", true};
  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::inference);
  EXPECT_EQ(result.error().message,
            std::string{"worker processor unavailable"});
  EXPECT_TRUE(result.error().retryable);
  EXPECT_EQ(worker.calls, std::size_t{1});
}
```

For `distributed_worker_reuses_generic_processor_output_validation`, invoke a fresh `CountingWorker` for each of these concrete outputs/limits and assert the existing Stage-C error class:

```cpp
source_truth.truth = codec::TruthClass::source_exact;  // invalid_argument
inverted.start_ns = 21; inverted.end_ns = 20;         // invalid_argument
reserved.type = codec::record_type_code(
    codec::RecordType::stream_provenance);             // invalid_argument
bad_process.process.operation.clear();                 // invalid_argument
count_limit.processor.maximum_outputs = 1;             // 2 outputs -> resource_exhausted
byte_limit.processor.maximum_output_bytes = 1;         // "derived" -> resource_exhausted
```

Each worker must have exactly one call: output rejection happens after provider execution and must not retry.

- [ ] **Step 6: Register only the RED test source**

Add `tests/test_distributed_worker.cpp` to `codec_tests` in `CMakeLists.txt`. Do not add an F.2 implementation source yet.

- [ ] **Step 7: Commit and prove RED**

```bash
git add AI_WORKSHEET.md CMakeLists.txt tests/test_distributed_worker.cpp
git commit -m "test: define Stage F.2 worker execution contract"
```

Run/require normal CI on this exact head. Valid RED means GCC, Clang, and sanitizer builds fail because the new F.2 worker types/functions are absent. Configuration errors or unrelated test failures do not satisfy the RED gate.

---

### Task 2: Implement private CDP1 reuse and bounded worker execution

**Files:**
- Modify: `include/codec/distributed.hpp`
- Create: `src/distributed/internal.hpp`
- Create: `src/distributed/identity.cpp`
- Modify: `src/distributed/partition.cpp`
- Create: `src/distributed/worker.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: exact F.1 `CDP1` membership semantics and Stage-C `invoke_processor()`.
- Produces: installed public F.2 execution API.

- [ ] **Step 1: Add the public API**

Append exactly these types to `include/codec/distributed.hpp` after including `<codec/processing.hpp>` and `<string>`:

```cpp
struct DistributedExecutionLimits {
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_worker_name_bytes{256};
  std::size_t maximum_processor_name_bytes{256};
  ProcessorRunLimits processor{};
};

class DistributedWorker {
 public:
  virtual ~DistributedWorker() = default;
  virtual std::string name() const = 0;
  virtual std::string processor_name() const = 0;
  virtual Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) = 0;
};

class LocalProcessorWorker final : public DistributedWorker {
 public:
  LocalProcessorWorker(StreamProcessor& processor, std::string worker_name);
  std::string name() const override;
  std::string processor_name() const override;
  Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) override;
 private:
  StreamProcessor* processor_{};
  std::string worker_name_;
};

struct DistributedExecutionResult {
  Sha256 partition_identity{};
  StreamId stream{};
  std::string worker_name;
  std::string processor_name;
  std::vector<ProcessorOutput> outputs;
};

Result<DistributedExecutionResult> execute_partition(
    DistributedWorker& worker,
    const DistributedPartition& partition,
    std::span<const ExtractedRecord> inputs,
    DistributedExecutionLimits limits = {});
```

- [ ] **Step 2: Share the exact existing F.1 membership helpers privately**

Create `src/distributed/internal.hpp`:

```cpp
#pragma once
#include <codec/archive.hpp>
#include <codec/integrity.hpp>
#include <span>

namespace codec::detail {
ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input);
Sha256 distributed_partition_identity(
    StreamId stream,
    std::span<const ProvenanceRecordLink> records);
}  // namespace codec::detail
```

Create `src/distributed/identity.cpp` by moving the current F.1 `append_u16`, `append_u64`, exact-link construction, and exact `CDP1` byte encoding out of `partition.cpp`. The identity function must still encode, in order: ASCII `CDP1`, 16 raw StreamId bytes, u64-le record count, then u16-le type + u64-le sequence + 32-byte hash for every ordered link.

Modify `partition.cpp` only to call:

```cpp
target.records.push_back(detail::distributed_exact_link(input));
partition.identity = detail::distributed_partition_identity(
    partition.stream, partition.records);
```

No F.1 validation or partition-order change is permitted.

- [ ] **Step 3: Implement `LocalProcessorWorker` and worker-output proxy**

Create `src/distributed/worker.cpp` with:

```cpp
#include <codec/distributed.hpp>
#include "internal.hpp"
#include <new>
#include <string>
#include <utility>

namespace codec {
namespace {

bool same_link(const ProvenanceRecordLink& a, const ProvenanceRecordLink& b) {
  return a.stream == b.stream && a.type == b.type &&
         a.sequence == b.sequence && a.hash == b.hash;
}

class WorkerProcessorProxy final : public StreamProcessor {
 public:
  WorkerProcessorProxy(DistributedWorker& worker, std::string processor_name)
      : worker_(worker), processor_name_(std::move(processor_name)) {}
  std::string name() const override { return processor_name_; }
  Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) override {
    return worker_.execute(inputs);
  }
 private:
  DistributedWorker& worker_;
  std::string processor_name_;
};

Result<void> validate_execution_limits(const DistributedExecutionLimits& limits) {
  if (limits.maximum_input_records == 0 || limits.maximum_input_bytes == 0 ||
      limits.maximum_worker_name_bytes == 0 ||
      limits.maximum_processor_name_bytes == 0 ||
      limits.processor.maximum_outputs == 0 ||
      limits.processor.maximum_output_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed execution limits must be non-zero");
  }
  return {};
}

}  // namespace

LocalProcessorWorker::LocalProcessorWorker(StreamProcessor& processor,
                                           std::string worker_name)
    : processor_(&processor), worker_name_(std::move(worker_name)) {}
std::string LocalProcessorWorker::name() const { return worker_name_; }
std::string LocalProcessorWorker::processor_name() const {
  return processor_->name();
}
Result<std::vector<ProcessorOutput>> LocalProcessorWorker::execute(
    std::span<const ExtractedRecord> inputs) {
  return processor_->process(inputs);
}
```

- [ ] **Step 4: Implement `execute_partition()` in this exact order**

1. Validate all six non-zero execution/output limits.
2. Reject empty partition/input or record-count mismatch as `invalid_argument`.
3. Reject input count over `maximum_input_records` as `resource_exhausted`.
4. Read worker and processor names once; reject empty as `invalid_argument`, over limit as `resource_exhausted`.
5. Require every partition link stream to equal `partition.stream`.
6. Recompute private `CDP1`; identity mismatch is `invalid_argument`. Catch only `std::bad_alloc` from this F.2-owned identity recomputation and return `resource_exhausted`.
7. For each ordered input: size must equal `RecordInfo::payload_size`; stream must equal partition stream; exact link must equal the link at the same index; `sha256(payload)` must equal `RecordInfo::hash`; overflow-safe aggregate bytes must stay within `maximum_input_bytes`.
8. Require aggregate bytes to equal `partition.payload_bytes`.
9. Construct `WorkerProcessorProxy` and call `invoke_processor(proxy, inputs, limits.processor)` exactly once.
10. Propagate any worker or processor-validation `Error` unchanged and do not retry.
11. On success return verified partition identity/stream, the captured labels, and moved validated outputs.

Use these exact error messages or equally specific wording; error codes are normative:

```text
distributed execution limits must be non-zero
distributed execution requires one non-empty exact partition input set
distributed execution input-count limit exceeded
distributed worker and processor names must be non-empty
distributed worker or processor name exceeds the configured limit
distributed partition contains a link from another stream
distributed partition identity does not match ordered membership
distributed execution input payload size is inconsistent
distributed execution input belongs to another stream
distributed execution input does not match ordered partition membership
distributed execution input payload hash mismatch
distributed execution input-byte limit exceeded
distributed partition payload-byte total is inconsistent
```

- [ ] **Step 5: Register implementation sources**

Add to `codec_core`:

```cmake
  src/distributed/identity.cpp
  src/distributed/partition.cpp
  src/distributed/worker.cpp
```

- [ ] **Step 6: Verify GREEN locally/CI-equivalent**

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/codec_tests --include-prefix distributed_
ctest --test-dir build --output-on-failure

cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

Expected: all distributed tests and every existing CTest target pass; sanitizer run has no new ASan/UBSan finding.

- [ ] **Step 7: Commit GREEN**

```bash
git add include/codec/distributed.hpp src/distributed/internal.hpp \
  src/distributed/identity.cpp src/distributed/partition.cpp \
  src/distributed/worker.cpp CMakeLists.txt tests/test_distributed_worker.cpp
git commit -m "feat: add bounded distributed worker execution"
```

---

### Task 3: Installed-package proof, truthful status, and exact-head merge

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: installed F.1+F.2 API.
- Produces: external consumer proof and accurate current/non-capability documentation.

- [ ] **Step 1: Exercise F.2 from the installed package**

Add a `PackageDistributedProcessor : codec::StreamProcessor` that returns one D output with non-empty valid `ProvenanceProcess`. Reuse the package consumer's existing `partition_inputs`/`partitions`, then execute:

```cpp
PackageDistributedProcessor processor;
codec::LocalProcessorWorker worker{processor, "package-local-worker"};
auto execution = codec::execute_partition(
    worker, partitions->front(), partition_inputs);
if (!execution || execution->partition_identity != partitions->front().identity ||
    execution->worker_name != "package-local-worker" ||
    execution->processor_name != "package-distributed" ||
    execution->outputs.size() != 1 ||
    execution->outputs.front().truth != codec::TruthClass::derived) {
  return 1;
}
```

Use direct includes for every public type the consumer names; do not rely on source-tree headers.

- [ ] **Step 2: Update README current truth**

Add one implemented generic bullet and one short F.2 paragraph stating:

```text
F.2 provides bounded synchronous one-partition/one-worker execution after exact F.1
membership and payload-integrity validation. LocalProcessorWorker delegates to one
caller-owned StreamProcessor; execute_partition validates worker output through the
existing invoke_processor contract. Worker/processor names are descriptive only.
```

Keep these items explicitly unimplemented:

```text
multi-partition scheduling/worker pools, RPC/network execution, processor discovery/distribution,
worker authentication/attestation, leases/retries/exactly-once semantics,
object-store backends/remote retrieval, distributed indexes/global locations,
automatic distributed persistence, operational benchmarks, deployment integrations
```

- [ ] **Step 3: Update CHANGELOG Unreleased only**

Add one F.2 bullet covering exact partition/input verification, synchronous local worker execution, reuse of `invoke_processor`, installed-package proof, and all non-claims above. Do not modify historical release entries.

- [ ] **Step 4: Require final exact-head CI**

The final PR head must complete successfully in:

```text
build (gcc): configure, build, tests, install, installed-package consumer
build (clang): configure, build, tests, install, installed-package consumer
sanitizers: configure, build, tests
```

- [ ] **Step 5: Audit the final diff**

Only these paths may differ from the F.1 merge base:

```text
AI_WORKSHEET.md
CHANGELOG.md
CMakeLists.txt
README.md
docs/superpowers/plans/2026-08-28-stage-f2-distributed-worker-execution.md
docs/superpowers/specs/2026-08-28-stage-f2-distributed-worker-execution-design.md
include/codec/distributed.hpp
src/distributed/internal.hpp
src/distributed/identity.cpp
src/distributed/partition.cpp
src/distributed/worker.cpp
tests/package_consumer/main.cpp
tests/test_distributed_worker.cpp
```

- [ ] **Step 6: Record concrete proof values and merge**

Copy the actual RED head SHA/run number, final green head SHA/run number, PR number, and squash-merge SHA returned by GitHub into the PR discussion and roadmap issue #10. Do not use inferred or earlier-run values.

The capability statement must be:

```text
F.1 partitions can now be verified against exact payload bytes and executed once through
a bounded worker boundary with a concrete synchronous LocalProcessorWorker; no remote
execution, scheduling pool, persistence, retry/exactly-once, trust/attestation, or scale claim.
```

Merge only when the PR is mergeable, its head SHA is unchanged, and that exact SHA has completed-success CI in all required jobs. If the head moves, verify the new exact head again.

After merge, audit Stage F from merged evidence. Object-store addressing/backends plus remote exact-record retrieval are the expected next dependency because a genuinely remote worker needs a location/retrieval layer; do not freeze the next API before that audit.
