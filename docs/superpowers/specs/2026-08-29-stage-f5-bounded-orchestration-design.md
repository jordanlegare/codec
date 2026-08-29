# Stage F.5 — bounded multi-partition scheduling and worker orchestration design

## Status

Approved for implementation under the repository's standing autonomous milestone approval. This design is derived from merged Stage F.1–F.4 contracts and keeps the next slice intentionally bounded.

## Context

Merged Stage F currently provides:

- F.1 deterministic exact-work partitioning (`DistributedPartition`, CDP1 identity).
- F.2 bounded synchronous execution of one exact materialized partition through one `DistributedWorker`.
- F.3 bounded exact record materialization through one caller-supplied `ObjectStoreBackend` and ordered `DistributedRecordLocation`s.
- F.4 bounded deterministic in-memory mapping from exact record links to canonical ordered placement candidates.

The smallest remaining dependency is a coordinator that can take more than one exact partition and drive those primitives in a deterministic, resource-bounded way while reporting each partition's outcome independently.

## Goal

Add a public C++ orchestration primitive that accepts an ordered batch of exact F.1 partitions, one F.4 location index, one caller-supplied F.3 backend, and an ordered caller-supplied worker pool, then deterministically coordinates location resolution, location selection, exact retrieval, and worker execution for each partition.

The new capability is scheduling/orchestration only. It does not add network workers, concurrency, retries, leases, exactly-once semantics, backend discovery/routing, persistent/global indexes, deployment, or performance/scale claims.

## Approaches considered

### A. Deterministic sequential round-robin orchestration — selected

Process partitions in caller order. Assign partition `i` to worker `i % worker_count`. Resolve F.4 candidates and choose the first canonical candidate for each exact record, then invoke F.3 and F.2 synchronously. Continue after partition-local failures and return one outcome per input partition.

Advantages: deterministic, directly compositional over F.1–F.4, easy to bound, no thread-safety assumptions on caller objects, and no accidental throughput/fault-tolerance claim.

### B. Concurrent worker pool

Dispatch partitions concurrently across workers.

Rejected for F.5 because it introduces thread-safety/lifetime requirements, completion-order semantics, cancellation, synchronization, and performance expectations that are not needed to satisfy the next dependency.

### C. Planner-only schedule generation

Return worker/location assignments without actually invoking F.3/F.2.

Rejected because it would leave the actual multi-partition orchestration dependency unmet and would duplicate caller glue rather than prove F.1→F.4→F.3→F.2 composition.

## Public API

Add to `<codec/distributed.hpp>`:

```cpp
enum class DistributedPartitionOutcomeStatus {
  succeeded,
  location_unavailable,
  retrieval_failed,
  execution_failed,
};

struct DistributedSchedulingLimits {
  std::size_t maximum_partitions{4096};
  std::size_t maximum_workers{256};
  std::size_t maximum_total_records{16384};
  std::uint64_t maximum_total_payload_bytes{256ULL * 1024ULL * 1024ULL};
  DistributedLocationQueryLimits location_query{};
  DistributedRetrievalLimits retrieval{};
  DistributedExecutionLimits execution{};
};

struct DistributedPartitionOutcome {
  Sha256 partition_identity{};
  StreamId stream{};
  std::size_t partition_index{};
  std::size_t worker_index{};
  DistributedPartitionOutcomeStatus status{
      DistributedPartitionOutcomeStatus::location_unavailable};
  std::vector<DistributedRecordLocation> selected_locations;
  std::optional<DistributedExecutionResult> execution;
  std::optional<Error> error;
};

struct DistributedScheduleResult {
  std::size_t succeeded{};
  std::vector<DistributedPartitionOutcome> partitions;
};

Result<DistributedScheduleResult> schedule_partitions(
    std::span<DistributedWorker* const> workers,
    ObjectStoreBackend& backend,
    const DistributedLocationIndex& index,
    std::span<const DistributedPartition> partitions,
    DistributedSchedulingLimits limits = {});
```

Exact names may be adjusted minimally during implementation if required by C++ type-system/compiler constraints, while preserving this contract.

## Deterministic scheduling contract

1. The scheduler validates all top-level limits are non-zero.
2. An empty partition batch returns an empty successful schedule result and does not require a worker.
3. A non-empty batch requires at least one non-null worker and obeys `maximum_workers` and `maximum_partitions`.
4. Before backend reads or worker execution, the scheduler preflights every input partition:
   - non-empty exact membership;
   - all links belong to the partition stream;
   - CDP1 identity matches ordered membership;
   - aggregate record and payload-byte bounds are satisfied;
   - duplicate partition identities in the same schedule are rejected to avoid accidental repeated execution.
5. Partition `i` is assigned worker index `i % workers.size()`. Assignment is based on input position, not on prior success/failure, so failures do not perturb later assignments.
6. F.4 resolution runs for the partition under caller-provided nested query limits.
7. If resolution is incomplete, the partition outcome is `location_unavailable`; no F.3 read or F.2 execution occurs for that partition.
8. If complete, the scheduler selects candidate `0` from each F.4 candidate set. F.4 already canonical-sorts candidates, so this is a deterministic placement-selection rule independent of registration order.
9. F.3 exact retrieval runs with those selected locations and the caller-supplied backend.
10. Retrieval failure is recorded as `retrieval_failed`; F.2 is not invoked for that partition.
11. On retrieval success, F.2 executes the assigned worker using the exact materialized records and caller-provided nested execution limits.
12. Execution failure is recorded as `execution_failed`.
13. Success stores the F.2 execution result and increments `succeeded`.
14. The scheduler continues in input order after partition-local location/retrieval/execution failures and returns exactly one outcome per input partition.

## Error boundary

`Result<DistributedScheduleResult>` failure is reserved for schedule-level invalidity or inability to construct the result safely: invalid/zero limits, invalid worker pool, malformed/tampered batch preflight, duplicate partition identities, aggregate bound exhaustion, or allocation failure.

After preflight succeeds, F.4/F.3/F.2 errors are partition-local and are captured in `DistributedPartitionOutcome::error`. This keeps orchestration success distinct from work-item success.

No retry is performed even when an underlying `Error` is marked retryable.

## Exactness and truth invariants

- F.1 `DistributedPartition` and CDP1 bytes remain unchanged.
- F.5 never changes `RecordInfo`, payload bytes, S0/S1/D semantics, or provenance.
- F.4 candidate ordering remains the source of deterministic placement order; F.5 does not invent health/locality/cost ranking.
- F.3 remains responsible for exact byte-count and SHA-256 verification before materialization succeeds.
- F.2 remains responsible for exact partition/input membership validation and processor-output validation.
- Logical stream identity is never bound to a worker, backend, object placement, or schedule position.

## Bounds and side-effect discipline

The whole batch is structurally and aggregately preflighted before the first backend read or worker invocation. Nested F.4/F.3/F.2 limits remain independently enforced per partition.

The scheduler is synchronous and sequential. It owns no threads, sockets, worker processes, persistent state, leases, timers, heartbeats, queues, or retry state.

## Tests

Add `tests/test_distributed_scheduler.cpp` covering:

- two or more F.1 partitions driven through F.4 resolution → F.3 retrieval → F.2 execution;
- deterministic round-robin worker assignment;
- deterministic first-canonical-candidate selection independent of F.4 registration order;
- per-partition success outcomes and exact output preservation;
- incomplete F.4 resolution produces `location_unavailable` with no backend/worker call for that partition;
- F.3 failure produces `retrieval_failed` and no worker call for that partition;
- F.2 failure produces `execution_failed` while later partitions still execute on their input-position worker;
- preflight rejects tampered CDP1, cross-stream links, duplicate partition identities, null workers, and exceeded aggregate/worker/partition limits before side effects;
- zero limits fail closed;
- empty partition batch returns an empty result without side effects;
- installed-package consumer proves the public API from installed `<codec/distributed.hpp>`.

Existing F.1–F.4, CODA, Stage E, C ABI, CLI, and package tests must remain green.

## Documentation and status

Update `AI_WORKSHEET.md`, `CHANGELOG.md`, `README.md`, CMake source/test lists, and the package consumer. The roadmap issue completion record must state the exact RED/GREEN/CI/merge evidence and re-audit the remaining Stage F dependency graph after merge.

## Explicit non-claims

F.5 does not claim or implement:

- concurrent/thread-pool execution;
- RPC/network worker transport or process management;
- worker discovery, health, authentication, authorization, or attestation;
- backend registry/discovery, store-specific routing, locality/cost ranking, retries, or failover;
- leases, heartbeats, cancellation protocol, idempotency keys, or exactly-once semantics;
- persistent/global/network location catalogs;
- automatic distributed CODA persistence;
- deployment integration;
- throughput, latency, availability, durability, fault-tolerance, or scale evidence.

Those remain separate later Stage F gates.
