# Stage F.3 Object-Store Retrieval Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded provider-neutral object-store read contract that materializes exact F.1 partition members as verified `ExtractedRecord`s ready for F.2 execution.

**Architecture:** Keep F.1 partition membership immutable and placement-independent. Add opaque store/key/version placement descriptors plus a caller-supplied read-only backend; preflight the entire descriptor set before provider I/O, then fetch each exact range once in order, verify length and SHA-256, and return owned exact records. Reuse the existing private `CDP1` helper and extend the private exact-link helper to accept `RecordInfo` directly.

**Tech Stack:** C++20, existing `codec::Result`/`ErrorCode`, `Sha256`, `RecordInfo`/`ExtractedRecord`, F.1/F.2 distributed APIs, CMake/Ninja CI, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-f3-object-store-retrieval-design.md`

## Global Constraints

- Preserve CODA development-profile bytes, S0/S1/D semantics, provenance encoding, F.1 `DistributedPartition` fields and `CDP1` bytes, and F.2 worker semantics.
- `ObjectStoreObjectRef::{store,key,version}` are opaque placement strings; never parse them as URLs, paths, credentials, or authorization data.
- Validate the complete partition/location request before the first `ObjectStoreBackend::read_range()` call.
- Read one range per record, sequentially in partition order, with no retry, cache, range coalescing, failover, repair, decompression, decryption, or transformation.
- A provider `Error` propagates unchanged; wrong returned length is `protocol`; same-length wrong content is `archive_corrupt`.
- Any `std::bad_alloc` crossing `retrieve_partition_records()` becomes `resource_exhausted`; arbitrary provider exceptions remain uncaught.
- No bundled cloud/network/filesystem provider, upload/write API, backend registry, distributed index, scheduler, RPC worker execution, deployment integration, or performance/scale claim.
- No benchmark is required because F.3 makes no throughput/latency/scale/availability/durability/cost claim.

---

## File Structure

- `AI_WORKSHEET.md` — replace only the active work record with F.3 base/branch/proof/nonclaims.
- `include/codec/distributed.hpp` — additive public F.3 object reference, location, limits, backend, result, and retrieval declarations.
- `src/distributed/internal.hpp` — add `distributed_exact_link(const RecordInfo&)` overload.
- `src/distributed/identity.cpp` — implement the `RecordInfo` overload and keep the existing `ExtractedRecord` overload delegating to it; do not alter `CDP1` encoding.
- `src/distributed/retrieval.cpp` — all F.3 validation, provider invocation, integrity verification, and result assembly.
- `tests/test_distributed_retrieval.cpp` — complete F.3 RED/GREEN contract tests and F.2 handoff proof.
- `CMakeLists.txt` — register the F.3 test at RED; later register `src/distributed/retrieval.cpp` at GREEN.
- `tests/package_consumer/main.cpp` — define a tiny installed-header `ObjectStoreBackend`, retrieve exact records, then execute them via F.2.
- `README.md` — current-status F.3 capability and explicit remaining non-capabilities.
- `CHANGELOG.md` — one Unreleased F.3 bullet only.

---

### Task 1: Establish the F.3 RED contract

**Files:**
- Modify: `AI_WORKSHEET.md`
- Create: `tests/test_distributed_retrieval.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: merged F.1 `partition_exact_records()` and F.2 `execute_partition()`.
- Produces: a compile-time wished-for F.3 API exactly matching the spec; production declarations do not exist yet on the RED head.

- [ ] **Step 1: Replace the active worksheet record with F.3 evidence**

Use this exact active record shape at the top of `AI_WORKSHEET.md` while leaving the reusable worksheet sections below unchanged:

```yaml
## Active work record — Stage F.3

```yaml
task: Add bounded object-store placement descriptors and exact record retrieval for F.1 partitions while preserving F.2 execution and generic truth semantics.
base_ref: origin/main
base_head_sha: c685cdcc06518478cb7390b3abfc71f2cdc32692
work_branch: automation/stage-f3-object-store-retrieval
current_version: 0.2.0
active_roadmap_stage: F — F.1 exact-work partitioning and F.2 bounded worker execution are merged; exact storage placement/materialization is the next unmet dependency.
continuity_evidence:
  - git_head: main at c685cdcc06518478cb7390b3abfc71f2cdc32692
  - open_prs: preserve unrelated work; F.3 uses its own branch/PR
  - exact_head_ci: F.2 final head de963a367457439bc6445bd50af0479b8c803beb passed CI 233 before merge
  - roadmap_issue: issue 10 records F.2 complete and object-store retrieval next
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: A caller can bind exact F.1 record links to opaque object-store ranges, retrieve each range through a caller-supplied backend under explicit bounds, verify exact record SHA-256, and hand the resulting ExtractedRecord batch unchanged to F.2.
change_class: generic_stream_abstraction
```

```text
BEFORE: F.1 identifies exact work and F.2 executes materialized exact records, but CODEC has no generic storage-placement/read boundary that can materialize a partition from object-store ranges.
AFTER: retrieve_partition_records validates one exact F.1 partition plus ordered placement descriptors before provider I/O, reads each exact range once through a caller-supplied backend, verifies length and SHA-256, and returns records directly consumable by F.2.
```

```yaml
proof:
  regression_test: tests/test_distributed_retrieval.cpp plus unchanged F.1/F.2 and all existing tests
  exactness_test: partition CDP1/link order/payload total plus each retrieved range length and SHA-256 must verify before success
  compatibility_test: F.1 CDP1 bytes, F.2 execution, CODA, S0/S1/D, provenance, Stage E, C ABI, CLI, and installed package behavior remain compatible
  failure_path_test: malformed/tampered placement descriptors, exceeded bounds, provider errors, short/long ranges, and wrong content fail closed with no retry or partial result
  security_test: store/key/version/backend labels remain descriptive placement metadata only; no credential, authorization, authentication, attestation, or storage-proof claim
  benchmark: n/a — no throughput, latency, scale, availability, durability, fault-tolerance, or cost claim
```

Invariant decisions:

- [x] S0/S1/D semantics remain unchanged; F.3 only materializes exact bytes already identified by physical record hashes.
- [x] F.1 partition identity remains independent of archive/object placement; CDP1 bytes do not change.
- [x] Original RecordInfo.file_offset is preserved as record metadata and is never used as the object-store byte offset.
- [x] Complete descriptor preflight occurs before the first backend range read.
- [x] Returned range length and SHA-256 verify before a record enters the success result.
- [x] No cloud SDK/client, upload/write path, distributed index, backend registry, scheduler, RPC execution, retry/failover, automatic persistence, deployment integration, or scale claim is introduced.
```

- [ ] **Step 2: Create the RED test file**

Create `tests/test_distributed_retrieval.cpp` with helpers that exercise the wished-for public API before it exists:

```cpp
#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
  out.record.file_offset = 1000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::DistributedPartition one_partition(
    const std::vector<codec::ExtractedRecord>& inputs) {
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});
  return partitions->front();
}

codec::DistributedRecordLocation location_for(
    const codec::ExtractedRecord& input,
    std::string key,
    std::uint64_t offset) {
  return codec::DistributedRecordLocation{
      .record = input.record,
      .object = {.store = "store-a", .key = std::move(key), .version = "v1"},
      .offset = offset,
      .length = input.record.payload_size,
  };
}

struct RangeCall {
  codec::ObjectStoreObjectRef object;
  std::uint64_t offset{};
  std::uint64_t length{};
};

class MemoryRangeBackend final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return backend_name; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    calls.push_back(RangeCall{object, offset, length});
    if (failure.has_value()) return *failure;
    const auto found = objects.find(object.store + "/" + object.key + "/" + object.version);
    if (found == objects.end()) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "object missing", true);
    }
    if (offset > found->second.size()) return std::vector<std::byte>{};
    const auto available = static_cast<std::uint64_t>(found->second.size()) - offset;
    const auto count = std::min(length, available);
    const auto begin = found->second.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(count);
    return std::vector<std::byte>{begin, end};
  }

  void put(const codec::ObjectStoreObjectRef& object,
           std::vector<std::byte> payload) {
    objects[object.store + "/" + object.key + "/" + object.version] =
        std::move(payload);
  }

  std::string backend_name{"memory-range"};
  std::map<std::string, std::vector<std::byte>> objects;
  std::optional<codec::Error> failure;
  std::vector<RangeCall> calls;
};

class CountingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "f3-consumer"; }
  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{codec::ProcessorOutput{
        .stream = inputs.front().record.stream,
        .type = 0x7b01,
        .start_ns = inputs.front().record.start_ns,
        .end_ns = inputs.back().record.end_ns,
        .truth = codec::TruthClass::derived,
        .payload = bytes("ok"),
        .process = {.operation = "f3-test",
                    .implementation_id = "codec-test",
                    .implementation_version = "1",
                    .created_utc_ns = 1},
    }};
  }
  std::size_t calls{};
};

void expect_pre_read_error(MemoryRangeBackend& backend,
                           const codec::DistributedPartition& partition,
                           std::span<const codec::DistributedRecordLocation> locations,
                           codec::ErrorCode expected,
                           codec::DistributedRetrievalLimits limits = {}) {
  const auto before = backend.calls.size();
  auto result = codec::retrieve_partition_records(
      backend, partition, locations, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, expected);
  EXPECT_EQ(backend.calls.size(), before);
}

}  // namespace
```

Add focused tests with these exact behavior names:

```cpp
TEST(distributed_retrieval_materializes_exact_partition_and_hands_off_to_f2)
TEST(distributed_retrieval_preserves_record_metadata_and_provider_ranges)
TEST(distributed_retrieval_rejects_tampered_partition_before_reads)
TEST(distributed_retrieval_rejects_wrong_or_reordered_locations_before_reads)
TEST(distributed_retrieval_rejects_invalid_location_shape_before_reads)
TEST(distributed_retrieval_validates_limits_and_labels_before_reads)
TEST(distributed_retrieval_propagates_provider_error_without_retry)
TEST(distributed_retrieval_rejects_short_or_long_provider_ranges)
TEST(distributed_retrieval_rejects_same_length_wrong_payload_hash)
TEST(distributed_retrieval_supports_zero_length_exact_record)
TEST(distributed_retrieval_preserves_unknown_record_type_code)
```

For the first test, construct two same-stream records, build one F.1 partition, store each payload in a separate backend object, call `retrieve_partition_records()`, assert exact partition identity/stream/backend name and byte-for-byte record equality, then call F.2 `execute_partition()` with a `LocalProcessorWorker` and assert one processor call.

For every descriptor/limit rejection test, use `expect_pre_read_error()` and assert `backend.calls` remains unchanged. Cover identity tamper, partition link stream tamper, reordered/missing/extra locations, location record sequence/hash/stream tamper, interval inversion, `length != record.payload_size`, `offset + length` overflow, empty store, empty key, partition payload total mismatch, each zero limit, excessive count/bytes/backend-name/store/key/version length.

For provider error, set `backend.failure = Error{ErrorCode::network, "remote unavailable", true}` and assert one call, unchanged error fields, and no retry. For short/long range behavior, use dedicated test backends that intentionally return `length - 1` or `length + 1` bytes and assert `protocol`. For wrong content, return exactly the requested byte count with different bytes and assert `archive_corrupt`.

For unknown type preservation, set `input.record.type = static_cast<codec::RecordType>(0x7d01)` before partitioning and assert `result.records.front().record.type_code() == 0x7d01` after retrieval. Preserve `file_offset` and assert it is not replaced by the object offset.

- [ ] **Step 3: Register only the RED test in CMake**

Add `tests/test_distributed_retrieval.cpp` immediately after the other distributed tests in the `codec_tests` source list. Do **not** add `src/distributed/retrieval.cpp` because it must not exist yet.

- [ ] **Step 4: Commit the RED head**

Commit only `AI_WORKSHEET.md`, `tests/test_distributed_retrieval.cpp`, and the test registration in `CMakeLists.txt`:

```bash
git add AI_WORKSHEET.md tests/test_distributed_retrieval.cpp CMakeLists.txt
git commit -m "test: define Stage F.3 object-store retrieval contract"
```

- [ ] **Step 5: Open the PR and verify RED in CI**

Open PR title `Stage F.3: add bounded object-store record retrieval` against `main`, documenting that the head is intentionally RED and has no F.3 declarations/implementation.

Expected CI failure: compilation of `tests/test_distributed_retrieval.cpp` because `ObjectStoreObjectRef`, `DistributedRecordLocation`, `DistributedRetrievalLimits`, `ObjectStoreBackend`, `DistributedRetrievalResult`, and `retrieve_partition_records()` do not exist. Confirm the failure is the missing feature, not a typo/test-harness error, before proceeding.

---

### Task 2: Implement the minimal F.3 retrieval boundary

**Files:**
- Modify: `include/codec/distributed.hpp`
- Modify: `src/distributed/internal.hpp`
- Modify: `src/distributed/identity.cpp`
- Create: `src/distributed/retrieval.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_distributed_retrieval.cpp`

**Interfaces:**
- Consumes: `detail::distributed_partition_identity()`, `RecordInfo`, `ExtractedRecord`, `Sha256`, `Result`, F.1 `DistributedPartition`.
- Produces: the exact public F.3 declarations from the spec and verified ordered `DistributedRetrievalResult::records` for F.2.

- [ ] **Step 1: Add the public declarations exactly as specified**

Append to `include/codec/distributed.hpp` before the namespace closing brace:

```cpp
struct ObjectStoreObjectRef {
  std::string store;
  std::string key;
  std::string version;
};

struct DistributedRecordLocation {
  RecordInfo record{};
  ObjectStoreObjectRef object;
  std::uint64_t offset{};
  std::uint64_t length{};
};

struct DistributedRetrievalLimits {
  std::size_t maximum_records{1024};
  std::uint64_t maximum_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_backend_name_bytes{256};
  std::size_t maximum_store_bytes{512};
  std::size_t maximum_key_bytes{4096};
  std::size_t maximum_version_bytes{512};
};

class ObjectStoreBackend {
 public:
  virtual ~ObjectStoreBackend() = default;
  virtual std::string name() const = 0;
  virtual Result<std::vector<std::byte>> read_range(
      const ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) = 0;
};

struct DistributedRetrievalResult {
  Sha256 partition_identity{};
  StreamId stream{};
  std::string backend_name;
  std::vector<ExtractedRecord> records;
};

Result<DistributedRetrievalResult> retrieve_partition_records(
    ObjectStoreBackend& backend,
    const DistributedPartition& partition,
    std::span<const DistributedRecordLocation> locations,
    DistributedRetrievalLimits limits = {});
```

No new dependencies are required because `distributed.hpp` already includes the types used above.

- [ ] **Step 2: Extend the private exact-link helper without changing CDP1 bytes**

Change `src/distributed/internal.hpp` to declare both overloads:

```cpp
ProvenanceRecordLink distributed_exact_link(const RecordInfo& record);
ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input);
```

In `src/distributed/identity.cpp`, implement:

```cpp
ProvenanceRecordLink distributed_exact_link(const RecordInfo& record) {
  return ProvenanceRecordLink{
      .stream = record.stream,
      .type = record.type_code(),
      .sequence = record.sequence,
      .hash = record.hash,
  };
}

ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input) {
  return distributed_exact_link(input.record);
}
```

Do not modify `distributed_partition_identity()` or the `"CDP1"` byte encoding.

- [ ] **Step 3: Implement preflight and exact retrieval**

Create `src/distributed/retrieval.cpp` with this structure:

```cpp
#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
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

Result<void> validate_retrieval_limits(
    const DistributedRetrievalLimits& limits) {
  if (limits.maximum_records == 0 || limits.maximum_bytes == 0 ||
      limits.maximum_backend_name_bytes == 0 ||
      limits.maximum_store_bytes == 0 || limits.maximum_key_bytes == 0 ||
      limits.maximum_version_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed retrieval limits must be non-zero");
  }
  return {};
}

}  // namespace

Result<DistributedRetrievalResult> retrieve_partition_records(
    ObjectStoreBackend& backend,
    const DistributedPartition& partition,
    std::span<const DistributedRecordLocation> locations,
    DistributedRetrievalLimits limits) {
  try {
    auto valid_limits = validate_retrieval_limits(limits);
    if (!valid_limits) return valid_limits.error();

    if (partition.records.empty() || locations.empty() ||
        partition.records.size() != locations.size()) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed retrieval requires one non-empty exact location per partition member");
    }
    if (locations.size() > limits.maximum_records) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::resource_exhausted,
          "distributed retrieval record-count limit exceeded");
    }

    std::string backend_name = backend.name();
    if (backend_name.empty()) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "object-store backend name must be non-empty");
    }
    if (backend_name.size() > limits.maximum_backend_name_bytes) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::resource_exhausted,
          "object-store backend name exceeds the configured limit");
    }

    for (const auto& link : partition.records) {
      if (link.stream != partition.stream) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed partition contains a link from another stream");
      }
    }
    if (detail::distributed_partition_identity(partition.stream,
                                               partition.records) !=
        partition.identity) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed partition identity does not match ordered membership");
    }

    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0; index < locations.size(); ++index) {
      const auto& location = locations[index];
      if (location.record.stream != partition.stream ||
          !same_link(detail::distributed_exact_link(location.record),
                     partition.records[index])) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location does not match ordered partition membership");
      }
      if (location.record.end_ns < location.record.start_ns) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location end time precedes start time");
      }
      if (location.length != location.record.payload_size) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location length does not match payload size");
      }
      if (location.offset >
          std::numeric_limits<std::uint64_t>::max() - location.length) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location range overflows");
      }
      if (location.object.store.empty() || location.object.key.empty()) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "object-store location store and key must be non-empty");
      }
      if (location.object.store.size() > limits.maximum_store_bytes ||
          location.object.key.size() > limits.maximum_key_bytes ||
          location.object.version.size() > limits.maximum_version_bytes) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::resource_exhausted,
            "object-store location metadata exceeds configured limits");
      }
      if (location.length > limits.maximum_bytes - total_bytes) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::resource_exhausted,
            "distributed retrieval aggregate byte limit exceeded");
      }
      total_bytes += location.length;
    }
    if (total_bytes != partition.payload_bytes) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed partition payload-byte total is inconsistent with locations");
    }

    std::vector<ExtractedRecord> records;
    records.reserve(locations.size());
    for (const auto& location : locations) {
      auto payload = backend.read_range(location.object,
                                        location.offset,
                                        location.length);
      if (!payload) return payload.error();
      if (payload->size() != location.length) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::protocol,
            "object-store backend returned a range with the wrong byte count");
      }
      if (sha256(*payload) != location.record.hash) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::archive_corrupt,
            "object-store payload hash does not match exact record identity");
      }
      records.push_back(ExtractedRecord{
          .record = location.record,
          .payload = std::move(*payload),
      });
    }

    return DistributedRetrievalResult{
        .partition_identity = partition.identity,
        .stream = partition.stream,
        .backend_name = std::move(backend_name),
        .records = std::move(records),
    };
  } catch (const std::bad_alloc&) {
    return fail<DistributedRetrievalResult>(
        ErrorCode::resource_exhausted,
        "distributed retrieval allocation failed");
  }
}

}  // namespace codec
```

The exact wording of diagnostic strings is not API; keep error codes and validation ordering as specified.

- [ ] **Step 4: Register the production source**

Add `src/distributed/retrieval.cpp` next to `src/distributed/partition.cpp` and `worker.cpp` in `codec_core`.

- [ ] **Step 5: Run CI-equivalent verification and fix implementation defects only**

Release:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizers:

```bash
cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

If this environment cannot resolve the repository's external CI dependencies locally, use the GitHub Actions PR run as the equivalent gate and fetch failing job logs before changing code. Do not weaken tests to make a defect disappear.

- [ ] **Step 6: Commit the minimal GREEN implementation**

```bash
git add include/codec/distributed.hpp src/distributed/internal.hpp \
  src/distributed/identity.cpp src/distributed/retrieval.cpp CMakeLists.txt \
  tests/test_distributed_retrieval.cpp
git commit -m "feat: add bounded object-store record retrieval"
```

---

### Task 3: Prove installed-package F.3 → F.2 composition

**Files:**
- Modify: `tests/package_consumer/main.cpp`

**Interfaces:**
- Consumes: installed `<codec/distributed.hpp>`, `ObjectStoreBackend`, `retrieve_partition_records()`, `LocalProcessorWorker`, `execute_partition()`.
- Produces: package-level proof that F.3 output is directly usable by F.2 using only installed headers/library.

- [ ] **Step 1: Add tiny package-consumer provider and processor classes**

Near the top of `tests/package_consumer/main.cpp`, add required standard headers (`<span>`, `<string>`) and anonymous-namespace helpers:

```cpp
namespace {

class PackageObjectStore final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return "package-object-store"; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    if (object.store != "package" || object.key != "records" ||
        object.version != "v1" || offset > bytes_.size() ||
        length > bytes_.size() - offset) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "package object range unavailable");
    }
    const auto begin = bytes_.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    return std::vector<std::byte>{begin, end};
  }

  std::vector<std::byte> bytes_;
};

class PackageProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "package-processor"; }
  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{codec::ProcessorOutput{
        .stream = inputs.front().record.stream,
        .type = 0x7c01,
        .start_ns = inputs.front().record.start_ns,
        .end_ns = inputs.back().record.end_ns,
        .truth = codec::TruthClass::derived,
        .payload = {std::byte{0x55}},
        .process = {.operation = "package-f3",
                    .implementation_id = "package-consumer",
                    .implementation_version = "1",
                    .created_utc_ns = 1},
    }};
  }
  std::size_t calls{};
};

}  // namespace
```

- [ ] **Step 2: Extend the existing F.1 package example into F.3/F.2 composition**

After the package consumer creates `partition_inputs` and verifies the single F.1 partition, concatenate the two payloads into `PackageObjectStore::bytes_`, create two `DistributedRecordLocation`s for offsets 0 and 1 with the original `RecordInfo`s and the same `{store="package", key="records", version="v1"}`, then:

```cpp
auto retrieved = codec::retrieve_partition_records(
    object_store, partitions->front(), locations);
if (!retrieved || retrieved->records != partition_inputs) return 1;

PackageProcessor package_processor;
codec::LocalProcessorWorker package_worker{package_processor, "package-worker"};
auto distributed_execution = codec::execute_partition(
    package_worker, partitions->front(), retrieved->records);
if (!distributed_execution || package_processor.calls != 1 ||
    distributed_execution->outputs.size() != 1 ||
    distributed_execution->outputs.front().payload !=
        std::vector<std::byte>{std::byte{0x55}}) {
  return 1;
}
```

Because `ExtractedRecord` has no equality operator, compare both returned records field-by-field and payload-by-payload rather than using `retrieved->records != partition_inputs` if compilation rejects that shorthand.

- [ ] **Step 3: Verify package installation/consumer on GCC and Clang CI**

Push the package-consumer commit and require the PR CI jobs' `Install package` and `Test installed package consumer` steps to pass for both GCC and Clang.

- [ ] **Step 4: Commit package proof**

```bash
git add tests/package_consumer/main.cpp
git commit -m "test: exercise F.3 through installed package"
```

---

### Task 4: Synchronize status claims and perform the exact-head merge gate

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Review: all base-to-head changed files

**Interfaces:**
- Consumes: fully green F.3 implementation and package proof.
- Produces: truthful current-status documentation and exact-head merge evidence.

- [ ] **Step 1: Update README current-status bullets only after GREEN evidence**

Add one implemented generic bullet summarizing F.3:

```text
- bounded provider-neutral object-store record retrieval over explicit placement descriptors: a caller-supplied backend receives opaque store/key/version plus exact byte ranges only after complete F.1 partition/location preflight; returned ranges must match requested lengths and physical-record SHA-256 before ordered ExtractedRecord materialization, preserving original RecordInfo and composing directly with F.2
```

Replace the Stage F planned-not-implemented line so it leaves these capabilities planned: concrete cloud/network provider clients, writes/uploads, backend registry, location discovery/indexes/global archive catalog, scheduler/RPC worker execution, retries/failover/exactly-once, automatic persistence, operational benchmarks, and deployment integrations.

Update the roadmap/repository-map Stage F wording to say F.1 partitioning, F.2 bounded worker execution, and F.3 bounded provider-neutral exact retrieval are implemented; indexes, operational benchmarks, and deployment remain planned.

In `## Distributed Processing Profile`, retain F.1/F.2 paragraphs and append a concise F.3 section that explicitly states:

- placement remains outside `CDP1`;
- `RecordInfo.file_offset` is original metadata, not object offset;
- backend/provider details remain caller-owned;
- returned ranges are verified by length and SHA-256;
- no concrete provider client, write/upload, index, registry, scheduling/RPC, retry/failover, deployment, or measured scale claim is added.

- [ ] **Step 2: Add one Unreleased changelog bullet**

At the top of `## Unreleased`, add exactly one F.3 bullet describing the public API and nonclaims. Do not modify historical release wording.

- [ ] **Step 3: Commit docs/status**

```bash
git add README.md CHANGELOG.md
git commit -m "docs: record Stage F.3 retrieval scope"
```

- [ ] **Step 4: Run final exact-head CI**

Record the final branch head SHA, then require one GitHub Actions run associated with that exact SHA to complete successfully with:

- GCC: configure, build, tests, install, installed-package consumer;
- Clang: configure, build, tests, install, installed-package consumer;
- sanitizers: configure, build, tests.

If the head changes for any reason, the previous run is stale and does not satisfy the merge gate.

- [ ] **Step 5: Review the exact base-to-head diff**

Compare base `c685cdcc06518478cb7390b3abfc71f2cdc32692` to the final head. Expected paths are limited to:

```text
AI_WORKSHEET.md
CHANGELOG.md
CMakeLists.txt
README.md
docs/superpowers/plans/2026-08-28-stage-f3-object-store-retrieval.md
docs/superpowers/specs/2026-08-28-stage-f3-object-store-retrieval-design.md
include/codec/distributed.hpp
src/distributed/identity.cpp
src/distributed/internal.hpp
src/distributed/retrieval.cpp
tests/package_consumer/main.cpp
tests/test_distributed_retrieval.cpp
```

Review specifically for: unchanged `CDP1` bytes, no F.1/F.2 semantic weakening, no URL/path/cloud-vendor parsing, no retries, no partial success result, no undocumented network/storage claim, and no accidental archive/profile/transport edits.

- [ ] **Step 6: Submit/record pre-merge review**

If no independent reviewer subagent is available in the harness, perform a direct exact-diff review and record a PR COMMENT review stating whether Critical/Important/Minor findings remain. Any Critical or Important finding must be fixed and reverified before merge.

- [ ] **Step 7: Merge only the exact green head**

Squash-merge the PR with `expected_head_sha=<final-green-sha>`. The merge message must state that F.3 adds bounded caller-supplied object-store range retrieval and explicitly does not add a concrete cloud client, write/upload, index, scheduler/RPC, retry/failover, deployment integration, or scale claim.

- [ ] **Step 8: Post-merge verification and roadmap evidence**

Fetch `main` and require it to equal the merge commit returned by GitHub. Add a roadmap issue #10 comment containing:

- RED head and failing CI run proving the missing API;
- final exact head and fully green CI run;
- merge commit on `main`;
- exact F.3 capability/nonclaims;
- Stage F audit: F.1 membership + F.2 execution + F.3 exact storage materialization are now present;
- next expected dependency: distributed location/index primitive, subject to a fresh merged-tree design audit before freezing F.4.
