# Stage F.2 Distributed Worker Execution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded one-partition/one-worker execution boundary that proves F.1 exact membership before running one generic processor and returns only Stage-C-validated S1/D outputs.

**Architecture:** Extend the existing distributed module with a pluggable `DistributedWorker` interface and concrete synchronous `LocalProcessorWorker`. `execute_partition()` validates the F.1 `CDP1` descriptor and exact input bytes before invoking the worker, then routes worker outputs through `invoke_processor()` so F.2 does not redefine generic processing truth/provenance/resource rules.

**Tech Stack:** C++20, CMake >= 3.20, existing `codec::Result`, SHA-256 integrity helper, `StreamProcessor`/`invoke_processor`, repository unit-test harness, GitHub Actions GCC/Clang/ASan+UBSan matrix.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-f2-distributed-worker-execution-design.md`

## Global Constraints

- Base is merged F.1 `812f7d90ca994efb1dcbd9b03d5e25fe6f4da445`.
- Preserve F.1 `DistributedPartition` fields and exact `CDP1` bytes.
- Preserve CODA bytes, S0/S1/D definitions, provenance encoding, StreamId semantics, Stage E contracts, C ABI, and CLI behavior.
- Execute exactly one partition through exactly one worker per `execute_partition()` call.
- Verify exact ordered record links, payload sizes, payload SHA-256, partition payload bytes, and canonical partition identity before worker invocation.
- Reuse `invoke_processor()` for worker output validation.
- No scheduler/thread pool, RPC/socket protocol, service discovery, worker trust/attestation, leases/retries/exactly-once semantics, object storage/remote retrieval, indexes, automatic CODA persistence, deployment integration, or scale/performance claim.
- Worker and processor labels are descriptive only and must remain bounded.
- F.2 allocation/resource failures return existing `ErrorCode` values; arbitrary provider exceptions are not reinterpreted.

---

### Task 1: Record F.2 work state and establish the RED worker contract

**Files:**
- Modify: `AI_WORKSHEET.md`
- Create: `tests/test_distributed_worker.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: merged F.1 `DistributedPartition`, `partition_exact_records()`, `StreamProcessor`, `ProcessorOutput`, `ProcessorRunLimits`, `sha256()`.
- Produces: compile-time wished-for F.2 API contract: `DistributedExecutionLimits`, `DistributedWorker`, `LocalProcessorWorker`, `DistributedExecutionResult`, `execute_partition()`.

- [ ] **Step 1: Replace the active worksheet record with F.2**

Set the active record to:

```yaml
task: Add bounded one-partition/one-worker execution over F.1 exact partitions while preserving generic processor truth/provenance semantics.
base_ref: origin/main
base_head_sha: 812f7d90ca994efb1dcbd9b03d5e25fe6f4da445
work_branch: automation/stage-f2-distributed-worker-execution
current_version: 0.2.0
active_roadmap_stage: F — deterministic F.1 exact-work partitioning is merged; the next unmet dependency is bounded worker execution against exact partition membership.
continuity_evidence:
  - git_head: main at 812f7d90ca994efb1dcbd9b03d5e25fe6f4da445
  - open_prs: preserve unrelated work; F.2 uses its own branch/PR
  - exact_head_ci: F.1 final head 83d408f5b2e5be74482f586313feebe3c99cce79 passed CI 222 before merge
  - roadmap_issue: issue 10 records F.1 complete and F.2 worker execution next
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
AFTER: execute_partition validates one exact F.1 partition and its record bytes before one worker invocation, then returns only outputs accepted by invoke_processor; the concrete LocalProcessorWorker runs a caller-owned StreamProcessor synchronously.
```

Use this proof block:

```yaml
proof:
  regression_test: tests/test_distributed_worker.cpp plus all existing tests
  exactness_test: partition stream/ordered links/payload bytes/CDP1 identity and each supplied payload SHA-256 must match before worker invocation
  compatibility_test: F.1 CDP1 membership identity, CODA, S0/S1/D, provenance, Stage E, C ABI, CLI, and installed package behavior remain compatible
  failure_path_test: malformed/tampered partition or exact inputs, zero/exceeded bounds, worker errors, and invalid worker outputs fail closed with no retry or partial result
  security_test: worker/processor names and partition SHA-256 remain descriptive/integrity metadata only; no authentication, attestation, authorization, or remote trust claim
  benchmark: n/a — no throughput, latency, scale, fault-tolerance, or cost claim
```

- [ ] **Step 2: Add the wished-for test helpers and valid processor/worker fixtures**

Create `tests/test_distributed_worker.cpp` beginning with:

```cpp
#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto characters = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(characters);
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
    return std::vector<codec::ProcessorOutput>{valid_output(inputs.front().record.stream)};
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
    return std::vector<codec::ProcessorOutput>{valid_output(inputs.front().record.stream)};
  }

  std::string worker_name{"worker-a"};
  std::string processor_label{"processor-a"};
  std::vector<codec::ProcessorOutput> outputs;
  std::optional<codec::Error> failure;
  std::size_t calls{};
};

}  // namespace
```

- [ ] **Step 3: Add the successful local-worker execution proof**

Append:

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
  EXPECT_EQ(result->outputs.front().payload, bytes("derived"));
}
```

- [ ] **Step 4: Add fail-before-worker exact-membership tests**

Append:

```cpp
TEST(distributed_worker_rejects_tampered_partition_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/tamper", 1, "one"),
      exact_record("f2/tamper", 2, "two")};
  const auto original = one_partition(inputs);
  CountingWorker worker;

  auto wrong_identity = original;
  wrong_identity.identity[0] ^= 0x01U;
  auto identity = codec::execute_partition(worker, wrong_identity, inputs);
  EXPECT_FALSE(identity);
  EXPECT_EQ(identity.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_bytes = original;
  ++wrong_bytes.payload_bytes;
  auto payload_bytes = codec::execute_partition(worker, wrong_bytes, inputs);
  EXPECT_FALSE(payload_bytes);
  EXPECT_EQ(payload_bytes.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_link = original;
  ++wrong_link.records.front().sequence;
  auto link = codec::execute_partition(worker, wrong_link, inputs);
  EXPECT_FALSE(link);
  EXPECT_EQ(link.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_stream = original;
  wrong_stream.records.front().stream = codec::derive_stream_id("other");
  auto stream = codec::execute_partition(worker, wrong_stream, inputs);
  EXPECT_FALSE(stream);
  EXPECT_EQ(stream.error().code, codec::ErrorCode::invalid_argument);

  EXPECT_EQ(worker.calls, std::size_t{0});
}

TEST(distributed_worker_rejects_wrong_or_reordered_inputs_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/order", 1, "one"),
      exact_record("f2/order", 2, "two")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  auto reversed = inputs;
  std::reverse(reversed.begin(), reversed.end());
  auto order = codec::execute_partition(worker, partition, reversed);
  EXPECT_FALSE(order);
  EXPECT_EQ(order.error().code, codec::ErrorCode::invalid_argument);

  const std::vector<codec::ExtractedRecord> missing{inputs.front()};
  auto missing_result = codec::execute_partition(worker, partition, missing);
  EXPECT_FALSE(missing_result);
  EXPECT_EQ(missing_result.error().code, codec::ErrorCode::invalid_argument);

  auto extra = inputs;
  extra.push_back(exact_record("f2/order", 3, "three"));
  auto extra_result = codec::execute_partition(worker, partition, extra);
  EXPECT_FALSE(extra_result);
  EXPECT_EQ(extra_result.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_stream = inputs;
  wrong_stream.front().record.stream = codec::derive_stream_id("f2/other");
  auto stream_result = codec::execute_partition(worker, partition, wrong_stream);
  EXPECT_FALSE(stream_result);
  EXPECT_EQ(stream_result.error().code, codec::ErrorCode::invalid_argument);

  EXPECT_EQ(worker.calls, std::size_t{0});
}

TEST(distributed_worker_verifies_exact_payload_integrity_before_execution) {
  std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/hash", 1, "one")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  inputs.front().payload.front() ^= std::byte{0x01};
  auto hash = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(hash);
  EXPECT_EQ(hash.error().code, codec::ErrorCode::archive_corrupt);
  EXPECT_EQ(worker.calls, std::size_t{0});

  inputs = {exact_record("f2/hash", 1, "one")};
  ++inputs.front().record.payload_size;
  auto size = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(size);
  EXPECT_EQ(size.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_EQ(worker.calls, std::size_t{0});
}
```

- [ ] **Step 5: Add limits, worker-error, and output-validation proofs**

Append:

```cpp
TEST(distributed_worker_validates_limits_and_labels_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/limits", 1, "abcd")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  codec::DistributedExecutionLimits zero;
  zero.maximum_input_records = 0;
  auto zero_result = codec::execute_partition(worker, partition, inputs, zero);
  EXPECT_FALSE(zero_result);
  EXPECT_EQ(zero_result.error().code, codec::ErrorCode::invalid_argument);

  codec::DistributedExecutionLimits count;
  count.maximum_input_records = 0 + 1;
  const std::vector<codec::ExtractedRecord> two_inputs{
      exact_record("f2/limits-two", 1, "a"),
      exact_record("f2/limits-two", 2, "b")};
  const auto two_partition = one_partition(two_inputs);
  auto count_result = codec::execute_partition(worker, two_partition, two_inputs, count);
  EXPECT_FALSE(count_result);
  EXPECT_EQ(count_result.error().code, codec::ErrorCode::resource_exhausted);

  codec::DistributedExecutionLimits bytes_limit;
  bytes_limit.maximum_input_bytes = 3;
  auto bytes_result = codec::execute_partition(worker, partition, inputs, bytes_limit);
  EXPECT_FALSE(bytes_result);
  EXPECT_EQ(bytes_result.error().code, codec::ErrorCode::resource_exhausted);

  worker.worker_name = "worker-name-too-long";
  codec::DistributedExecutionLimits worker_name_limit;
  worker_name_limit.maximum_worker_name_bytes = 4;
  auto worker_name = codec::execute_partition(
      worker, partition, inputs, worker_name_limit);
  EXPECT_FALSE(worker_name);
  EXPECT_EQ(worker_name.error().code, codec::ErrorCode::resource_exhausted);

  worker.worker_name = "worker-a";
  worker.processor_label = "processor-name-too-long";
  codec::DistributedExecutionLimits processor_name_limit;
  processor_name_limit.maximum_processor_name_bytes = 4;
  auto processor_name = codec::execute_partition(
      worker, partition, inputs, processor_name_limit);
  EXPECT_FALSE(processor_name);
  EXPECT_EQ(processor_name.error().code, codec::ErrorCode::resource_exhausted);

  EXPECT_EQ(worker.calls, std::size_t{0});
}

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

TEST(distributed_worker_reuses_generic_processor_output_validation) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/output", 1, "input")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  auto invalid_truth = valid_output(partition.stream);
  invalid_truth.truth = codec::TruthClass::source_exact;
  worker.outputs = {invalid_truth};
  auto truth = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(truth);
  EXPECT_EQ(truth.error().code, codec::ErrorCode::invalid_argument);

  worker.outputs = {valid_output(partition.stream), valid_output(partition.stream)};
  codec::DistributedExecutionLimits count_limit;
  count_limit.processor.maximum_outputs = 1;
  auto count = codec::execute_partition(worker, partition, inputs, count_limit);
  EXPECT_FALSE(count);
  EXPECT_EQ(count.error().code, codec::ErrorCode::resource_exhausted);

  worker.outputs = {valid_output(partition.stream)};
  codec::DistributedExecutionLimits byte_limit;
  byte_limit.processor.maximum_output_bytes = 1;
  auto output_bytes = codec::execute_partition(worker, partition, inputs, byte_limit);
  EXPECT_FALSE(output_bytes);
  EXPECT_EQ(output_bytes.error().code, codec::ErrorCode::resource_exhausted);

  EXPECT_EQ(worker.calls, std::size_t{3});
}
```

- [ ] **Step 6: Register the test source only**

Add `tests/test_distributed_worker.cpp` to the `codec_tests` source list in `CMakeLists.txt`. Do not add `worker.cpp` or any F.2 public API yet.

- [ ] **Step 7: Commit the RED contract**

```bash
git add AI_WORKSHEET.md CMakeLists.txt tests/test_distributed_worker.cpp
git commit -m "test: define Stage F.2 worker execution contract"
```

- [ ] **Step 8: Verify RED for the intended reason**

Run or require the normal CI build on this exact head.

Expected: GCC, Clang, and sanitizer builds fail compiling `tests/test_distributed_worker.cpp` because `DistributedWorker`, `LocalProcessorWorker`, `DistributedExecutionLimits`, or `execute_partition` do not yet exist. Configuration or unrelated legacy-test failure does not count as valid RED evidence.

---

### Task 2: Implement shared F.1 identity helpers and F.2 worker execution

**Files:**
- Modify: `include/codec/distributed.hpp`
- Create: `src/distributed/internal.hpp`
- Create: `src/distributed/identity.cpp`
- Modify: `src/distributed/partition.cpp`
- Create: `src/distributed/worker.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_distributed_worker.cpp`
- Regression: `tests/test_distributed_partition.cpp`, `tests/test_processing.cpp`

**Interfaces:**
- Consumes: exact F.1 `CDP1` membership definition and Stage C `invoke_processor()`.
- Produces: installed public F.2 worker API and a synchronous local processor worker.

- [ ] **Step 1: Add the public F.2 types exactly as specified**

In `include/codec/distributed.hpp`, include `<codec/processing.hpp>` and `<string>`, retain the F.1 declarations unchanged, and append:

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

- [ ] **Step 2: Extract the existing exact-link/CDP1 implementation into private helpers**

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

Create `src/distributed/identity.cpp` by moving the exact existing F.1 canonical encoding out of `partition.cpp` without changing its bytes:

```cpp
#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace codec::detail {
namespace {

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xffU));
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

}  // namespace

ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input) {
  return ProvenanceRecordLink{
      .stream = input.record.stream,
      .type = input.record.type_code(),
      .sequence = input.record.sequence,
      .hash = input.record.hash,
  };
}

Sha256 distributed_partition_identity(
    StreamId stream,
    std::span<const ProvenanceRecordLink> records) {
  std::vector<std::byte> encoded;
  constexpr std::string_view domain{"CDP1"};
  for (const auto character : domain) {
    encoded.push_back(static_cast<std::byte>(
        static_cast<unsigned char>(character)));
  }
  for (const auto byte : stream.bytes) {
    encoded.push_back(static_cast<std::byte>(byte));
  }
  append_u64(encoded, static_cast<std::uint64_t>(records.size()));
  for (const auto& record : records) {
    append_u16(encoded, record.type);
    append_u64(encoded, record.sequence);
    for (const auto byte : record.hash) {
      encoded.push_back(static_cast<std::byte>(byte));
    }
  }
  return sha256(encoded);
}

}  // namespace codec::detail
```

In `src/distributed/partition.cpp`, include `internal.hpp`, remove its anonymous `append_u16`, `append_u64`, `exact_link`, and `partition_identity` functions, replace `exact_link(input)` with `detail::distributed_exact_link(input)`, and replace `partition_identity(partition)` with:

```cpp
partition.identity = detail::distributed_partition_identity(
    partition.stream, partition.records);
```

Do not alter F.1 validation, partition creation order, limits, or output fields.

- [ ] **Step 3: Implement the synchronous local worker**

Create `src/distributed/worker.cpp` with:

```cpp
#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace codec {
namespace {

bool same_link(const ProvenanceRecordLink& lhs,
               const ProvenanceRecordLink& rhs) {
  return lhs.stream == rhs.stream && lhs.type == rhs.type &&
         lhs.sequence == rhs.sequence && lhs.hash == rhs.hash;
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

- [ ] **Step 4: Implement exact partition/input validation before provider execution**

Continue `src/distributed/worker.cpp` with this `execute_partition()` structure:

```cpp
Result<DistributedExecutionResult> execute_partition(
    DistributedWorker& worker,
    const DistributedPartition& partition,
    std::span<const ExtractedRecord> inputs,
    DistributedExecutionLimits limits) {
  auto valid_limits = validate_execution_limits(limits);
  if (!valid_limits) return valid_limits.error();

  if (partition.records.empty() || inputs.empty() ||
      partition.records.size() != inputs.size()) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed execution requires one non-empty exact partition input set");
  }
  if (inputs.size() > limits.maximum_input_records) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed execution input-count limit exceeded");
  }

  std::string worker_name = worker.name();
  std::string processor_name = worker.processor_name();
  if (worker_name.empty() || processor_name.empty()) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed worker and processor names must be non-empty");
  }
  if (worker_name.size() > limits.maximum_worker_name_bytes ||
      processor_name.size() > limits.maximum_processor_name_bytes) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed worker or processor name exceeds the configured limit");
  }

  for (const auto& link : partition.records) {
    if (link.stream != partition.stream) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed partition contains a link from another stream");
    }
  }

  try {
    if (detail::distributed_partition_identity(partition.stream,
                                               partition.records) !=
        partition.identity) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed partition identity does not match ordered membership");
    }
  } catch (const std::bad_alloc&) {
    return fail<DistributedExecutionResult>(
        ErrorCode::resource_exhausted,
        "distributed partition identity allocation failed");
  }

  std::uint64_t total_input_bytes = 0;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const auto& input = inputs[index];
    if (input.payload.size() != input.record.payload_size) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input payload size is inconsistent");
    }
    if (input.record.stream != partition.stream) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input belongs to another stream");
    }
    if (!same_link(detail::distributed_exact_link(input),
                   partition.records[index])) {
      return fail<DistributedExecutionResult>(
          ErrorCode::invalid_argument,
          "distributed execution input does not match ordered partition membership");
    }
    if (sha256(input.payload) != input.record.hash) {
      return fail<DistributedExecutionResult>(
          ErrorCode::archive_corrupt,
          "distributed execution input payload hash mismatch");
    }

    const auto input_bytes = static_cast<std::uint64_t>(input.payload.size());
    if (input_bytes > limits.maximum_input_bytes - total_input_bytes) {
      return fail<DistributedExecutionResult>(
          ErrorCode::resource_exhausted,
          "distributed execution input-byte limit exceeded");
    }
    total_input_bytes += input_bytes;
  }

  if (total_input_bytes != partition.payload_bytes) {
    return fail<DistributedExecutionResult>(
        ErrorCode::invalid_argument,
        "distributed partition payload-byte total is inconsistent");
  }

  WorkerProcessorProxy proxy{worker, processor_name};
  auto outputs = invoke_processor(proxy, inputs, limits.processor);
  if (!outputs) return outputs.error();

  return DistributedExecutionResult{
      .partition_identity = partition.identity,
      .stream = partition.stream,
      .worker_name = std::move(worker_name),
      .processor_name = std::move(processor_name),
      .outputs = std::move(*outputs),
  };
}

}  // namespace codec
```

Do not catch arbitrary worker/processor exceptions and do not retry a failed worker call.

- [ ] **Step 5: Register the new distributed implementation sources**

Add both sources to `codec_core` in `CMakeLists.txt` immediately around the existing distributed source:

```cmake
  src/distributed/identity.cpp
  src/distributed/partition.cpp
  src/distributed/worker.cpp
```

- [ ] **Step 6: Run targeted tests and fix only F.2 defects**

Run:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/codec_tests --include-prefix distributed_
```

Expected: every `distributed_partition_` and `distributed_worker_` case passes with zero failures.

- [ ] **Step 7: Run the full release suite**

```bash
ctest --test-dir build --output-on-failure
```

Expected: all existing CTest targets pass, including `codec-ai-contract`, C API, and CLI integration.

- [ ] **Step 8: Run sanitizers**

```bash
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

Expected: sanitizer build and tests pass with no new ASan/UBSan finding.

- [ ] **Step 9: Commit the GREEN worker implementation**

```bash
git add include/codec/distributed.hpp src/distributed/internal.hpp \
  src/distributed/identity.cpp src/distributed/partition.cpp \
  src/distributed/worker.cpp CMakeLists.txt tests/test_distributed_worker.cpp
git commit -m "feat: add bounded distributed worker execution"
```

---

### Task 3: Prove installed-package use and publish truthful F.2 status

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md` only if final proof evidence is recorded there before merge

**Interfaces:**
- Consumes: installed `<codec/distributed.hpp>` F.1 + F.2 API.
- Produces: external package proof and accurate implementation/non-capability documentation.

- [ ] **Step 1: Extend the installed-package consumer through real F.2 execution**

In `tests/package_consumer/main.cpp`, reuse the existing F.1 `partition_inputs` and `partitions` setup. Add a small local processor near the file's helper declarations:

```cpp
class PackageDistributedProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "package-distributed"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    codec::ProvenanceProcess process{
        .operation = "package-distributed",
        .implementation_id = "package-consumer",
        .implementation_version = "1",
        .implementation_hash = std::nullopt,
        .configuration_hash = std::nullopt,
        .created_utc_ns = 1,
        .details_type = {},
        .details = {},
    };
    return std::vector<codec::ProcessorOutput>{codec::ProcessorOutput{
        .stream = inputs.front().record.stream,
        .type = 0x7a10,
        .start_ns = 0,
        .end_ns = 1,
        .truth = codec::TruthClass::derived,
        .payload = {std::byte{0x42}},
        .process = std::move(process),
    }};
  }
};
```

Ensure the file explicitly includes `<codec/processing.hpp>` if it does not already receive it through a direct include, plus `<optional>` and `<span>` for the declarations above.

After the existing F.1 partition assertions, add:

```cpp
PackageDistributedProcessor distributed_processor;
codec::LocalProcessorWorker distributed_worker{
    distributed_processor, "package-local-worker"};
auto execution = codec::execute_partition(
    distributed_worker, partitions->front(), partition_inputs);
if (!execution || execution->partition_identity != partitions->front().identity ||
    execution->worker_name != "package-local-worker" ||
    execution->processor_name != "package-distributed" ||
    execution->outputs.size() != 1 ||
    execution->outputs.front().truth != codec::TruthClass::derived) {
  return 1;
}
```

- [ ] **Step 2: Update README implementation status without remote-execution overclaim**

Add one implemented generic bullet stating that F.2 provides bounded synchronous one-partition/one-worker execution after exact F.1 membership and payload-integrity validation, with a concrete local `StreamProcessor` worker and generic `invoke_processor()` output validation.

In `planned_not_implemented`, remove only the now-implemented worker-execution primitive from the distributed item. Keep these explicit planned items:

```text
multi-partition scheduling/worker pools, RPC/network execution, processor discovery/distribution,
worker authentication/attestation and retry/lease/exactly-once semantics,
object-store backends/remote retrieval, distributed indexes/global locations,
automatic distributed persistence, operational benchmarks, and deployment integrations
```

Expand `## Distributed Processing Profile` with a concise F.2 paragraph. State that worker/processor labels are descriptive only, execution is synchronous/in-process for the concrete backend, and F.2 does not imply remote execution.

- [ ] **Step 3: Update CHANGELOG Unreleased**

Add one top Unreleased bullet describing:

```text
Stage F.2 adds a bounded DistributedWorker execution boundary and LocalProcessorWorker.
execute_partition verifies exact F.1 CDP1 membership, ordered record links, payload sizes,
payload SHA-256s and resource/name limits before one worker invocation, then validates outputs
through invoke_processor. No scheduling pool, RPC, object store, retry/exactly-once, attestation,
automatic persistence, deployment or performance/scale claim is added.
```

Do not edit historical release wording.

- [ ] **Step 4: Run the final exact-head CI-equivalent suite**

Require all of:

```text
GCC: configure + build + full tests + install + installed-package consumer
Clang: configure + build + full tests + install + installed-package consumer
Sanitizers: configure + build + full tests
```

The package consumer must compile against the installed headers/library rather than the source-tree include path.

- [ ] **Step 5: Audit the final diff**

Expected changed paths are limited to:

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

Reject any unrelated archive, transport, audio, CLI, C ABI, workflow, dependency, benchmark, or deployment change.

- [ ] **Step 6: Record final proof and merge only the exact green head**

Record in PR #/roadmap issue #10:

```yaml
result:
  outcome: bounded one-partition/one-worker execution over verified F.1 membership with a concrete synchronous LocalProcessorWorker and existing generic processor output validation
  red_head_sha: <exact RED SHA>
  red_ci_run: <run number>
  final_head_sha: <exact final green SHA>
  final_ci_run: <run number>
  capability_delta: F.1 partitions can now be verified against exact payload bytes and executed once through a worker boundary; no remote execution or persistence claim
  remaining_stage_exit_evidence: object-store addressing/backends and remote exact-record retrieval, distributed indexes/global locations, operational benchmarks, deployment integrations; multi-partition scheduling/RPC remain explicit later work
  merge_commit: <squash merge SHA>
```

Merge only if the PR head SHA is unchanged, mergeable, and that exact SHA has completed-success CI across all required jobs. If the head moves, verify the new head again before merging.

After merge, audit the Stage F dependency graph from merged evidence. The expected next dependency is object-store addressing/backends plus remote exact-record retrieval; do not freeze the next API solely from this plan.
