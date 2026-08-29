# Stage F.1 — deterministic distributed partitioning design

## Status

Approved under the repository's standing autonomous milestone authorization. This design is the first Stage F boundary after Stage E completion at `5301da72c65f519b0c16347f8684be113d15ab0b`.

## Goal

Add one bounded, deterministic, worker-agnostic partitioning primitive that turns an exact in-memory `ExtractedRecord` batch into logical work partitions suitable for later distributed execution without changing CODA bytes, stream identity, record truth, provenance, or processing semantics.

F.1 establishes **what exact work belongs together**. Later Stage F milestones decide **where that work runs** and **where bytes are stored**.

## Existing substrate

F.1 builds only on existing generic contracts:

- `StreamId` is stable logical identity independent of transport, archive placement, or worker placement.
- `ExtractedRecord` carries one exact `RecordInfo` plus its verified payload bytes.
- `ProvenanceRecordLink` is the existing exact physical-record reference shape used by generic processing/export provenance.
- Stage C processing APIs accept exact extracted records and do not automatically persist outputs.
- Stage E transport/recovery is complete at its bounded scope and is orthogonal to partition placement.

## Public API

Add `<codec/distributed.hpp>` with an additive C++ API:

```cpp
struct DistributedPartitionLimits {
  std::size_t maximum_input_records{16384};
  std::uint64_t maximum_input_bytes{256ULL * 1024ULL * 1024ULL};
  std::size_t maximum_partitions{4096};
  std::size_t maximum_records_per_partition{1024};
  std::uint64_t maximum_payload_bytes_per_partition{64ULL * 1024ULL * 1024ULL};
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
```

Names may be adjusted only for an existing repository naming collision; semantics are fixed by this design.

## Partitioning semantics

1. Validate all configured limits as non-zero before partitioning.
2. An empty input batch returns an empty partition list.
3. Reject input count above `maximum_input_records`.
4. Every input payload length must equal its `RecordInfo::payload_size`; malformed caller-constructed exact inputs fail with `invalid_argument`.
5. Aggregate input payload bytes are bounded by `maximum_input_bytes` using overflow-safe arithmetic.
6. A single record larger than `maximum_payload_bytes_per_partition` fails with `resource_exhausted`; records are never split.
7. Each partition contains records from exactly one `StreamId`.
8. Scan inputs in caller order. Maintain at most one open partition per stream. Interleaved records from the same stream may therefore remain in the same partition even when other streams occur between them.
9. Within each partition, exact record links preserve the relative order in which that stream's records appeared in the input batch.
10. Before adding a record, start a new partition for that stream if the current partition would exceed either `maximum_records_per_partition` or `maximum_payload_bytes_per_partition`.
11. Starting a partition beyond `maximum_partitions` fails with `resource_exhausted`.
12. Output partition order is deterministic creation order during the input scan. No global archive-contiguity or cross-stream ordering claim is added.
13. The function is transactional: on any validation, limit, or allocation failure, it returns no partial partition list and mutates no caller state.

## Exact record links

Each accepted input contributes exactly one `ProvenanceRecordLink` copied from its physical record metadata:

- exact `StreamId`;
- exact 16-bit record type code;
- exact archive record sequence;
- exact record hash.

F.1 does not reinterpret those records, infer truth class, rewrite provenance, or create CODA records.

## Deterministic partition identity

Each completed partition receives `Sha256 identity` from a private canonical identity encoding. This is an identity digest, not authentication and not a new wire/storage format.

Canonical bytes are:

1. ASCII domain tag `CDP1`;
2. the partition `StreamId`'s 16 raw bytes;
3. record count as unsigned 64-bit little-endian;
4. for every ordered `ProvenanceRecordLink`:
   - record type code as unsigned 16-bit little-endian;
   - archive record sequence as unsigned 64-bit little-endian;
   - the exact 32-byte record hash.

The SHA-256 of those bytes is the partition identity. It deliberately excludes worker identity, storage URI, scheduling state, resource limits, and payload copies. Therefore the same ordered exact-record membership produces the same partition identity across compatible processes.

The identity is **not** claimed to be a globally unique archive locator. F.1 has no cross-archive namespace or object-store location model yet.

## Failure model

- zero limits -> `invalid_argument`;
- inconsistent `ExtractedRecord` payload size -> `invalid_argument`;
- input count/aggregate bytes/partition count/per-partition bytes exhaustion -> `resource_exhausted`;
- allocation failure -> `resource_exhausted`;
- no fallback repartitioning that violates configured bounds.

## Compatibility and non-goals

F.1 is additive and changes none of:

- CODA development-profile bytes or record semantics;
- S0/S1/D classification;
- provenance encoding;
- StreamId derivation or stream epochs/clocks;
- Stage E CMX1/E.2/XRF1/streaming-repair contracts;
- C ABI or CLI behavior.

F.1 explicitly does **not** add:

- distributed worker processes or a scheduler;
- worker assignment, leases, retries, heartbeats, or exactly-once execution claims;
- RPC, sockets, service discovery, or transport authentication;
- object-store backends, archive replication, or remote byte retrieval;
- distributed indexes or global archive identifiers;
- automatic CODA persistence;
- deployment integrations;
- throughput, latency, scale, or cost benchmarks.

## Tests

Required proof:

1. same exact inputs and limits produce byte-identical partition memberships and SHA-256 identities across repeated calls;
2. one stream is split only at configured record/byte bounds and no record is split;
3. interleaved streams never share a partition while each stream's relative record order is retained;
4. every emitted exact record link equals the source `RecordInfo` fields;
5. partition identity changes when ordered membership changes and is independent of payload-buffer address or worker/storage concepts;
6. empty input succeeds with no partitions;
7. zero limits, malformed exact input, oversized individual record, aggregate input exhaustion, and partition-count exhaustion fail with the required error class and no partial result;
8. existing tests remain green;
9. installed-package consumer includes `<codec/distributed.hpp>` and exercises deterministic partitioning.

## Stage F dependency direction

F.1: deterministic exact-work partition descriptors.

Later milestones may add worker execution, object-store addressing/backends, distributed indexes, operational benchmarks, and deployment integration in that order only when their own evidence gates are defined. Those layers consume F.1 partitions; F.1 does not depend on them.
