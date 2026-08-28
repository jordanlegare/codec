# Stage C.3 Generic Processing Contracts Design

## Status

Approved for autonomous implementation under the user's standing global
decision and publication authorization. This document defines the smallest
Stage C.3 boundary that lets non-audio profiles implement adapters and
processors without changing CODA or the compatibility-era engine.

## Context

Stages B and C.1-C.2 established payload-agnostic record storage, exact
S0/S1/D links, physical query/extraction, and direct provenance queries. The
remaining public execution surfaces are profile-specific:

- `Engine::record()` accepts `FeedSpec` and owns the current file/HTTP capture
  path; and
- `SeparationBackend` accepts PCM audio and exposes one audio inference task.

There is no generic contract through which a telemetry, sensor, video,
document/event, or other profile can supply S0 records or transform exact
records into declared S1/D outputs.

## Decision

Add narrow C++ contracts and validating invocation helpers in a new public
`<codec/processing.hpp>` header. Do not add a registry, scheduler, persistence
transaction, dynamic plugin ABI, or engine migration in this milestone.

This approach was selected over two broader alternatives:

1. A registry/executor would prematurely fix lifetime, concurrency,
   cancellation, and archive-transaction policies.
2. Migrating `Engine` and audio separation immediately would combine generic
   architecture, compatibility migration, and Stage D Audio Profile work.

The selected boundary is independently usable, installed-package testable,
and small enough to preserve the existing runtime and archive behavior.

## Public API

Add these owned aggregates:

```cpp
struct AdapterRecord {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::vector<std::byte> payload;
};

struct ProcessorOutput {
  StreamId stream{};
  RecordTypeCode type{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  TruthClass truth{TruthClass::derived};
  std::vector<std::byte> payload;
  ProvenanceProcess process;
};
```

`AdapterRecord` has no truth field because every accepted adapter record is S0.
`ProcessorOutput` may be S1 or D only. Its process metadata uses the existing
bounded, versioned generic `ProvenanceProcess` contract.

Add these abstract interfaces:

```cpp
class StreamAdapter {
 public:
  virtual ~StreamAdapter() = default;
  virtual std::string name() const = 0;
  virtual Result<std::optional<AdapterRecord>> next() = 0;
};

class StreamProcessor {
 public:
  virtual ~StreamProcessor() = default;
  virtual std::string name() const = 0;
  virtual Result<std::vector<ProcessorOutput>> process(
      std::span<const ExtractedRecord> inputs) = 0;
};
```

The adapter is pull-based. One successful non-empty result transfers ownership
of one source record; a successful empty optional is end-of-stream; an error is
explicit. The caller controls pull cadence, providing natural backpressure and
cancellation by stopping pulls.

The processor is batch-based over exact extracted records. Every supplied input
is a direct supporting input for every returned output. A processor that needs
different dependency subsets invokes the boundary separately for each subset.
An empty output vector is a successful filtering/no-result operation.

Add explicit invocation limits:

```cpp
struct AdapterReadLimits {
  std::uint64_t maximum_payload_bytes{16ULL * 1024ULL * 1024ULL};
};

struct ProcessorRunLimits {
  std::size_t maximum_outputs{1024};
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};
```

Add validating helpers:

```cpp
Result<std::optional<AdapterRecord>> pull_adapter_record(
    StreamAdapter& adapter, AdapterReadLimits limits = {});

Result<std::vector<ProcessorOutput>> invoke_processor(
    StreamProcessor& processor,
    std::span<const ExtractedRecord> inputs,
    ProcessorRunLimits limits = {});
```

The contracts remain C++20 source interfaces. No C ABI or stable binary plugin
ABI is claimed.

## Validation Semantics

`pull_adapter_record()` validates limits before invoking the adapter. It then
accepts end-of-stream or validates the returned record:

- `end_ns` must not precede `start_ns`;
- `stream_provenance` and `final_index` are reserved and cannot be emitted as
  adapter artifacts; and
- payload size must not exceed `maximum_payload_bytes`.

`invoke_processor()` validates non-empty inputs, the Stage B provenance input
count ceiling, exact payload-size agreement with each input's `RecordInfo`, and
non-zero limits before invoking the processor. It then validates the complete
output vector:

- output count must not exceed `maximum_outputs`;
- the overflow-safe sum of payload sizes must not exceed
  `maximum_output_bytes`;
- each output interval must be valid;
- each truth class must be S1 or D, never S0 or an unknown enum value;
- reserved provenance/final-index record types cannot be processor subjects;
  and
- process identity, optional hashes, typed details, text, and size limits must
  satisfy the same validation used by persisted provenance sidecars.

Invalid limits and provider contract violations return `invalid_argument`.
Output count/byte excess returns `resource_exhausted`. Adapter/processor errors
are propagated unchanged. The helpers perform no archive writes, so provider
failure or invalid output cannot modify committed preservation.

## Data Flow and Truth Boundaries

The intended minimal flow is:

```text
profile adapter
  -> pull_adapter_record()
  -> caller appends exact S0 with CodaWriter::append_raw()
  -> caller extracts or retains exact input records
  -> invoke_processor()
  -> caller appends S1/D subject and StreamProvenance sidecar
```

The helper does not silently persist, reinterpret, or reclassify bytes.
Preservation remains an explicit caller action before optional processing.

- S0: every adapter record is exact accepted source representation.
- S1: a processor may return `state_exact` only under a profile-defined
  deterministic canonicalization contract.
- D: a processor may return `derived`; the supplied exact input batch and
  returned process identity are sufficient for the caller to append the
  mandatory provenance sidecar.

The boundary does not authenticate process metadata or configuration hashes;
it preserves the existing distinction between identity and authentication.

## Compatibility

The change adds one public header, one library implementation file, and tests.
It does not alter:

- CODA header, envelope, record encodings, registry, or repair;
- `Engine`, `FeedSpec`, current capture behavior, or audio APIs;
- `SeparationBackend` or capability reporting;
- C ABI or CLI;
- existing archive append/read/query methods; or
- installed CMake target name and version.

No adapter or processor is registered automatically. Existing source remains
compatible because all additions are new types and free functions.

## Proof Contract

Tests must establish:

1. a non-audio telemetry adapter emits owned, byte-exact S0 records in pull
   order and reports end-of-stream explicitly;
2. adapter failures propagate unchanged;
3. invalid adapter intervals, reserved types, zero limits, and oversized
   payloads are rejected deterministically;
4. a deterministic telemetry normalizer consumes exact extracted input and
   returns S1 output with complete generic process identity;
5. processor failures propagate without output;
6. empty/mismatched inputs, excessive input/output counts, overflow-safe byte
   limits, S0/invalid truth, invalid intervals, reserved types, and malformed
   process metadata are rejected;
7. empty processor output is valid;
8. an installed-package consumer can implement both interfaces, append the S0
   and S1 records with existing writer APIs, append exact provenance, reopen
   the archive, and query the S1 relationship; and
9. Release, ASan/UBSan, C ABI, CLI integration, AI contract, existing archive,
   engine, inference, and audio tests remain green.

## Explicit Non-Claims

This milestone does not implement or claim:

- a registry, discovery mechanism, dynamic loading, or stable plugin ABI;
- asynchronous execution, threads, scheduling, queues, retries, or distributed
  workers;
- an archive transaction that atomically persists processor subject and
  provenance records;
- automatic descriptor, continuity, policy, or provenance persistence;
- built-in non-audio adapters or processors;
- migration of compatibility-era `Engine` capture;
- migration or availability of audio neural inference;
- profile-specific confidence/calibration interpretation;
- processor/model selection queries, exporters, CLI/C ABI access;
- performance, throughput, scale, deployment, or frozen CODA v1 guarantees.

Those remain separate milestones so their policy and failure semantics can be
designed from evidence rather than implied by these contracts.
