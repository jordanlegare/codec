# Stage D.7 Bounded Offline PCM16 Separation Design

## Status and scope

Stage D remains active. D.1 defines deterministic APS1 PCM16 S1, D.2 and
D.6 preserve WAV and native FLAC S0 before optional canonicalization, D.3 is
the sole provenance-verified PCM16 S1 read gate, and D.4/D.5 export verified
S1 without changing archive truth.

The repository also has a legacy Audio Profile `SeparationBackend` boundary.
Its default implementation is deliberately unavailable because CODEC ships no
compatible neural model or runtime. There is no archive-aware, bounded offline
workflow connecting D.3 to that backend boundary, and there is no
caller-persistable D output contract for separated PCM16 artifacts.

D.7 adds that orchestration layer. It does not add a model, neural runtime,
GPU provider, streaming execution, automatic archive persistence, or a neural
availability claim.

## Decision

Add one profile-only public function that:

1. selects only D.3-verified APS1 PCM16 S1 within an explicit caller interval;
2. invokes one caller-supplied `SeparationBackend` independently for each
   selected state;
3. validates backend identity, model identity, output count, PCM geometry,
   residual presence, numeric metrics, and aggregate encoded-output limits;
4. independently computes mixture-reconstruction metrics from the returned
   stems plus residual;
5. returns stems and the mandatory residual as D-class `ProcessorOutput`
   values ready for caller-controlled persistence; and
6. retains the exact physical S1 support link and the full D.3 state/source
   lineage for every run.

The workflow uses the existing generic `invoke_processor` validation boundary
through a private one-shot Audio Profile processor. It does not write or
modify an archive.

## Alternatives considered

### Bundle ONNX Runtime and a production model in D.7

Rejected. Model licensing, bundle compatibility, provider selection, model
I/O schema, numerical qualification, distribution size, and hardware support
are a separate milestone. Combining them with archive orchestration would
make D.7 unreviewably broad and would risk overstating neural availability.

### Persist D records automatically into the input archive

Rejected. D.3 requires a finalized archive, while `CodaWriter` creates new
append-only archives and cannot reopen a finalized archive for mutation.
Introducing archive rewriting or cross-archive copy semantics is not required
to prove offline orchestration. D.7 instead returns exact payload, process
metadata, and supporting S1 link so a caller can choose a persistence policy.

### Call `SeparationBackend` directly and return only legacy
`SeparationResult`

Rejected. That would bypass the generic processor truth/bounds checks and
would omit exact archive support, D classification, deterministic APS1 output,
and independently computed reconstruction evidence.

## Public API

Add `include/codec/profiles/audio_offline_separation.hpp`, include it from the
Audio Profile facade, and expose:

```cpp
struct OfflinePcm16SeparationLimits {
  std::uint64_t maximum_output_bytes{256ULL * 1024ULL * 1024ULL};
};

struct MixtureReconstructionMetrics {
  std::uint64_t maximum_absolute_sample_error{};
  double root_mean_square_sample_error{};
  double backend_reported_error{};
};

enum class OfflinePcm16ArtifactRole : std::uint8_t {
  stem = 1,
  residual = 2,
};

struct OfflinePcm16Artifact {
  OfflinePcm16ArtifactRole role{OfflinePcm16ArtifactRole::stem};
  std::size_t stem_index{};
  Pcm16State state;
  ProcessorOutput output;
  ProvenanceRecordLink supporting_state;
};

struct OfflinePcm16Separation {
  VerifiedPcm16State input;
  std::vector<OfflinePcm16Artifact> stems;
  OfflinePcm16Artifact residual;
  MixtureReconstructionMetrics reconstruction;
  std::string backend;
  std::string provider;
  Sha256 model_hash{};
  Sha256 configuration_hash{};
};

struct OfflinePcm16SeparationRequest {
  Pcm16StateQuery states;
  std::size_t maximum_sources{8};
  std::string model_bundle;
  std::int64_t created_utc_ns{};
  OfflinePcm16SeparationLimits limits{};
};

Result<std::vector<OfflinePcm16Separation>>
separate_verified_pcm16_offline(
    const CodaArchive& archive,
    SeparationBackend& backend,
    const OfflinePcm16SeparationRequest& request);
```

The request must contain a non-empty, non-inverted `states.time` interval.
`states.maximum_results` and `states.maximum_encoded_bytes` retain D.3 input
bounds. `maximum_sources` must be between 1 and a small profile hard ceiling,
and `maximum_output_bytes` must be non-zero. `model_bundle` is a bounded opaque
backend reference, not a promise that CODEC inspected or authenticated an
external bundle.

The legacy `SeparationRequest`/`SeparationResult`/`SeparationBackend` ABI
remains unchanged. The new API is additive and profile-scoped.

## Data flow

### Selection

The wrapper validates request limits, explicit interval, model-bundle text,
and backend identity before archive work. It then calls
`query_verified_pcm16_states`. D.3 remains the only component that may classify
a physical `pcm16` record as S1.

For each returned state, the wrapper reads that exact state record payload and
forms one `ExtractedRecord`. This exact physical APS1 record is the single
direct input supplied to the private processor.

### Backend invocation

The private processor strictly decodes the exact APS1 input, converts it to
the layout-compatible legacy `WavPcm16` value, and calls the supplied backend
with the requested maximum source count and model-bundle reference.

An unavailable backend returns `model_incompatible`. Backend errors propagate
without substituting output. Runs are sequential and ordered exactly as the
D.3 results. If any run fails, the function returns an error and exposes no
partial result vector.

### Backend-output validation

A successful backend result must satisfy all of the following:

- at least one stem and no more than `maximum_sources`;
- every stem and the residual exactly match the mixture sample rate, channel
  count, and interleaved sample count;
- provider and backend identifiers are non-empty, bounded, and contain no
  embedded NUL;
- `model_hash` is exactly 64 hexadecimal characters and is decoded into a
  `Sha256` value;
- the backend-reported reconstruction error is finite and non-negative; and
- each returned PCM value passes the existing deterministic APS1 encoder.

Failure is `inference` for malformed backend success data, except caller output
bounds use `resource_exhausted` and caller request defects use
`invalid_argument`.

### Independent reconstruction metrics

For each interleaved sample, D.7 computes:

```text
error = mixture - (sum(stems) + residual)
```

using a wide signed accumulator. It returns maximum absolute sample error and
root-mean-square sample error. The backend's existing metric is retained
separately as `backend_reported_error`; D.7 does not silently equate an
unspecified backend metric with its independently defined sample-domain
metrics.

No zero-error threshold is imposed. Separation is probabilistic D output;
non-zero reconstruction error is evidence to report, not a reason to relabel
or conceal the output.

## D artifacts and persistence handoff

Each stem and residual is encoded with the existing deterministic APS1 byte
codec but is explicitly marked `TruthClass::derived` in its `ProcessorOutput`.
Using APS1 as a payload representation does not promote a separation result to
S1.

Every output uses:

- the input logical stream and authenticated interval;
- physical type `pcm16`;
- truth class D;
- process operation `audio.offline-separation`;
- a bounded implementation identity derived from backend name and provider;
- model SHA-256 as `implementation_hash`;
- a deterministic configuration SHA-256 over the versioned D.7 request
  parameters that affect backend execution;
- caller-supplied execution time; and
- typed opaque details identifying stem/residual role and the three
  reconstruction metrics.

`supporting_state` is derived from the exact D.3 APS1 `RecordInfo`. A caller
that persists an output can append that payload as a D subject and use the
returned process plus exact S1 record as its direct provenance input. D.7 does
not manufacture a subject link before a physical D record exists.

The enclosing `OfflinePcm16Separation` retains the complete
`VerifiedPcm16State`, so the D result remains connected to both the exact S1
support and D.3's verified S1-to-S0 lineage.

## Resource behavior

Input selection stays bounded by D.3. D.7 applies one aggregate encoded-output
byte budget across every stem and residual from every selected state. It uses
checked arithmetic before accumulating counts or bytes. The generic processor
wrapper rechecks per-run output count, encoded bytes, D truth, physical types,
intervals, and encodable process metadata.

`maximum_sources` has a profile hard ceiling to keep reconstruction
accumulation and output multiplication bounded even if a caller supplies an
extreme value. Output limits are checked before a successful result vector is
returned; no output is persisted by this function.

## Truth and compatibility

- **S0:** read-only and unchanged.
- **S1:** read-only; selected only through D.3 and retained as exact support.
- **D:** new in-memory separated stems and mandatory residual, each visibly
  derived and carrying process identity plus exact S1 support.
- **CODA layout:** unchanged.
- **Generic APIs:** unchanged; D.7 consumes the existing processor contract.
- **Audio compatibility:** legacy separation types and root-level names remain
  available.
- **Capabilities:** `neural_separation:false` and `gpu_inference:false` remain
  unchanged because CODEC still bundles no compatible runtime/model.

## Proof contract

The dedicated D.7 test must first fail to compile before the API exists, then
prove:

1. an actual D.2 ingest archive is consumed through D.3 and a deterministic
   fake backend produces ordered stems plus a mandatory residual;
2. every output is D, APS1-decodable, interval-preserving, process-bearing,
   and linked to the exact verified S1 physical record;
3. model, backend/provider, and deterministic configuration identities are
   retained;
4. independent reconstruction metrics are correct and distinct from the
   backend-reported metric;
5. default unavailable execution returns `model_incompatible` with no output;
6. request validation happens before backend invocation;
7. excessive output bytes/source counts return `resource_exhausted` without a
   partial successful vector;
8. malformed backend success values fail closed;
9. a later-state failure does not expose earlier successful results;
10. the input archive remains finalized, verified, and byte-for-byte
    unchanged; and
11. existing archive, Audio Profile, export, ingest, Engine, C ABI, CLI,
    watermark, generic processor, installed-package, and unavailable-inference
    coverage remains green in Release and sanitizer builds.

## Non-claims

D.7 does not claim or implement:

- a bundled or production neural model/runtime;
- ModelBundle file schema, signature, compatibility resolution, or download;
- neural-quality, source-count, perceptual, identity, diarization, or
  separation-accuracy qualification;
- live/streaming inference, GPU execution, latency, throughput, or scale;
- automatic CODA persistence, finalized-archive mutation, or cross-archive
  transactions;
- resampling, remixing, channel-layout interpretation, enhancement,
  concealment, or floating-point canonical state;
- CLI or C ABI offline separation;
- identity fusion, recovery/FEC, distributed/cloud execution, frozen CODA v1,
  or completion of Stage D / Audio Stream Profile 1.0.

## Stage D continuation

After D.7, production runtime/model-bundle compatibility remains the hard gate
for making neural separation available. Streaming orchestration should follow
only after the offline D truth/provenance contract and a compatible runtime are
both proven. Identity fusion and broader audio validation remain separate
milestones.
