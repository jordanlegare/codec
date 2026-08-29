# Stage F.2 — bounded distributed worker execution design

## Status

Approved under the repository's standing autonomous milestone authorization. This design starts from merged Stage F.1 at `812f7d90ca994efb1dcbd9b03d5e25fe6f4da445`.

## Goal

Add one bounded worker-execution boundary that consumes an F.1 `DistributedPartition`, proves that the exact in-memory `ExtractedRecord` inputs match that partition, executes one generic `StreamProcessor` through a worker abstraction, and returns already-validated processor outputs together with descriptive execution metadata.

F.2 establishes **how one exact partition is executed by one worker**. It does not yet decide how many workers exist, how partitions are scheduled among them, how work crosses a network, or where source/output bytes are stored.

## Existing substrate

F.2 builds only on merged generic contracts:

- F.1 `DistributedPartition` defines one stable `StreamId`, ordered exact `ProvenanceRecordLink` membership, aggregate payload bytes, and deterministic `CDP1` identity.
- `ExtractedRecord` carries one exact `RecordInfo` plus its payload bytes.
- `StreamProcessor` is the generic profile processing boundary.
- `invoke_processor()` already validates exact input sizes, S1/D output truth classes, output intervals, reserved record types, process metadata encodability, and caller output-count/output-byte limits. It never writes an archive.
- `Sha256` and `sha256()` provide integrity evidence only; they are not authentication or attestation.
- Logical `StreamId` is independent of worker, transport, file, archive segment, and deployment location.

## Approaches considered

### A. Pluggable single-partition worker boundary — selected

Define a generic `DistributedWorker` interface, a concrete in-process `LocalProcessorWorker`, and an `execute_partition()` wrapper that validates exact partition membership before invoking the worker. A later RPC or deployment adapter can implement the same worker interface without changing F.1 partition semantics.

This keeps F.2 small, testable, vendor-neutral, and compatible with current processing contracts.

### B. Bounded local thread pool and scheduler — deferred

A thread pool could assign several F.1 partitions across local workers now, but it would immediately introduce queueing, concurrency, fairness, cancellation, worker thread-safety, and scheduling semantics. None are required to prove the worker execution boundary itself.

### C. Serialized RPC request/response protocol — deferred

Freezing a work wire protocol now would require premature decisions about processor discovery/registration, transport, authentication, remote payload transfer, retries, and version negotiation. Those belong to later Stage F deployment/transport integration work.

## Public API

Extend `<codec/distributed.hpp>` additively:

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

Names may be adjusted only for an existing repository naming collision; semantics are fixed by this design.

## Worker semantics

`DistributedWorker` represents one caller-supplied execution provider for one configured processor capability.

- `name()` is a bounded descriptive worker label.
- `processor_name()` is a bounded descriptive processor/capability label.
- Neither label authenticates, authorizes, attests, or globally identifies a process/machine.
- `execute()` receives the exact ordered inputs after F.2 wrapper validation.
- A worker returns processor outputs or one explicit `Error`.
- F.2 performs no automatic retry, replay, lease, deduplication, or exactly-once enforcement.

`LocalProcessorWorker` is the concrete F.2 worker backend. It stores a non-owning reference to one caller-owned `StreamProcessor`, returns the configured worker label and `StreamProcessor::name()`, and executes by calling that processor exactly once for each `execute()` invocation. The caller must keep the processor alive while the worker is used.

The local worker is synchronous. It does not create threads, processes, sockets, queues, or a scheduler.

## Partition verification before execution

`execute_partition()` must fail before calling `worker.execute()` unless all conditions below hold.

### Limits and basic shape

1. `maximum_input_records`, `maximum_input_bytes`, `maximum_worker_name_bytes`, `maximum_processor_name_bytes`, `limits.processor.maximum_outputs`, and `limits.processor.maximum_output_bytes` must all be non-zero.
2. The partition and input batch must both be non-empty.
3. `partition.records.size()` must equal `inputs.size()`.
4. Input count must not exceed `maximum_input_records`.
5. Worker and processor labels must be non-empty and must not exceed their configured byte limits.

### Partition self-consistency

6. Every partition link must use exactly `partition.stream`.
7. `partition.payload_bytes` must equal the overflow-safe sum of the supplied exact input payload sizes.
8. Recompute the canonical F.1 `CDP1` identity from `partition.stream` plus the ordered partition links; it must equal `partition.identity`.

F.2 must reuse the exact F.1 canonical identity implementation through a private distributed-module helper rather than fork or redefine `CDP1` bytes.

### Exact input binding

For each input at index `i`:

9. `input.payload.size()` must equal `input.record.payload_size`.
10. The input stream must equal `partition.stream`.
11. The exact link derived from the input `(stream, raw type code, archive sequence, record hash)` must equal `partition.records[i]` field-for-field.
12. `sha256(input.payload)` must equal `input.record.hash`; otherwise the purported exact input bytes do not satisfy their physical-record integrity identity.
13. Aggregate input bytes must not exceed `maximum_input_bytes`, using overflow-safe arithmetic.

Input order is significant because F.1 partition identity commits ordered membership. Reordering otherwise identical inputs is rejected.

## Output validation

A worker is not trusted merely because it implements the C++ interface. F.2 therefore routes its output through the existing generic `invoke_processor()` validation contract.

Implementation may use a private `StreamProcessor` proxy whose `process()` method delegates exactly once to `worker.execute()`. `invoke_processor()` then validates:

- processor input-size consistency;
- output-count and aggregate output-byte limits;
- S1/D truth class only;
- non-decreasing output intervals;
- reserved archive metadata types remain prohibited;
- generic process provenance metadata remains encodable.

If the worker returns an `Error`, it is propagated unchanged and no retry occurs. If worker output violates the generic processor contract, the existing `invoke_processor()` validation error is returned and no `DistributedExecutionResult` is emitted.

A `std::bad_alloc` raised by F.2-owned validation/result assembly should be converted to `resource_exhausted`. F.2 does not attempt to catch or reinterpret arbitrary provider exceptions.

## Execution result

On success, `DistributedExecutionResult` contains:

- the exact verified F.1 `partition.identity`;
- the exact partition `StreamId`;
- the descriptive worker name observed for this invocation;
- the descriptive processor name observed for this invocation;
- the already-validated `ProcessorOutput` vector returned through the worker.

The result is an in-memory execution descriptor only. It is not an authenticated receipt, remote attestation, audit log, persistence record, archive locator, or proof that a processor's semantic inference is correct.

Processor output remains subject to existing truth semantics: S1 only when the profile independently defines deterministic state exactness; otherwise transformed/inferred output is D. F.2 does not upgrade probabilistic output into deterministic evidence.

## Transactionality and side effects

- Validation failures before execution must not call the worker.
- Worker failure produces no partial execution result.
- Output validation failure produces no partial execution result.
- `execute_partition()` does not mutate the partition or supplied `ExtractedRecord` objects.
- A caller-supplied worker/processor may maintain its own state; F.2 makes no purity or determinism claim about provider internals.
- F.2 performs no CODA, filesystem, network, object-store, or index write.

## Error model

- zero execution/name/output limits -> `invalid_argument`;
- empty partition/input, record-count mismatch, wrong stream/link/order, partition payload-byte mismatch, or `CDP1` identity mismatch -> `invalid_argument`;
- malformed `ExtractedRecord` payload size -> `invalid_argument`;
- input payload whose SHA-256 does not equal its `RecordInfo::hash` -> `archive_corrupt`;
- input count/aggregate input bytes/name length/output bounds -> `resource_exhausted`;
- worker/provider error -> propagated unchanged;
- invalid worker output -> existing `invoke_processor()` error;
- F.2-owned allocation failure -> `resource_exhausted`.

No failure automatically retries or moves the partition to another worker.

## Internal module structure

Keep Stage F implementation isolated:

- `include/codec/distributed.hpp` — additive public partition/worker execution API.
- `src/distributed/internal.hpp` — private exact-link and canonical `CDP1` identity helpers shared by F.1 and F.2.
- `src/distributed/partition.cpp` — F.1 partition construction, refactored only enough to use shared private helpers.
- `src/distributed/worker.cpp` — F.2 worker and `execute_partition()` implementation.
- `tests/test_distributed_worker.cpp` — focused F.2 contract tests.

No unrelated core/archive/transport/profile refactor is part of F.2.

## Compatibility and non-goals

F.2 is additive and changes none of:

- CODA development-profile bytes or record semantics;
- S0/S1/D definitions;
- provenance encoding;
- F.1 `DistributedPartition` fields or `CDP1` encoding;
- StreamId derivation, clocks, epochs, or archive sequences;
- Stage E transport/recovery contracts;
- C ABI or CLI behavior.

F.2 explicitly does **not** add:

- multi-partition scheduling, thread pools, worker queues, fairness, or cancellation;
- RPC, sockets, wire protocols, service discovery, or network transport;
- processor registry/discovery or remote code/model distribution;
- worker authentication, authorization, attestation, trust, or confidential execution;
- leases, heartbeats, retries, replay deduplication, checkpointing, or exactly-once execution;
- object-store backends, archive replication, remote record retrieval, or output upload;
- distributed indexes/global archive locations;
- automatic CODA persistence of processor outputs or execution metadata;
- deployment integrations;
- throughput, latency, scale, fault-tolerance, or cost benchmarks/claims.

## Tests

Required proof:

1. a matching F.1 partition plus exact records executes one `LocalProcessorWorker` exactly once and returns the same partition identity/stream plus bounded worker/processor labels and validated processor outputs;
2. the local worker delegates to the configured `StreamProcessor` and package consumers can construct/use it from installed headers;
3. tampered partition identity, partition stream, partition link fields, partition payload-byte count, reordered inputs, missing/extra inputs, or wrong-stream inputs fail before the worker is called;
4. payload-size mismatch fails `invalid_argument` before execution;
5. payload bytes whose SHA-256 disagrees with `RecordInfo::hash` fail `archive_corrupt` before execution;
6. zero limits, excessive input count/bytes, and excessive worker/processor label lengths fail with the specified error class before execution;
7. a worker error is propagated exactly once with no retry;
8. a worker that returns invalid truth class, interval, reserved record type, unencodable process metadata, excessive outputs, or excessive bytes is rejected by the existing processor validation path;
9. F.1 deterministic partitioning tests remain unchanged and green after private helper refactoring;
10. existing repository tests remain green;
11. the installed-package consumer includes `<codec/distributed.hpp>`, partitions exact records, executes them through `LocalProcessorWorker`, and observes one validated result.

## Stage F dependency direction after F.2

F.1 defines deterministic exact work membership.

F.2 defines bounded one-partition/one-worker processor execution against that exact membership.

After F.2 merges, audit Stage F again. The expected next infrastructure dependency is object-store addressing/backends and remote exact-record retrieval, because a genuinely remote worker cannot consume an F.1 partition without a location/retrieval layer. Do not freeze an F.3 API until the merged F.2 contract is available.
