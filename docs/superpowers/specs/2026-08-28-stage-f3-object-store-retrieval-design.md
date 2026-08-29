# Stage F.3 — bounded object-store record retrieval design

## Status

Approved under the repository's standing autonomous milestone authorization. This design starts from merged Stage F.2 at `c685cdcc06518478cb7390b3abfc71f2cdc32692`.

## Goal

Add one provider-neutral, bounded object-store read boundary that translates explicit archive-placement metadata into exact verified `ExtractedRecord` inputs for an existing F.1 `DistributedPartition`.

F.3 establishes **how exact partition members can be located in caller-selected object storage and materialized as verified in-memory records**. The returned records must be directly consumable by F.2 `execute_partition()` without changing F.1 membership semantics or teaching a worker where storage lives.

F.3 does not schedule work, execute a worker remotely, discover locations, upload outputs, or define a cloud-vendor deployment.

## Existing substrate

F.3 builds only on merged generic contracts:

- F.1 `DistributedPartition` defines one stable `StreamId`, ordered exact `ProvenanceRecordLink` membership, aggregate payload bytes, and deterministic `CDP1` identity.
- F.2 `execute_partition()` requires full ordered `ExtractedRecord` inputs, rechecks exact links and payload SHA-256, and executes one worker only after those checks pass.
- `RecordInfo` preserves raw type, archive sequence, stream, interval, payload size, original file offset, and SHA-256.
- `ProvenanceRecordLink` deliberately omits archive placement.
- `Sha256`/`sha256()` are integrity evidence only, not authentication.
- Logical stream identity, processing partition, and archive placement are distinct architecture concepts.

## Approaches considered

### A. Caller-supplied object-store backend plus explicit record locations — selected

Define an opaque provider-neutral object reference, a location descriptor that combines one original `RecordInfo` with an independent object byte range, a caller-supplied read-only `ObjectStoreBackend`, and a `retrieve_partition_records()` wrapper that verifies the complete request before fetching and verifies every returned payload before success.

This preserves F.1/F.2 semantics, keeps credentials/endpoints/vendor SDKs outside generic core, and creates the exact location-to-record boundary needed by a later distributed index.

### B. Add object locations directly to `DistributedPartition` — rejected

Embedding placement into F.1 would make deterministic work membership depend on storage topology and would violate the architecture invariant that logical stream identity, processing partition, and archive placement are separate. Partition identity must remain the existing `CDP1` ordered membership digest.

### C. Bundle S3/GCS/Azure or generic HTTP clients in F.3 — deferred

A concrete cloud/network client would force premature choices about endpoints, credentials, signing, redirects, proxy/private-network policy, retries, SDK lifecycle, and deployment configuration. Those are deployment/provider concerns. F.3 instead fixes the backend contract and exact retrieval semantics that such adapters must obey.

## Public API

Extend `<codec/distributed.hpp>` additively:

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

Names may be adjusted only for an existing repository naming collision; semantics are fixed by this design.

## Object-reference semantics

`ObjectStoreObjectRef` is opaque placement metadata interpreted only by the supplied backend.

- `store` is a non-empty bounded logical object-store namespace. A provider may map it to a bucket, container, archive namespace, or equivalent concept.
- `key` is a non-empty bounded object key inside that store.
- `version` is a bounded optional immutable-version/generation token. Empty means no backend-specific immutable version was supplied.
- These strings are not URLs, filesystem paths, credentials, capability tokens, signatures, or authorization claims at the generic F.3 layer.
- The backend instance owns endpoint, credential, transport, authorization, and provider-specific interpretation outside this generic contract.
- Even when `version` is supplied, exactness is established only by the record SHA-256 check after retrieval.

F.3 does not parse, normalize, concatenate, escape, or reinterpret object-reference strings.

## Record-location semantics

`DistributedRecordLocation` separates original record metadata from current storage placement.

- `record` is the original physical `RecordInfo` whose exact link must match one ordered F.1 partition member.
- `record.file_offset` is preserved original archive metadata and is never used by F.3 to address object storage.
- `object`, `offset`, and `length` describe the current object-store byte range containing exactly that record payload and only that payload.
- `length` must equal `record.payload_size`.
- `offset + length` must not overflow `uint64_t`.
- Zero-length record payloads are valid and produce a zero-length backend range request.

No location field changes `StreamId`, raw record type, archive sequence, interval, hash, or truth class.

## Preflight validation before object reads

`retrieve_partition_records()` must fail before calling `backend.read_range()` unless the complete request passes all checks below.

### Limits and basic shape

1. Every retrieval limit must be non-zero.
2. The partition and location batch must both be non-empty.
3. `partition.records.size()` must equal `locations.size()`.
4. Location count must not exceed `maximum_records`.
5. `backend.name()` must be non-empty and within `maximum_backend_name_bytes`.

### Partition self-consistency

6. Every partition link must use exactly `partition.stream`.
7. Recompute the existing canonical F.1 `CDP1` identity using the shared private distributed helper; it must equal `partition.identity`.
8. The overflow-safe sum of all location payload lengths must equal `partition.payload_bytes` and must not exceed `maximum_bytes`.

### Ordered location binding

For each location at index `i`:

9. `location.record.stream` must equal `partition.stream`.
10. The exact link derived from `location.record` `(stream, raw type code, archive sequence, hash)` must equal `partition.records[i]` field-for-field.
11. `location.record.end_ns` must not precede `location.record.start_ns`.
12. `location.length` must equal `location.record.payload_size`.
13. `location.offset + location.length` must not overflow `uint64_t`.
14. `object.store` and `object.key` must be non-empty.
15. `store`, `key`, and optional `version` byte lengths must fit their configured limits.

Input order is significant because `CDP1` commits ordered membership. Reordered otherwise-valid locations are rejected before any backend read.

## Backend read contract

After successful preflight, F.3 reads each location sequentially in partition order.

For each location:

1. call `backend.read_range(location.object, location.offset, location.length)` exactly once;
2. if the backend returns an `Error`, propagate it unchanged and stop; no retry occurs;
3. require the returned vector length to equal `location.length`; a short or long provider result is a `protocol` error;
4. require `sha256(returned_bytes) == location.record.hash`; mismatch is `archive_corrupt`;
5. construct one `ExtractedRecord` by copying the exact supplied `RecordInfo` and moving in the verified bytes.

No partial `DistributedRetrievalResult` is returned on failure. Reads completed before a later failure are ordinary provider side effects; F.3 does not claim transactional remote I/O.

The wrapper performs no caching, range coalescing, speculative prefetch, parallel reads, retry, failover, replication, repair, decompression, decryption, or content transformation.

## Retrieval result

On success, `DistributedRetrievalResult` contains:

- the exact verified F.1 `partition.identity`;
- the exact partition `StreamId`;
- the bounded descriptive backend name observed for the retrieval;
- ordered verified `ExtractedRecord`s whose metadata exactly matches the supplied locations and whose payloads satisfy the committed record hashes.

`result.records` may be passed directly to F.2 `execute_partition()`; F.2 intentionally revalidates membership and payload hashes rather than trusting F.3 by position or type.

The result is not an authenticated receipt, storage proof, availability guarantee, archive index, access-control decision, or remote attestation.

## Allocation and provider exceptions

Any `std::bad_alloc` crossing `retrieve_partition_records()` is converted to `resource_exhausted`, including allocation failure during provider invocation or result assembly. F.3 does not catch or reinterpret arbitrary provider exceptions.

## Error model

- zero limits -> `invalid_argument`;
- empty partition/locations, count mismatch, wrong stream/link/order, bad `CDP1`, interval inversion, length/payload-size mismatch, range overflow, empty store/key, or partition payload-byte mismatch -> `invalid_argument`;
- record count, aggregate bytes, backend-name length, store/key/version length -> `resource_exhausted`;
- backend/provider `Error` -> propagated unchanged;
- provider returns a byte count different from the requested range -> `protocol`;
- returned payload SHA-256 disagrees with `RecordInfo::hash` -> `archive_corrupt`;
- allocation failure -> `resource_exhausted`.

No error automatically retries another object, version, endpoint, backend, or worker.

## Internal module structure

Keep Stage F isolated:

- `include/codec/distributed.hpp` — additive F.3 public object-store/retrieval API alongside F.1/F.2.
- `src/distributed/retrieval.cpp` — F.3 validation and materialization implementation.
- `src/distributed/internal.hpp` / `identity.cpp` — reuse existing exact-link and `CDP1` helpers; no `CDP1` byte change.
- `tests/test_distributed_retrieval.cpp` — focused provider-contract, integrity, limit, error, and F.2 handoff tests.
- `tests/package_consumer/main.cpp` — installed-package use with a tiny caller-supplied backend and F.2 handoff.

No archive, transport, audio/profile, CLI, or C ABI source refactor is part of F.3.

## Compatibility and non-goals

F.3 is additive and changes none of:

- CODA development-profile bytes or record semantics;
- S0/S1/D definitions;
- provenance encoding;
- F.1 `DistributedPartition` fields or `CDP1` encoding;
- F.2 worker/execution semantics;
- `StreamId`, clocks, epochs, archive sequences, or source truth;
- Stage E transport/recovery contracts;
- C ABI or CLI behavior.

F.3 explicitly does **not** add:

- object writes/uploads, multipart upload, delete, list, copy, replication, lifecycle, or retention policy;
- a bundled S3, GCS, Azure Blob, HTTP, filesystem, or other concrete provider client;
- credentials, signing, authorization, secrets, capability tokens, or endpoint policy;
- location discovery, a distributed index, global archive catalog, or backend registry;
- multi-backend routing or automatic failover;
- RPC/socket worker execution, processor distribution, scheduling, thread pools, or queues;
- leases, heartbeats, retries, replay deduplication, checkpointing, or exactly-once semantics;
- automatic CODA persistence of retrieved records or processor outputs;
- encryption/decryption, compression/decompression, repair, or semantic interpretation;
- deployment integrations;
- throughput, latency, scale, availability, durability, fault-tolerance, or cost claims.

## Tests

Required proof:

1. a valid F.1 partition plus matching locations retrieves exact ordered payload ranges and returns the same partition identity/stream/backend label;
2. the returned records pass unchanged into F.2 `execute_partition()` with a `LocalProcessorWorker`;
3. backend receives exact store/key/version/offset/length values once per record and in partition order;
4. original `RecordInfo` metadata, including interval, raw unknown type code, sequence, stream, hash, payload size, and original `file_offset`, is preserved while object offset remains separate;
5. tampered partition identity, partition stream, location link fields, reordered/missing/extra locations, interval inversion, length mismatch, range overflow, empty store/key, or payload-byte-total mismatch fails before any backend range read;
6. zero limits fail `invalid_argument`; excessive record count/bytes/backend-name/store/key/version lengths fail `resource_exhausted` before reads;
7. a backend error is propagated after exactly one attempted read of the failing location with no retry;
8. short or long backend results fail `protocol`;
9. same-length wrong bytes fail `archive_corrupt`;
10. zero-length exact records are supported and verified;
11. F.1 partition tests and F.2 worker tests remain unchanged and green;
12. existing repository tests remain green;
13. installed-package consumer can define an `ObjectStoreBackend`, retrieve exact records through F.3, and execute the returned records through F.2.

## Stage F dependency direction after F.3

F.1 defines deterministic exact work membership.

F.2 defines bounded execution of one materialized partition.

F.3 defines bounded storage placement descriptors and exact materialization through a caller-supplied object-store backend.

After F.3 merges, audit Stage F again. The expected next infrastructure dependency is a distributed location/index primitive that maps exact record links or partition membership to one or more `DistributedRecordLocation` candidates without changing record truth or partition identity. Do not freeze an F.4 API until the merged F.3 contract is available.
