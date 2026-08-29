# Stage F.1 Distributed Partitioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded deterministic C++ partitioning primitive that groups exact extracted records into stable one-stream work partitions without adding worker, storage, network, persistence, or scale claims.

**Architecture:** Add an isolated Stage F public surface in `<codec/distributed.hpp>` and implementation in `src/distributed/partition.cpp`. `partition_exact_records()` pre-validates exact inputs and caller limits, then scans input order while keeping one open partition per `StreamId`; partitions contain ordered `ProvenanceRecordLink`s and receive a SHA-256 identity over a private canonical `CDP1` descriptor encoding. The function is transactional and never mutates CODA or reclassifies S0/S1/D.

**Tech Stack:** C++20, existing `codec::Result`, `Sha256`, `StreamId`, `ExtractedRecord`, `ProvenanceRecordLink`, CMake/CTest, GitHub Actions GCC/Clang/ASan+UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-f1-distributed-partitioning-design.md`

## Global Constraints

- Base is merged `main` commit `5301da72c65f519b0c16347f8684be113d15ab0b`.
- Preserve CODA development-profile bytes, S0/S1/D semantics, provenance encoding, StreamId semantics, Stage E contracts, C ABI, and CLI behavior.
- All limits are explicit and non-zero; empty input is valid and returns no partitions.
- A partition contains exactly one `StreamId`; records are never split.
- Partition identity is SHA-256 over `CDP1`, stream bytes, ordered exact record type/sequence/hash membership only.
- SHA-256 is identity/integrity evidence only, never authentication.
- No workers, scheduling, RPC/sockets, object store, remote retrieval, distributed index, automatic CODA persistence, deployment integration, or performance/scale claim in F.1.

---

### Task 1: Record the F.1 contract and establish RED tests

**Files:**
- Modify: `AI_WORKSHEET.md` active work record
- Create: `tests/test_distributed_partition.cpp`
- Modify: `CMakeLists.txt` test source list only

**Interfaces:**
- Consumes: existing `codec::ExtractedRecord`, `codec::RecordInfo`, `codec::StreamId`, `codec::Sha256`.
- Produces for Task 2: compile-time expectations for `DistributedPartitionLimits`, `DistributedPartition`, and `partition_exact_records()` from `<codec/distributed.hpp>`.

- [ ] **Step 1: Replace the worksheet active record with F.1**

Use these values at the top of `AI_WORKSHEET.md`:

```yaml
task: Add bounded deterministic distributed work partitioning over exact extracted records without changing truth or archive semantics.
base_ref: origin/main
base_head_sha: 5301da72c65f519b0c16347f8684be113d15ab0b
work_branch: automation/stage-f1-distributed-partitioning
current_version: 0.2.0
active_roadmap_stage: F — Stage E is complete at the bounded CMX1/E.2/XRF1 streaming-repair scope; the first unmet distributed-profile gate is worker-agnostic exact-work partitioning.
continuity_evidence:
  - git_head: main at 5301da72c65f519b0c16347f8684be113d15ab0b
  - open_prs: inspect before merge and preserve unrelated work
  - exact_head_ci: Stage E.5 PR head 57f6bbd6e8b8fb9e2ac20ee23d78a8c9bbc55036 passed CI 209 before merge
  - roadmap_issue: issue 10 records Stage E complete and F.1 next
roadmap_issue_title: CODEC v1.0 roadmap execution log
scope: distributed
touched_truth_classes: []
current_behavior_verified_from: [code, tests, cli, changelog]
new_capability_claim: A caller can deterministically divide an exact extracted-record batch into bounded one-stream logical work partitions with stable exact-record membership identities, without assigning workers or storage locations.
change_class: generic_stream_abstraction
```

Set BEFORE/AFTER and proof text to the exact F.1 design and explicitly retain all non-claims from the spec.

- [ ] **Step 2: Add the failing test file**

Create `tests/test_distributed_partition.cpp` with helpers and tests using the desired public API:

```cpp
#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::size_t payload_size) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.payload.resize(payload_size);
  for (std::size_t i = 0; i < payload_size; ++i) {
    out.payload[i] = static_cast<std::byte>((sequence + i * 17U) & 0xffU);
  }
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

void expect_link(const codec::ProvenanceRecordLink& link,
                 const codec::ExtractedRecord& input) {
  EXPECT_EQ(link.stream, input.record.stream);
  EXPECT_EQ(link.type, input.record.type_code());
  EXPECT_EQ(link.sequence, input.record.sequence);
  EXPECT_EQ(link.hash, input.record.hash);
}

}  // namespace

TEST(distributed_partition_is_deterministic_and_preserves_exact_links) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/a", 1, 3), exact_record("f1/b", 2, 4),
      exact_record("f1/a", 3, 5), exact_record("f1/a", 4, 6),
      exact_record("f1/b", 5, 7)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 2;
  limits.maximum_payload_bytes_per_partition = 64;

  auto first = codec::partition_exact_records(inputs, limits);
  auto second = codec::partition_exact_records(inputs, limits);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(first->size(), std::size_t{3});
  EXPECT_EQ(second->size(), first->size());

  EXPECT_EQ((*first)[0].stream, inputs[0].record.stream);
  EXPECT_EQ((*first)[0].records.size(), std::size_t{2});
  expect_link((*first)[0].records[0], inputs[0]);
  expect_link((*first)[0].records[1], inputs[2]);
  EXPECT_EQ((*first)[0].payload_bytes, std::uint64_t{8});

  EXPECT_EQ((*first)[1].stream, inputs[1].record.stream);
  EXPECT_EQ((*first)[1].records.size(), std::size_t{2});
  expect_link((*first)[1].records[0], inputs[1]);
  expect_link((*first)[1].records[1], inputs[4]);

  EXPECT_EQ((*first)[2].stream, inputs[3].record.stream);
  EXPECT_EQ((*first)[2].records.size(), std::size_t{1});
  expect_link((*first)[2].records[0], inputs[3]);

  for (std::size_t i = 0; i < first->size(); ++i) {
    EXPECT_EQ((*first)[i].identity, (*second)[i].identity);
    EXPECT_EQ((*first)[i].records, (*second)[i].records);
  }
}

TEST(distributed_partition_splits_on_payload_bytes_without_splitting_records) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/bytes", 1, 5),
      exact_record("f1/bytes", 2, 6),
      exact_record("f1/bytes", 3, 4)};
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 8;
  limits.maximum_payload_bytes_per_partition = 10;
  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_TRUE(result);
  EXPECT_EQ(result->size(), std::size_t{2});
  EXPECT_EQ((*result)[0].records.size(), std::size_t{1});
  EXPECT_EQ((*result)[0].payload_bytes, std::uint64_t{5});
  EXPECT_EQ((*result)[1].records.size(), std::size_t{2});
  EXPECT_EQ((*result)[1].payload_bytes, std::uint64_t{10});
}

TEST(distributed_partition_identity_changes_with_ordered_membership) {
  auto a = exact_record("f1/id", 1, 2);
  auto b = exact_record("f1/id", 2, 2);
  const std::vector<codec::ExtractedRecord> forward{a, b};
  const std::vector<codec::ExtractedRecord> reverse{b, a};
  auto lhs = codec::partition_exact_records(forward);
  auto rhs = codec::partition_exact_records(reverse);
  EXPECT_TRUE(lhs);
  EXPECT_TRUE(rhs);
  EXPECT_EQ(lhs->size(), std::size_t{1});
  EXPECT_EQ(rhs->size(), std::size_t{1});
  EXPECT_FALSE((*lhs)[0].identity == (*rhs)[0].identity);
}

TEST(distributed_partition_empty_input_is_valid) {
  const std::vector<codec::ExtractedRecord> inputs;
  auto result = codec::partition_exact_records(inputs);
  EXPECT_TRUE(result);
  EXPECT_TRUE(result->empty());
}

TEST(distributed_partition_rejects_zero_limits) {
  codec::DistributedPartitionLimits limits;
  limits.maximum_partitions = 0;
  const std::vector<codec::ExtractedRecord> inputs{exact_record("f1/a", 1, 1)};
  auto result = codec::partition_exact_records(inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_partition_rejects_inconsistent_exact_input) {
  auto malformed = exact_record("f1/a", 1, 3);
  malformed.record.payload_size += 1;
  const std::vector<codec::ExtractedRecord> inputs{malformed};
  auto result = codec::partition_exact_records(inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(distributed_partition_enforces_input_and_partition_bounds) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f1/a", 1, 6), exact_record("f1/a", 2, 6)};

  auto aggregate_limits = codec::DistributedPartitionLimits{};
  aggregate_limits.maximum_input_bytes = 11;
  auto aggregate = codec::partition_exact_records(inputs, aggregate_limits);
  EXPECT_FALSE(aggregate);
  EXPECT_EQ(aggregate.error().code, codec::ErrorCode::resource_exhausted);

  auto record_limits = codec::DistributedPartitionLimits{};
  record_limits.maximum_payload_bytes_per_partition = 5;
  auto oversized = codec::partition_exact_records(inputs, record_limits);
  EXPECT_FALSE(oversized);
  EXPECT_EQ(oversized.error().code, codec::ErrorCode::resource_exhausted);

  auto partition_limits = codec::DistributedPartitionLimits{};
  partition_limits.maximum_records_per_partition = 1;
  partition_limits.maximum_partitions = 1;
  auto too_many = codec::partition_exact_records(inputs, partition_limits);
  EXPECT_FALSE(too_many);
  EXPECT_EQ(too_many.error().code, codec::ErrorCode::resource_exhausted);
}
```

If `ProvenanceRecordLink` lacks `operator==`, compare the four fields individually instead of changing archive ABI solely for test convenience.

- [ ] **Step 3: Register only the new test source in CMake**

Add:

```cmake
    tests/test_distributed_partition.cpp
```

to the existing `codec_tests` source list. Do not add production source yet.

- [ ] **Step 4: Open a PR for the RED head and verify the expected failure**

Push the branch/open the PR, then run the normal GitHub CI. Expected failure: compilation cannot find `<codec/distributed.hpp>` (or equivalent missing wished-for F.1 API). Any unrelated CMake/test syntax failure must be fixed before proceeding.

- [ ] **Step 5: Commit the RED contract**

The commit containing worksheet + tests + test registration must precede production code. Record its exact SHA and CI run in the PR body/roadmap evidence.

---

### Task 2: Implement the bounded deterministic partitioner

**Files:**
- Create: `include/codec/distributed.hpp`
- Create: `src/distributed/partition.cpp`
- Modify: `CMakeLists.txt` library source list
- Test: `tests/test_distributed_partition.cpp`

**Interfaces:**
- Consumes: `ExtractedRecord`, `ProvenanceRecordLink`, `StreamId`, `Sha256`, `Result`.
- Produces: `DistributedPartitionLimits`, `DistributedPartition`, `partition_exact_records()` exactly as specified.

- [ ] **Step 1: Add the public header**

Create `include/codec/distributed.hpp`:

```cpp
#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec {

struct DistributedPartitionLimits {
  std::size_t maximum_input_records{16384};
  std::uint64_t maximum_input_bytes{256ULL * 1024ULL * 1024ULL};
  std::size_t maximum_partitions{4096};
  std::size_t maximum_records_per_partition{1024};
  std::uint64_t maximum_payload_bytes_per_partition{
      64ULL * 1024ULL * 1024ULL};
};

struct DistributedPartition {
  Sha256 identity{};
  StreamId stream{};
  std::vector<ProvenanceRecordLink> records;
  std::uint64_t payload_bytes{};
};

Result<std::vector<DistributedPartition>> partition_exact_records(
    std::span<const ExtractedRecord> inputs,
    DistributedPartitionLimits limits = {});

}  // namespace codec
```

- [ ] **Step 2: Add private helpers for validation and CDP1 identity**

In `src/distributed/partition.cpp`, include `<codec/distributed.hpp>`, `<codec/integrity.hpp>`, and only standard headers needed for bounded vectors/lookup. Implement little-endian append helpers locally:

```cpp
void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  out.push_back(static_cast<std::byte>(value & 0xffU));
  out.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}
```

Build identity bytes exactly as spec `CDP1 + StreamId + u64 count + ordered(type u16, sequence u64, hash[32])`, then call `sha256(bytes)`. Do not encode payload copies, worker IDs, limits, or locations.

- [ ] **Step 3: Prevalidate all inputs transactionally**

Before constructing any output partition:

```cpp
if (limits.maximum_input_records == 0 ||
    limits.maximum_input_bytes == 0 ||
    limits.maximum_partitions == 0 ||
    limits.maximum_records_per_partition == 0 ||
    limits.maximum_payload_bytes_per_partition == 0) {
  return fail<std::vector<DistributedPartition>>(
      ErrorCode::invalid_argument,
      "distributed partition limits must be non-zero");
}
if (inputs.size() > limits.maximum_input_records) {
  return fail<std::vector<DistributedPartition>>(
      ErrorCode::resource_exhausted,
      "distributed partition input-count limit exceeded");
}
```

Then validate `payload.size() == record.payload_size`, each payload fits the per-partition byte limit, and aggregate bytes fit `maximum_input_bytes` using subtraction-based overflow-safe checks. Empty input returns `{}` after limit validation.

- [ ] **Step 4: Partition by stream with one open partition per stream**

Use a small private `OpenPartition { StreamId stream; std::size_t partition_index; }` vector rather than introducing a hashing requirement for `StreamId`. For each input in caller order:

1. find the current open partition for its stream;
2. if none, or adding the record would exceed record/byte limits, stage a new partition after checking `maximum_partitions`;
3. append exactly one `ProvenanceRecordLink` from `RecordInfo`;
4. increase `payload_bytes` with already-validated size;
5. if a new partition was created for a known stream, update its open index.

Construct all output in local staged vectors and catch `std::bad_alloc`, returning `resource_exhausted` without caller mutation.

- [ ] **Step 5: Finalize every partition identity**

After membership is complete, calculate each partition's `identity` from the exact ordered link list. Identity creation must be deterministic and must not observe pointer addresses, allocation capacity, worker/storage state, or limit values.

- [ ] **Step 6: Register production source**

Add:

```cmake
  src/distributed/partition.cpp
```

inside `add_library(codec_core ...)`.

- [ ] **Step 7: Build and run only the F.1 tests**

Run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
./build/codec_tests --include-prefix distributed_partition_
```

Expected: all F.1 tests pass, zero failures.

- [ ] **Step 8: Run the full existing suite**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all CTest targets pass.

- [ ] **Step 9: Commit implementation**

Commit header, source, CMake production registration, and any test corrections required by actual existing comparison operators. Do not broaden scope.

---

### Task 3: Prove installed-package compatibility and synchronize current-status claims

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md` proof/completion evidence if needed
- Test: existing package-consumer CI plus full matrix

**Interfaces:**
- Consumes: installed `<codec/distributed.hpp>` and `partition_exact_records()` from Task 2.
- Produces: external-package proof and precise F.1 status/non-claims.

- [ ] **Step 1: Exercise the installed public header**

Add `#include <codec/distributed.hpp>` to `tests/package_consumer/main.cpp`. After an existing exact `MultiplexFrame`/archive record setup, construct two small `ExtractedRecord` values directly with matching payload sizes and distinct exact hashes, call `partition_exact_records()`, and require one partition with two exact links and a non-zero identity. Keep the consumer independent of repository-private headers.

A minimal check is:

```cpp
codec::ExtractedRecord partition_input_a;
partition_input_a.record.type = codec::RecordType::source_bytes;
partition_input_a.record.stream = codec::derive_stream_id("package-consumer/partition");
partition_input_a.record.sequence = 1;
partition_input_a.payload = {std::byte{0x10}};
partition_input_a.record.payload_size = 1;
partition_input_a.record.hash = codec::sha256(partition_input_a.payload);

auto partition_input_b = partition_input_a;
partition_input_b.record.sequence = 2;
partition_input_b.payload = {std::byte{0x20}, std::byte{0x21}};
partition_input_b.record.payload_size = 2;
partition_input_b.record.hash = codec::sha256(partition_input_b.payload);

const std::vector<codec::ExtractedRecord> partition_inputs{
    partition_input_a, partition_input_b};
auto partitions = codec::partition_exact_records(partition_inputs);
if (!partitions || partitions->size() != 1 ||
    partitions->front().records.size() != 2 ||
    partitions->front().payload_bytes != 3) {
  return 1;
}
```

If `sha256` is not currently included transitively in the consumer, include `<codec/integrity.hpp>` explicitly rather than relying on transitive includes.

- [ ] **Step 2: Update README current implementation**

Add one implemented generic bullet describing bounded deterministic one-stream exact-record partitioning with stable SHA-256 partition identity. Replace `distributed/cloud execution profile` in `planned_not_implemented` with the remaining Stage F work: distributed workers/scheduler, object-store backend, indexes, operational benchmarks, and deployment integrations. Add a short `Distributed Processing Profile` section stating F.1's exact semantics and explicit non-capabilities.

Do not claim distributed execution merely because partition descriptors exist.

- [ ] **Step 3: Update CHANGELOG Unreleased**

Add one F.1 bullet describing the additive C++ API, deterministic exact-link membership/identity, caller bounds, and all non-goals. Do not edit historical release wording.

- [ ] **Step 4: Run final exact-head verification**

Run/require the repository CI matrix on the exact final branch head:

- GCC build + tests + install + package consumer;
- Clang build + tests + install + package consumer;
- sanitizer build + tests;
- `codec-ai-contract` through the existing CTest suite.

Do not merge from an earlier green SHA.

- [ ] **Step 5: Review the final PR diff**

Confirm changed files are limited to the F.1 spec/plan/work record, public distributed API, partition implementation/tests, CMake registration, package consumer, README, and CHANGELOG. Reject unrelated historical wording changes or capability drift.

- [ ] **Step 6: Record completion and merge only the exact green head**

Update PR/roadmap evidence with RED SHA/run, final exact green SHA/run, scope/non-claims, and merge commit. Merge to `main` only after rechecking the PR head SHA is unchanged and mergeable.

The next Stage F milestone after F.1 should be selected from the remaining architecture gates using merged evidence; do not assume F.2's exact API until the post-merge audit is performed.
