# Stage F.6 — bounded remote-worker transport design

Status: approved by the standing CODEC milestone approval and the explicit request to advance through F.6.

Base: `main` at `f98575cb88ac2115b7960c4fb4d417e9b8a381d0`.

## Purpose

Stage F.5 can schedule any `DistributedWorker`, but CODEC only provides `LocalProcessorWorker`. F.6 adds the smallest provider-neutral remote execution seam: a caller-supplied transport contract plus a concrete `RemoteDistributedWorker` adapter that implements the existing `DistributedWorker` interface.

The milestone deliberately does not define a socket protocol, HTTP/gRPC binding, endpoint discovery mechanism, authentication scheme, retry policy, lease model, or concurrency model. Those remain later Stage F dependencies.

## Before / after

**Before:** F.5 can coordinate many partitions, but every concrete CODEC-supplied worker executes in-process through a caller-owned `StreamProcessor`. Callers can author their own `DistributedWorker`, but CODEC provides no bounded generic boundary for delegating one worker invocation to a remote transport implementation.

**After:** a caller can wrap one `DistributedWorkerTransport` in `RemoteDistributedWorker`, configure the expected remote worker and processor labels plus explicit resource limits, and pass that worker unchanged into F.2/F.5. The adapter performs bounded preflight, dispatches exactly once, validates descriptive response identity and output bounds, preserves one transport/remote error without retry, and returns outputs to the existing F.2 validation path.

## Selected approach

Use a structured provider-neutral transport boundary rather than standardizing bytes in F.6.

### Why

1. `DistributedWorker` is already the exact scheduler seam. F.6 only needs a concrete adapter that can delegate through a caller-owned transport implementation.
2. F.2 already performs the authoritative exact input verification and processor-output semantic validation around every worker call. F.6 should not duplicate or weaken it.
3. A CODEC-owned request/response wire format would require freezing serialization for `RecordInfo`, `ProcessorOutput`, `ProvenanceProcess`, and `Error` before a real RPC provider exists. That is unnecessary scope expansion.
4. A concrete HTTP/TCP/gRPC client would pull endpoint policy, credentials, TLS, deadlines, and provider-specific behavior into the same milestone. Those are later dependencies.

The F.6 transport therefore receives the already-materialized exact records in memory and returns a structured response. A later RPC transport can serialize these structures however its protocol requires while preserving the same `RemoteDistributedWorker` contract.

## Public API

Add the following additive surface to `<codec/distributed.hpp>`.

```cpp
struct DistributedRemoteWorkerLimits {
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_transport_name_bytes{256};
  std::size_t maximum_worker_name_bytes{256};
  std::size_t maximum_processor_name_bytes{256};
  ProcessorRunLimits processor{};
};

struct DistributedRemoteExecutionResponse {
  std::string worker_name;
  std::string processor_name;
  std::vector<ProcessorOutput> outputs;
};

class DistributedWorkerTransport {
 public:
  virtual ~DistributedWorkerTransport() = default;
  virtual std::string name() const = 0;
  virtual Result<DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const ExtractedRecord> inputs) = 0;
};

class RemoteDistributedWorker final : public DistributedWorker {
 public:
  RemoteDistributedWorker(DistributedWorkerTransport& transport,
                          std::string worker_name,
                          std::string processor_name,
                          DistributedRemoteWorkerLimits limits = {});

  std::string name() const override;
  std::string processor_name() const override;
  Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) override;
};
```

Exact names may be adjusted during implementation only for consistency with existing CODEC naming; the behavioral contract below is fixed.

## Request preflight

Before calling `DistributedWorkerTransport::dispatch()`, `RemoteDistributedWorker::execute()` must validate all adapter-owned configuration and request bounds:

- all numeric maxima are non-zero;
- `ProcessorRunLimits.maximum_outputs` and `maximum_output_bytes` are non-zero;
- transport, configured worker, and configured processor labels are non-empty;
- each label is within its caller-configured byte limit;
- input count is non-zero and within `maximum_input_records`;
- every input payload size equals `RecordInfo::payload_size`;
- aggregate input bytes are overflow-safe and within `maximum_input_bytes`.

F.6 does not reimplement F.2 partition identity, ordered membership, stream, or SHA-256 verification. `execute_partition()` remains the authoritative boundary for those properties before it invokes the worker.

A preflight failure returns before transport dispatch.

## Dispatch semantics

For one successful preflight:

- call the caller-supplied transport exactly once;
- pass the configured worker name, configured processor name, and exact input span in original order;
- do not mutate, reorder, truncate, retry, split, or batch the request;
- preserve a transport/provider `Error` exactly, including `retryable`, without retrying.

The transport is synchronous at this boundary. It may internally use a remote mechanism, but F.6 makes no concurrency or network-I/O claim about CODEC itself.

## Response validation

A successful transport response is accepted only when:

- `response.worker_name` exactly equals the configured worker name;
- `response.processor_name` exactly equals the configured processor name;
- output count does not exceed `limits.processor.maximum_outputs`;
- aggregate output payload bytes are overflow-safe and do not exceed `limits.processor.maximum_output_bytes`.

Identity mismatch is `ErrorCode::protocol`; output bound exhaustion is `ErrorCode::resource_exhausted`.

F.6 does not independently classify S1/D or validate output intervals/process metadata. The returned outputs flow immediately back to F.2, where existing `execute_partition()` validation remains authoritative.

Worker, processor, and transport names are descriptive routing evidence only. Matching labels are not authentication, authorization, attestation, or proof that a particular machine executed the work.

## F.5 composition

No F.5 public API or scheduler implementation changes are required. `RemoteDistributedWorker` is a normal `DistributedWorker`, so an ordered pool may contain local workers, remote workers, or both. F.5 retains stable `partition_index % worker_count` assignment and its existing per-partition failure semantics.

A transport/remote failure returned by a remote worker therefore appears through the existing F.5 `execution_failed` outcome. F.5 continues later partitions and performs no retry.

## Allocation and exception behavior

The adapter owns strings and may allocate while returning outputs. Consistent with existing distributed primitives, catch `std::bad_alloc` at the F.6 implementation boundary and return `ErrorCode::resource_exhausted` rather than throwing through the public API.

Other provider behavior is expressed through the transport `Result` contract; F.6 does not catch arbitrary exceptions from caller code.

## Security boundary

F.6 does not provide or imply:

- socket, HTTP, TLS, QUIC, gRPC, or other concrete network transport;
- endpoint parsing or SSRF/private-network policy;
- credentials, authentication, authorization, signing, MACs, or attestation;
- worker discovery, health checking, locality, cost, or capability negotiation;
- retries, backoff, failover, leases, heartbeats, cancellation propagation, idempotency keys, or exactly-once execution;
- request/response encryption or a normative CODEC RPC wire format;
- automatic CODA persistence of requests, outputs, or remote evidence.

Concrete transports introduced later must define those properties explicitly rather than inheriting them from F.6 labels.

## Compatibility and truth semantics

- additive C++ API only;
- no CODA byte/layout change;
- no CDP1 partition identity change;
- no F.1/F.2/F.3/F.4/F.5 behavior change;
- no Stage E change;
- no C ABI or CLI change;
- no S0/S1/D or provenance semantic change.

## Proof contract

### RED

Add `tests/test_distributed_remote_worker.cpp` and register it before production API implementation. The initial test build must fail because the F.6 transport/remote-worker types do not exist.

### GREEN unit coverage

Prove:

1. one exact F.2 partition executes through `RemoteDistributedWorker` with exactly one transport dispatch and unchanged input order/payload;
2. transport, worker, and processor labels plus request bounds fail before dispatch;
3. transport/provider errors propagate exactly once, preserving error code/message/retryability;
4. mismatched response worker or processor labels fail as `protocol`;
5. response output count/byte limits fail closed;
6. invalid remote outputs are still rejected by existing F.2 validation after one remote dispatch;
7. F.5 schedules remote workers without scheduler changes and retains deterministic worker slots/per-partition outcomes.

### Installed-package proof

Extend `tests/package_consumer/main.cpp` with a caller-defined in-process test transport, wrap it in `RemoteDistributedWorker`, and exercise the installed public API through F.2 or F.5. This proves the new types and implementation are exported/linkable from an installed package without relying on private headers.

### Full verification

On the exact final branch head:

- GCC configure/build/full CTest/install/package-consumer;
- Clang configure/build/full CTest/install/package-consumer;
- sanitizer configure/build/tests;
- repository AI contract and existing CLI/C ABI tests unchanged.

## Documentation delta

Update `README.md`, `CHANGELOG.md`, and `AI_WORKSHEET.md` only after code/tests are green. Claims must say “bounded provider-neutral remote-worker transport boundary” or equivalent and must explicitly retain the networking/RPC/security/retry/discovery/deployment non-claims.

## Stage exit / next dependency

F.6 is complete when a caller-supplied transport can back a tested `RemoteDistributedWorker`, the worker composes unchanged through F.2 and F.5, installed-package use is proven, and the exact final head passes the complete CI matrix.

After F.6, perform a fresh Stage F dependency audit. Likely remaining nodes include a concrete bounded network/RPC transport, worker discovery/health, persistent/global location services, routing/failover/retry semantics, operational benchmarks, and deployment integration. Do not pre-name or combine them without repository evidence.
