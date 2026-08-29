# Stage F.4 Distributed Location Index Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded deterministic in-memory index from existing F.3 `DistributedRecordLocation` descriptors and resolve exact F.1 partition membership into ordered candidate sets without performing storage I/O or changing partition identity.

**Architecture:** F.4 extends `<codec/distributed.hpp>` additively and keeps all implementation in `src/distributed/location_index.cpp`. The index owns a canonical immutable vector of exact-link entries; build validates F.3 descriptor shape, rejects metadata conflicts, deduplicates exact placements, and canonical-sorts. Resolution revalidates F.1 `CDP1`, preserves partition order, reports missing placements explicitly, and fails rather than truncating when output bounds are exceeded.

**Tech Stack:** C++20, existing `codec::Result`, `RecordInfo`, `ProvenanceRecordLink`, `DistributedPartition`, `DistributedRecordLocation`, SHA-256 helpers, CMake/CTest, GitHub Actions GCC/Clang/ASan+UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-f4-distributed-location-index-design.md`

## Global Constraints

- Start from exact merged F.3 `main` SHA `56ce57a40bcfeeff97598c6d3afb4d58e3d7c25b` on branch `automation/stage-f4-distributed-location-index`.
- Do not change CODA bytes, S0/S1/D definitions, provenance encoding, F.1 `DistributedPartition` fields or `CDP1`, F.2 execution, F.3 retrieval/location semantics, Stage E, C ABI, or CLI behavior.
- Reuse `DistributedRecordLocation`; do not create a second placement schema.
- F.4 performs zero `ObjectStoreBackend` calls.
- All candidates for one exact link must preserve identical complete `RecordInfo`.
- Canonical entry/candidate order must be independent of input order and must not imply placement preference.
- Missing indexed placements are successful empty candidate sets with `complete=false`.
- Query bounds fail with `resource_exhausted`; never silently truncate.
- No persistent/global/network catalog, provider client, auth, routing, retry/failover, scheduler/RPC, deployment integration, or scale claim.

---

### Task 1: Establish the intentional RED proof

**Files:**
- Create: `tests/test_distributed_location_index.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing F.1/F.3 helpers and public types only.
- Produces: a test suite requiring `DistributedLocationIndexLimits`, `DistributedLocationQueryLimits`, `DistributedLocationIndex`, `DistributedLocationCandidateSet`, `DistributedPartitionLocationCandidates`, `build_distributed_location_index()`, and `resolve_partition_location_candidates()` before those symbols exist.

- [ ] **Step 1: Add focused test helpers and wished-for behavior**

Create `tests/test_distributed_location_index.cpp` with helpers equivalent to:

```cpp
#include "test.hpp"
#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

namespace {
std::vector<std::byte> bytes(std::string_view value) {
  const auto raw = std::as_bytes(std::span{value.data(), value.size()});
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

codec::DistributedRecordLocation location_for(
    const codec::ExtractedRecord& input,
    std::string store,
    std::string key,
    std::string version,
    std::uint64_t offset) {
  return {.record = input.record,
          .object = {.store = std::move(store),
                     .key = std::move(key),
                     .version = std::move(version)},
          .offset = offset,
          .length = input.record.payload_size};
}
}
```

Add tests proving at minimum:

```cpp
TEST(distributed_location_index_empty_build_is_valid) {
  const std::vector<codec::DistributedRecordLocation> locations;
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);
  EXPECT_EQ(index->record_count(), std::size_t{0});
  EXPECT_EQ(index->location_count(), std::size_t{0});
  EXPECT_TRUE(index->entries().empty());
}

TEST(distributed_location_index_canonicalizes_deduplicates_and_preserves_metadata) {
  // Build A/B locations in reverse order with one exact duplicate.
  // Expect one entry for A, two unique candidates canonical-sorted by
  // store/key/version/offset/length, and exact RecordInfo preserved.
}

TEST(distributed_location_index_resolves_partition_order_and_missing_members) {
  // Index only records 1 and 3, resolve partition [1,2,3].
  // Expect candidate sets [nonempty, empty, nonempty], complete=false,
  // exact partition identity/stream, and exact partition link order.
}

TEST(distributed_location_index_rejects_conflicting_replica_metadata) {
  // Same exact link/hash but change file_offset or interval in second replica.
  // Expect invalid_argument.
}

TEST(distributed_location_index_bounds_fail_without_truncation) {
  // Exercise zero limits, input count, unique records, per-record candidates,
  // aggregate metadata, query record count, per-record output, aggregate output.
}
```

Also cover unknown raw type preservation, malformed descriptor shape, range overflow, wrong hash/type non-match, tampered `CDP1`, and input-order-independent canonical equality.

- [ ] **Step 2: Register only the new test source**

Add `tests/test_distributed_location_index.cpp` beside the existing distributed tests in `codec_tests`; do not add any production source yet.

- [ ] **Step 3: Open the PR on this test-only head**

PR title: `Stage F.4: add bounded distributed location index`.

State explicitly that the head is intentionally RED because the F.4 public symbols and `location_index.cpp` do not exist.

- [ ] **Step 4: Run CI and verify the intended RED cause**

Expected compiler failure: missing F.4 types/functions from `<codec/distributed.hpp>`. Existing project configuration/core sources must get far enough to show this is the requested-behavior failure, not CMake damage.

- [ ] **Step 5: Commit evidence in the PR/roadmap comment**

Record the exact RED SHA and CI run number before implementation.

---

### Task 2: Add the immutable public index API and canonical builder

**Files:**
- Modify: `include/codec/distributed.hpp`
- Create: `src/distributed/location_index.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_distributed_location_index.cpp`

**Interfaces:**
- Consumes: `DistributedRecordLocation`, `RecordInfo`, `ProvenanceRecordLink`, `detail::distributed_exact_link()`.
- Produces:

```cpp
struct DistributedLocationIndexLimits;
struct DistributedLocationQueryLimits;
struct DistributedLocationIndexEntry;
struct DistributedLocationCandidateSet;
struct DistributedPartitionLocationCandidates;
class DistributedLocationIndex;
Result<DistributedLocationIndex> build_distributed_location_index(...);
Result<DistributedPartitionLocationCandidates>
resolve_partition_location_candidates(...);
```

- [ ] **Step 1: Add the exact public declarations**

Use the signatures/defaults from the F.4 design spec. Put `DistributedLocationCandidateSet` and `DistributedPartitionLocationCandidates` before the class if needed for friend declaration completeness. Keep `entries_` and `location_count_` private and expose only const inspection methods.

- [ ] **Step 2: Implement build-limit and descriptor validation**

In `src/distributed/location_index.cpp`, add helpers equivalent to:

```cpp
Result<void> validate_index_limits(const DistributedLocationIndexLimits& l);
Result<void> validate_location_shape(const DistributedRecordLocation& x,
                                     const DistributedLocationIndexLimits& l);
bool same_record_info(const RecordInfo& a, const RecordInfo& b);
bool same_placement(const DistributedRecordLocation& a,
                    const DistributedRecordLocation& b);
```

`same_record_info()` must compare type code, sequence, stream, interval, payload size, original file offset, and hash. `validate_location_shape()` must enforce interval, `length == payload_size`, overflow-safe range, non-empty store/key, and per-string bounds.

- [ ] **Step 3: Implement canonical comparators**

Exact-link comparator order:

```text
stream bytes -> raw type -> sequence -> hash bytes
```

Candidate comparator order:

```text
store -> key -> version -> offset -> length
```

Do not include health/cost/preference metadata because F.4 has none.

- [ ] **Step 4: Implement transactional build**

Algorithm:

```cpp
Result<DistributedLocationIndex> build_distributed_location_index(...) {
  try {
    validate limits;
    reject input count over maximum_input_locations;
    preflight every descriptor and sum every input store/key/version byte count
      with overflow-safe maximum_metadata_bytes accounting;
    copy input locations;
    canonical-sort by exact link, then placement;
    group equal exact links;
      reject non-identical RecordInfo within group;
      deduplicate exact placement duplicates;
      reject unique candidate count > maximum_locations_per_record;
      append canonical entry;
    reject unique entry count > maximum_records;
    set unique stored location_count_;
    return immutable index;
  } catch (const std::bad_alloc&) {
    return fail<DistributedLocationIndex>(ErrorCode::resource_exhausted,
                                         "distributed location index allocation failed");
  }
}
```

Empty input returns an empty index.

- [ ] **Step 5: Implement const inspection methods**

```cpp
std::size_t DistributedLocationIndex::record_count() const noexcept {
  return entries_.size();
}
std::size_t DistributedLocationIndex::location_count() const noexcept {
  return location_count_;
}
std::span<const DistributedLocationIndexEntry>
DistributedLocationIndex::entries() const noexcept {
  return entries_;
}
```

- [ ] **Step 6: Register `src/distributed/location_index.cpp` in `codec_core`**

Place it with `identity.cpp`, `partition.cpp`, `retrieval.cpp`, and `worker.cpp`.

- [ ] **Step 7: Run CI on the unchanged RED tests**

Expected: build succeeds and canonical-build tests pass; resolver tests may pass only after Task 3 implementation if the file is implemented in two commits.

---

### Task 3: Implement bounded F.1 partition resolution

**Files:**
- Modify: `src/distributed/location_index.cpp`
- Test: `tests/test_distributed_location_index.cpp`

**Interfaces:**
- Consumes: immutable `DistributedLocationIndex`, F.1 `DistributedPartition`, `detail::distributed_partition_identity()`.
- Produces: `resolve_partition_location_candidates()`.

- [ ] **Step 1: Implement query-limit validation**

Zero `maximum_records`, `maximum_candidates_per_record`, or `maximum_candidates` -> `invalid_argument`.

- [ ] **Step 2: Revalidate F.1 partition before lookup**

Require non-empty membership, member count within query bound, every link stream equal to `partition.stream`, and recomputed exact `CDP1` equal to `partition.identity`.

- [ ] **Step 3: Resolve each member by exact link**

Binary-search canonical `entries_` using the same exact-link comparator. For each partition member in original membership order:

```cpp
DistributedLocationCandidateSet set;
set.record = link;
if (entry found) set.candidates = entry.candidates;
else set.candidates.clear();
```

Never substitute same stream/sequence with a different type/hash.

- [ ] **Step 4: Enforce output bounds before success**

For each matched entry, if its candidate count exceeds `maximum_candidates_per_record`, return `resource_exhausted`. Maintain an overflow-safe aggregate candidate count and fail when `maximum_candidates` would be exceeded. Never truncate.

- [ ] **Step 5: Set completeness exactly**

`complete=true` iff every candidate set is non-empty. Preserve exact `partition.identity`, `partition.stream`, and one set per partition member.

- [ ] **Step 6: Convert `std::bad_alloc` to `resource_exhausted`**

Use a dedicated message such as `"distributed location query allocation failed"`.

- [ ] **Step 7: Run full F.4 tests and existing distributed tests**

CI must prove canonical build, conflicts/bounds, missing-member behavior, tampered partition rejection, and no regression in F.1/F.2/F.3.

---

### Task 4: Prove installed-package F.4 → F.3 → F.2 composition

**Files:**
- Modify: `tests/package_consumer/main.cpp`

**Interfaces:**
- Consumes: installed `<codec/distributed.hpp>` F.1–F.4 API.
- Produces: an external-style composition proof using a caller-owned `ObjectStoreBackend`.

- [ ] **Step 1: Extend the existing distributed package-consumer scenario**

After building the two-record F.1 partition and before F.3 retrieval, create at least one location per record, build F.4 index, and resolve the partition:

```cpp
auto location_index = codec::build_distributed_location_index(locations);
if (!location_index) return 1;
auto candidates = codec::resolve_partition_location_candidates(
    *location_index, partitions->front());
if (!candidates || !candidates->complete || candidates->records.size() != 2)
  return 1;

std::vector<codec::DistributedRecordLocation> selected;
for (const auto& set : candidates->records) {
  if (set.candidates.empty()) return 1;
  selected.push_back(set.candidates.front());
}
```

Pass `selected` unchanged into existing F.3 `retrieve_partition_records()`, then pass returned records unchanged into F.2 `execute_partition()`.

- [ ] **Step 2: Run package CI**

Require GCC and Clang install/package-consumer steps to pass, plus sanitizer tests.

---

### Task 5: Synchronize current Stage F claims only after code/package proof

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: green implementation/package evidence.
- Produces: truthful current-status claims.

- [ ] **Step 1: Add one implemented-v0.1 generic capability bullet**

Describe bounded deterministic in-memory exact-location indexing/resolution, canonical deduplication, explicit incomplete candidate sets, and unchanged F.1/F.3 semantics.

- [ ] **Step 2: Update `planned_not_implemented`**

Remove only the now-implemented bounded in-memory location index. Keep persistent/global/network indexes/catalogs, discovery services, routing/failover, scheduling/RPC, benchmarks, and deployment explicitly planned.

- [ ] **Step 3: Add an F.4 paragraph to `Distributed Processing Profile`**

State exact behavior and non-claims. Do not imply canonical candidate order is preference/routing policy.

- [ ] **Step 4: Add one `Unreleased` CHANGELOG bullet**

Mention public API, deterministic index semantics, F.1 partition-resolution proof, installed-package F.4→F.3→F.2 composition, and explicit non-scope.

- [ ] **Step 5: Inspect README/CHANGELOG patches for unrelated text churn**

Restore any accidental historical, Audio, security, formatting, or newline changes before final CI.

---

### Task 6: Exact-head verification, review, roadmap evidence, and guarded merge

**Files:**
- No additional source changes expected.

- [ ] **Step 1: Audit exact base-to-head diff**

Expected paths only:

```text
AI_WORKSHEET.md
CHANGELOG.md
CMakeLists.txt
README.md
docs/superpowers/plans/2026-08-28-stage-f4-distributed-location-index.md
docs/superpowers/specs/2026-08-28-stage-f4-distributed-location-index-design.md
include/codec/distributed.hpp
src/distributed/location_index.cpp
tests/package_consumer/main.cpp
tests/test_distributed_location_index.cpp
```

`src/distributed/internal.hpp`/`identity.cpp` should remain unchanged unless compilation proves an exact helper exposure is required; any such change must not alter `CDP1` bytes.

- [ ] **Step 2: Run/observe final exact-head CI**

Require all of:

```text
build (gcc): configure/build/test/install/package consumer = success
build (clang): configure/build/test/install/package consumer = success
sanitizers: configure/build/test = success
```

- [ ] **Step 3: Review the exact final diff**

Check specifically: F.1 `CDP1` unchanged, F.3 retrieval unchanged, no provider/backend calls in F.4, full `RecordInfo` conflict rejection, canonical/dedup semantics, no candidate truncation, missing-entry semantics, and no status overclaim.

- [ ] **Step 4: Ensure zero unresolved PR review threads**

Fix Critical/Important findings before proceeding; record a no-findings review if clean.

- [ ] **Step 5: Update PR body from RED state to final evidence**

Include RED SHA/run, final exact green SHA/run, package proof, changed paths, non-claims, review status, and base SHA.

- [ ] **Step 6: Guarded squash merge**

Merge only with `expected_head_sha` equal to the exact green reviewed head. If head moved, re-run the exact-head gate first.

- [ ] **Step 7: Verify post-merge `main` and roadmap issue**

Confirm `main` points to the returned merge commit and PR is merged. Add issue #10 completion evidence plus a fresh Stage F dependency audit; do not freeze F.5 unless the merged tree proves the next dependency.
