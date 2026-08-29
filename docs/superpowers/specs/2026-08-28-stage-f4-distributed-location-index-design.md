# Stage F.4 — bounded distributed location index design

## Status

Approved under the repository's standing autonomous milestone authorization. This design starts from merged Stage F.3 at `56ce57a40bcfeeff97598c6d3afb4d58e3d7c25b`.

## Goal

Add one bounded, deterministic, provider-neutral in-memory location index that maps exact F.1 physical record membership to zero or more existing F.3 `DistributedRecordLocation` placement candidates.

F.4 establishes **how a caller can index known exact placements and resolve an F.1 partition into ordered placement candidate sets**. It does not choose a candidate, choose a backend, retrieve bytes, schedule work, discover locations over a network, or persist a global catalog.

The central invariant remains unchanged:

> exact work membership and logical stream identity are independent of physical archive/object placement.

## Existing substrate

F.4 builds only on merged generic contracts:

- F.1 `DistributedPartition` defines one stable `StreamId`, ordered exact `ProvenanceRecordLink` membership, aggregate payload bytes, and deterministic `CDP1` identity.
- F.2 `execute_partition()` validates one materialized partition before one worker invocation.
- F.3 `DistributedRecordLocation` binds one original `RecordInfo` to opaque object-store `store`/`key`/`version` plus an independent byte `offset`/`length`.
- F.3 `retrieve_partition_records()` requires exactly one ordered location per partition member and independently verifies range length and payload SHA-256 before returning `ExtractedRecord`s.
- `ProvenanceRecordLink` deliberately omits archive/object placement.
- `RecordInfo::file_offset` is original archive metadata, not object-store addressing.
- `Sha256`/`sha256()` are integrity evidence only, not authentication.

Merged F.3 intentionally left location discovery/indexing unimplemented and identified it as the next Stage F dependency.

## Approaches considered

### A. Immutable exact-link index over existing F.3 locations — selected

Build a bounded in-memory index from caller-supplied `DistributedRecordLocation` descriptors. Group candidates by exact `ProvenanceRecordLink`, require all candidates for one link to preserve identical original `RecordInfo`, deduplicate exact duplicate placements, canonical-sort entries/candidates, and resolve F.1 partition membership into ordered candidate sets under explicit query bounds.

This reuses the F.3 placement contract unchanged, preserves F.1 identity semantics, provides deterministic lookup independent of registration order, and leaves candidate selection/provider policy external.

### B. Add locations to `DistributedPartition` — rejected

This would make `CDP1` or partition semantics depend on current storage topology and violate the architecture separation between logical work membership and archive placement.

### C. Add a provider-aware/global network catalog — deferred

A network catalog would require endpoint discovery, authentication, consistency/replication semantics, persistence, cache invalidation, update protocols, availability policy, and deployment choices. Those are later Stage F operational/deployment concerns. F.4 fixes the deterministic index semantics that a future catalog implementation may populate or wrap.

## Public API

Extend `<codec/distributed.hpp>` additively:

```cpp
struct DistributedLocationIndexLimits {
  std::size_t maximum_input_locations{65536};
  std::size_t maximum_records{16384};
  std::size_t maximum_locations_per_record{16};
  std::uint64_t maximum_metadata_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_store_bytes{512};
  std::size_t maximum_key_bytes{4096};
  std::size_t maximum_version_bytes{512};
};

struct DistributedLocationQueryLimits {
  std::size_t maximum_records{1024};
  std::size_t maximum_candidates_per_record{16};
  std::size_t maximum_candidates{4096};
};

struct DistributedLocationIndexEntry {
  RecordInfo record{};
  std::vector<DistributedRecordLocation> candidates;
};

class DistributedLocationIndex {
 public:
  DistributedLocationIndex() = default;

  std::size_t record_count() const noexcept;
  std::size_t location_count() const noexcept;
  std::span<const DistributedLocationIndexEntry> entries() const noexcept;

 private:
  std::vector<DistributedLocationIndexEntry> entries_;
  std::size_t location_count_{};

  friend Result<DistributedLocationIndex> build_distributed_location_index(
      std::span<const DistributedRecordLocation>,
      DistributedLocationIndexLimits);
  friend Result<DistributedPartitionLocationCandidates>
  resolve_partition_location_candidates(
      const DistributedLocationIndex&,
      const DistributedPartition&,
      DistributedLocationQueryLimits);
};

struct DistributedLocationCandidateSet {
  ProvenanceRecordLink record{};
  std::vector<DistributedRecordLocation> candidates;
};

struct DistributedPartitionLocationCandidates {
  Sha256 partition_identity{};
  StreamId stream{};
  bool complete{};
  std::vector<DistributedLocationCandidateSet> records;
};

Result<DistributedLocationIndex> build_distributed_location_index(
    std::span<const DistributedRecordLocation> locations,
    DistributedLocationIndexLimits limits = {});

Result<DistributedPartitionLocationCandidates>
resolve_partition_location_candidates(
    const DistributedLocationIndex& index,
    const DistributedPartition& partition,
    DistributedLocationQueryLimits limits = {});
```

Declaration order may be adjusted for C++ friend/type completeness, and names may change only to resolve an existing collision. Semantics are fixed by this design.

The index exposes read-only canonical entries for inspection/testing but no mutation API. Updating an index means building a new index from a new descriptor snapshot.

## Exact record key

The index key is exactly the existing physical `ProvenanceRecordLink` tuple:

- `StreamId`;
- raw 16-bit record type code;
- archive sequence;
- payload SHA-256.

No object-store field, original archive `file_offset`, interval, worker, region, backend label, or deployment attribute enters the key or F.1 `CDP1`.

## Replica metadata consistency

Multiple indexed locations may represent replicas/placements of the same exact physical record link.

All candidate descriptors grouped under one exact link must carry identical complete original `RecordInfo` field-for-field:

- raw type code;
- archive sequence;
- stream;
- `start_ns`;
- `end_ns`;
- `payload_size`;
- original archive `file_offset`;
- payload SHA-256.

A descriptor with the same link but conflicting interval, payload size, or original file offset is rejected as `invalid_argument`. F.4 must not silently choose one metadata version.

## Location validation

Every input `DistributedRecordLocation` must satisfy the static F.3 placement shape before indexing:

1. `record.end_ns >= record.start_ns`;
2. `length == record.payload_size`;
3. `offset + length` does not overflow `uint64_t`;
4. `object.store` is non-empty;
5. `object.key` is non-empty;
6. store/key/version byte lengths are within configured bounds.

Zero-length record payloads remain valid.

F.4 performs no backend call and therefore does not assert that a syntactically valid location currently exists or is readable.

## Build limits

All index limits must be non-zero.

`build_distributed_location_index()` applies bounds transactionally:

- input descriptor count <= `maximum_input_locations`;
- unique exact record count <= `maximum_records`;
- unique placement candidate count for one record <= `maximum_locations_per_record`;
- overflow-safe sum of input store/key/version byte lengths <= `maximum_metadata_bytes`;
- per-string bounds above.

The aggregate metadata bound counts every input descriptor before deduplication. Repeating the same placement cannot bypass resource accounting.

An empty input span is valid and produces an empty index.

## Duplicate and candidate identity semantics

Two descriptors are the same placement candidate only when all placement fields match exactly:

- `object.store`;
- `object.key`;
- `object.version`;
- `offset`;
- `length`.

Because all candidates for a link already require identical `RecordInfo`, that placement tuple is sufficient for duplicate detection within one entry.

Exact duplicate placements are idempotently deduplicated. They do not appear twice in the stored candidate vector and do not consume multiple `maximum_locations_per_record` slots, though each input descriptor still counts toward input/metadata bounds.

Two candidates that differ by version, object key/store, offset, or length remain distinct.

## Canonical ordering

Index results must not depend on caller registration order.

### Entry order

Entries are canonical-sorted by exact link fields in this order:

1. `StreamId` bytes lexicographically;
2. raw record type code ascending;
3. archive sequence ascending;
4. SHA-256 bytes lexicographically.

### Candidate order

Candidates within one entry are canonical-sorted by:

1. `object.store` bytewise lexicographic order;
2. `object.key` bytewise lexicographic order;
3. `object.version` bytewise lexicographic order;
4. `offset` ascending;
5. `length` ascending.

Equivalent inputs in any permutation therefore build observably equivalent canonical indexes.

Candidate ordering is not a health, locality, cost, latency, durability, trust, preference, or failover ranking. F.4 attaches no such semantics.

## Partition resolution

`resolve_partition_location_candidates()` maps one F.1 partition onto the immutable index.

### Query preflight

Before assembling output:

1. every query limit must be non-zero;
2. the partition must be non-empty;
3. partition member count <= `maximum_records`;
4. every partition link must use exactly `partition.stream`;
5. recomputed existing F.1 `CDP1` identity must equal `partition.identity`.

A tampered partition fails before any result is returned.

### Resolution behavior

For each partition link in exact partition order:

- look up the exact link in the canonical index;
- if found, copy its canonical candidate vector into one `DistributedLocationCandidateSet`;
- if not found, return the exact requested link with an empty candidate vector;
- never substitute a same-stream/same-sequence location whose type or hash differs;
- never reorder partition members.

The result preserves:

- `partition.identity`;
- `partition.stream`;
- exactly one candidate set per partition member in exact membership order.

`complete` is `true` iff every returned candidate set is non-empty. An empty candidate set is an explicit availability-of-metadata result only; it is not proof that the record exists nowhere else.

Missing indexed placement is not an `Error` because the index is a bounded snapshot and absence is a normal query result. CODEC currently has no generic `not_found` error category, and F.4 does not invent one solely for this case.

## Query output bounds

Resolution must fail with `resource_exhausted` rather than silently truncate when:

- any matched record has more candidates than `maximum_candidates_per_record`;
- the overflow-safe aggregate matched candidate count exceeds `maximum_candidates`.

Missing records contribute zero candidates.

No partial/truncated `DistributedPartitionLocationCandidates` is returned on failure.

## Composition with F.3

F.4 intentionally returns candidate sets rather than an F.3-ready single location vector.

A caller may apply its own external placement/backend policy, choose exactly one candidate per complete record set, then pass that ordered selection to `retrieve_partition_records()`.

F.3 independently revalidates the chosen descriptors, exact F.1 membership, returned byte lengths, and SHA-256. F.4 does not weaken or bypass those checks.

The installed-package proof should demonstrate:

```text
F.1 partition
  -> F.4 index + candidate resolution
  -> caller chooses first canonical candidate
  -> F.3 exact retrieval through caller backend
  -> F.2 exact execution
```

This is composition evidence only. Selecting the first canonical candidate in a test is not a production routing recommendation.

## Allocation failures

Any `std::bad_alloc` crossing F.4 build or resolution is converted to `resource_exhausted`.

F.4 has no provider callback, so it does not need provider-exception semantics.

## Error model

### Build

- any zero index limit -> `invalid_argument`;
- malformed interval, payload-length mismatch, range overflow, empty store/key, or conflicting `RecordInfo` for one exact link -> `invalid_argument`;
- excessive input locations, unique records, unique candidates per record, per-string bytes, or aggregate metadata bytes -> `resource_exhausted`;
- allocation failure -> `resource_exhausted`.

### Resolve

- any zero query limit -> `invalid_argument`;
- empty/tampered/internally inconsistent F.1 partition -> `invalid_argument`;
- excessive partition records or candidate expansion -> `resource_exhausted`;
- missing exact link -> successful empty candidate set and `complete=false`;
- allocation failure -> `resource_exhausted`.

## Internal module structure

Keep Stage F isolated:

- `include/codec/distributed.hpp` — additive F.4 index/query API alongside F.1–F.3.
- `src/distributed/location_index.cpp` — F.4 validation, canonicalization, deduplication, and resolution.
- `src/distributed/internal.hpp` / `identity.cpp` — reuse existing exact-link and `CDP1` helpers without byte changes.
- `tests/test_distributed_location_index.cpp` — focused canonicalization, conflict, limit, missing-entry, tampered-partition, and F.3 handoff tests.
- `tests/package_consumer/main.cpp` — installed-package F.1→F.4→F.3→F.2 composition.

No archive, transport, audio/profile, CLI, C ABI, or provider client refactor is part of F.4.

## Compatibility and non-goals

F.4 is additive and changes none of:

- CODA development-profile bytes or record semantics;
- S0/S1/D definitions;
- provenance encoding;
- F.1 `DistributedPartition` fields or `CDP1` encoding;
- F.2 worker/execution semantics;
- F.3 location descriptor or retrieval semantics;
- `StreamId`, clocks, epochs, archive sequences, or source truth;
- Stage E transport/recovery contracts;
- C ABI or CLI behavior.

F.4 explicitly does **not** add:

- a persistent/on-disk index format;
- a distributed/network index service or global archive catalog;
- object-store list/discovery calls;
- a bundled S3/GCS/Azure/HTTP/filesystem client;
- credentials, signing, authorization, secrets, capability tokens, or endpoint policy;
- candidate health checks, preference scores, locality, cost, latency, durability, trust, or placement policy;
- automatic candidate selection, routing, retry, failover, replication, or repair;
- object reads/writes/uploads/deletes;
- multi-partition scheduling, RPC/socket worker execution, queues, thread pools, leases, heartbeats, checkpointing, or exactly-once behavior;
- automatic CODA persistence;
- deployment integrations;
- throughput, latency, scale, availability, durability, fault-tolerance, or cost claims.

## Required tests

1. empty input builds a valid empty index;
2. valid descriptors build one entry per exact physical link and preserve full `RecordInfo` plus all distinct placements;
3. input permutations produce identical canonical entry/candidate ordering;
4. exact duplicate placements deduplicate idempotently;
5. same exact link with conflicting interval, payload size, or original `file_offset` fails `invalid_argument`;
6. unknown raw record type codes are preserved in index entries and query results;
7. malformed interval, length mismatch, range overflow, or empty store/key fails build;
8. zero build limits fail `invalid_argument`;
9. input-count, unique-record, per-record-candidate, per-string, and aggregate metadata limits fail `resource_exhausted`;
10. a valid F.1 partition resolves candidate sets in exact partition membership order even though the index itself is link-sorted;
11. a missing exact link returns an empty candidate set at the correct position and `complete=false`; fully covered partitions return `complete=true`;
12. same stream/sequence but wrong type/hash is not treated as a match;
13. zero query limits fail `invalid_argument`;
14. tampered partition stream/membership/identity fails `invalid_argument`;
15. partition-record and per-record/aggregate candidate output bounds fail `resource_exhausted` with no truncation;
16. F.4 performs no `ObjectStoreBackend` calls;
17. chosen F.4 candidates pass unchanged to existing F.3 retrieval and the retrieved records pass unchanged to F.2 execution;
18. existing F.1/F.2/F.3 and repository tests remain green;
19. installed-package consumer can build/resolve F.4, choose candidates externally, retrieve through F.3, and execute through F.2.

## Stage F dependency direction after F.4

F.1 defines deterministic exact work membership.

F.2 defines bounded execution of one materialized partition.

F.3 defines explicit storage placement descriptors and exact materialization through a caller-supplied backend.

F.4 defines deterministic bounded indexing/resolution of known placements for exact partition membership.

After F.4 merges, audit Stage F again. Remaining architecture items include multi-partition scheduling/remote execution, operational benchmarks, and deployment integrations. Do not assume their ordering or freeze an F.5 API until the merged F.4 contract is available.
