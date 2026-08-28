# CODEC

**Channel-Oriented Decomposition, Extraction, and Capture**

CODEC is a C++20, stream-first engine and archive substrate for authorized heterogeneous temporal streams. It preserves source truth in **CODA** (`.coda`), keeps derived intelligence traceable to source evidence, and lets stream-specific profiles add exact normalization, inference, identity, recovery, and export behavior without redefining core semantics.

> **AI entry point:** repository-aware agents start with [`AGENTS.md`](AGENTS.md); otherwise read this manifest first. Then use [`AI_WORKSHEET.md`](AI_WORKSHEET.md) for execution. Detailed rationale lives in the linked design spec; do not duplicate it here.

## AI manifest

```yaml
project:
  name: CODEC
  version: 0.1.0
  language: C++20
  build: CMake >= 3.20
  archive: CODA (.coda)
  architecture: stream-first, payload-type agnostic

truth_classes:
  S0: exact accepted source representation
  S1: exact canonical state defined by a registered profile
  D: derived/inferred/transformed artifact with provenance

core_invariants:
  - preserve S0 before optional interpretation
  - never silently conflate S0, S1, and D
  - keep logical stream identity independent of transport, worker, file, and archive segment
  - preserve time, sequence, epochs, gaps, integrity, and provenance explicitly
  - keep profile-specific fields out of generic core records unless universal
  - inference/profile failure must not corrupt committed preservation
  - unknown compatible stream/payload types remain preservable and extractable
  - derived results remain traceable to exact supporting source records/intervals
  - scale claims require measured bandwidth/compute/memory/storage/latency evidence
  - unimplemented capability must remain explicitly unavailable

core_vocabulary:
  - StreamId
  - StreamType
  - StreamSpec
  - StreamDescriptor
  - StreamClock
  - StreamEpoch
  - StreamRecord
  - StreamProvenance
  - StreamAdapter
  - StreamProcessor
  - StreamExporter
  - StreamInference
  - StreamExtraction

implemented_v0_1:
  generic:
    - append-only CODA development profile
    - SHA-256 payload/chain integrity evidence
    - commit trailers, final index, verification, exact S0 extraction, non-mutating repair
    - bounded file/stdin/HTTP/HTTPS capture with hardened path/network policy
    - generic C++ metadata primitives: StreamId, StreamType, TruthClass, StreamDescriptor, StreamClock, StreamEpoch
    - versioned generic StreamDescriptor archive records and C++ append/list APIs, with legacy feed descriptors projected as opaque streams with unspecified payload type
    - versioned generic S0 timing and gap records with C++ append/read APIs, per-stream sequence/epoch validation, explicit missing ranges, and exact prior source-record SHA-256 links preserved across repair
    - versioned declared S1/D provenance sidecars with C++ append/read APIs, exact backward-only subject/input record links, bounded generic process identity, typed opaque profile details, and repair preservation
    - AND-combined C++ direct-provenance queries over S1/D truth class plus physical subject and immediate-input filters, preserving sidecar order and exact record links
    - explicit append/extract/repair round trips for unknown 16-bit development-profile record type codes without payload interpretation
    - AND-combined C++ physical record queries over exact stream/raw type and half-open archive-sequence/envelope-time ranges, plus boundary-preserving per-record exact payload extraction
    - pull-based C++ StreamAdapter S0 and bounded batch StreamProcessor S1/D contracts with interval, truth, process-metadata, and resource validation; registration, scheduling, and automatic persistence remain external
    - bounded caller-supplied C++ StreamExporter contract for typed external representations from exact extracted record batches, with input/output/payload-type limits and exact ordered physical support links; registry, automatic query, persistence, CLI, and C ABI export remain external
    - typed C++ generic stream recording with caller-owned stable StreamId and exact StreamDescriptor persistence through the existing hardened URI capture path; FeedSpec recording remains compatible
    - generic CLI stream listing and exact S0 extraction by stable StreamId; legacy feed list/extract remains available
    - C++ API, C ABI, CLI
  audio_profile:
    - explicit C++ profile facade at codec/profiles/audio.hpp in codec::profiles::audio, forwarding the exact existing WAV/PCM, watermark, and separation types/functions while root-level codec::* audio APIs remain compatible
    - deterministic sample-exact PCM16 S1 canonical state with a versioned self-contained APS1 encoding, strict decode validation, generic pcm16-record storage, and exact S0 provenance links
    - bounded preservation-first PCM16 WAV ingest that captures one owned snapshot, commits its exact S0 before profile interpretation, emits APS1 S1 with exact provenance on success, and finalizes the verified S0-only archive with an explicit profile error when interpretation fails
    - bounded preservation-first native PCM16 FLAC ingest that captures one owned audio/flac snapshot, commits its byte-exact S0 before decoding, accepts only native signed 16-bit FLAC under an independent decoded-PCM bound, emits the existing APS1 S1 with exact provenance on success, and finalizes malformed, unsupported 24-bit, Ogg-container, or over-limit input as verified S0-only with explicit profile error
    - bounded finalized-archive PCM16 S1 query that returns decoded APS1 state only when exact state_exact subject/source lineage resolves to one same-stream, same-interval S0 record; unprovenanced pcm16 records remain unclassified
    - bounded finalized-archive verified PCM16 WAV export that returns deterministic in-memory audio/wav bytes for D.3-verified S1 with exact state/source/provenance evidence and performs no archive or filesystem write
    - bounded finalized-archive verified PCM16 FLAC export that emits native in-memory audio/flac only for D.3-verified S1, enables libFLAC verification, proves sample-exact decode round trips, retains exact state/source/provenance evidence, and keeps APS1 as the canonical S1 identity
    - bounded offline PCM16 separation orchestration over explicit intervals and D.3-verified S1 using a caller-supplied backend, returning D-class APS1 stems plus mandatory residual with exact S1 support, model/runtime/configuration identity, and independent reconstruction metrics without archive mutation or a bundled-neural-runtime claim
    - bounded deterministic AMB1 Audio separation ModelBundle encoding and strict in-memory decoding with canonical manifest metadata, exact opaque ONNX-byte SHA-256 verification, whole-bundle identity, and no filesystem/archive persistence or model execution
    - caller-activated ONNX Runtime CPU separation backend with D.8 identity revalidation, real in-memory graph/session compatibility checks, bounded CODEC-owned window/output buffers, deterministic PCM16 overlap/add, mandatory residual construction, and D.7 integration; runtime binaries and production models are not bundled
    - PCM16 RIFF/WAVE exact read/write
    - Ed25519/COSE W0 signed statements
    - W1 reference sub-20-kHz carrier/detector
    - W2 reference >24-kHz carrier with sample-rate/Nyquist gating
    - identity candidates and explicit non-authoritative stateless fusion
  inference:
    - backend boundary exists
    - bounded structural AMB1 separation-bundle loader exists
    - optional caller-activated ONNX Runtime CPU adapter exists when built with compatible private headers and a runtime library is supplied
    - no production model or default runtime selection is bundled; the default backend returns model_incompatible

planned_not_implemented:
  - generalized Stream* CLI recording/processing/export and C ABI migration; profile API migration beyond the explicit Audio Stream Profile facade
  - persisted generic policy tags
  - generalized S1 canonical-state implementations beyond deterministic audio PCM16
  - video, telemetry, sensor, document/event, network/system profiles
  - production neural separation/diarization/identity models
  - recovery/FEC profile
  - distributed/cloud execution profile
  - trust/selective-disclosure profile

profile_rule:
  generic_requirement_wins_core_conflict: true
  audio_is_first_reference_profile: true
  root_audio_names_remain_v0_1_compatibility_surface: true
  current_Feed_names_may_remain_for_v0_1_compatibility: true

roadmap_order:
  - stabilize generic stream identity/type/time/provenance/archive semantics
  - expose generic adapter/processor/query/extraction boundaries
  - complete Audio Stream Profile 1.0
  - add recovery/multiplexing
  - add distributed/cloud execution
  - add trust/selective-disclosure
  - add more stream/vertical profiles
```

## Runtime truth vs specification

Use this precedence when deciding what CODEC **currently does**:

1. code + tests + runtime capability output;
2. `CMakeLists.txt`, public headers, CLI behavior, and `CHANGELOG.md`;
3. this README for architectural invariants and current-status summary;
4. design documents for rationale and future direction.

A planned item is **not implemented** until code and tests prove it and the status above is updated.

## Build and verify

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/codec capabilities
```

Sanitizer gate:

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

## Repository map

| Path | Purpose |
|---|---|
| `AGENTS.md` | Automatic cold-start and repository-wide agent instructions |
| `include/` | Public C++/C interfaces |
| `src/archive/` | CODA archive/integrity implementation |
| `src/capture/` | Source ingest/capture |
| `src/core/` | Engine/core primitives; currently includes compatibility-era feed naming |
| `src/audio/` | Audio Stream Profile implementation |
| `src/watermark/` | Audio W0/W1/W2 identity implementation |
| `src/inference/` | Inference backend boundary |
| `src/cli/` | CLI |
| `tests/` | Unit, C ABI, and CLI integration tests |
| `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md` | Approved stream-first architecture |
| `AI_WORKSHEET.md` | Canonical AI implementation/verification worksheet |

## Core record direction

Conceptual only; this is not a claim about the frozen v0.1 binary layout.

```text
RecordEnvelope {
  stream_id, stream_type, source_id,
  monotonic_time, observed_utc, source_timestamp,
  sequence, format_epoch, connection_epoch,
  truth_class, payload_type, payload,
  payload_hash, previous_record_hash,
  provenance, policy_tags
}
```

Physical transport, logical stream identity, processing partition, and archive placement are separate concerns. One endpoint may multiplex many logical streams; one stream may migrate while retaining identity and provenance.

## Audio Stream Profile

Audio is the first implemented profile, not CODEC core. PCM/WAV, sample rate/channel layout, FLAC, W0/W1/W2, diarization, speaker embeddings, neural source separation/stems, and audio-specific fidelity tests belong to this profile.

The canonical additive C++ profile entry point is `<codec/profiles/audio.hpp>` under `codec::profiles::audio`. It aliases/imports the existing WAV/PCM, watermark, and separation surface rather than moving ABI-bearing symbols; existing root-level `codec::*` audio headers and names remain the v0.1 compatibility surface.

For audio, `Pcm16State` is the implemented sample-exact S1 contract: non-zero sample rate and channel count, complete interleaved frames, and exact signed 16-bit samples. `encode_pcm16_state` and `decode_pcm16_state` use the deterministic versioned `APS1` payload. A `pcm16` record is only S1 when a `state_exact` provenance sidecar links it to its exact S0 input; the record tag alone does not make that claim. Resampling, remixing, channel-layout interpretation, floating-point PCM, enhancement, concealment, separation, embeddings, and inferred labels remain outside the implemented canonical-state contract. FLAC is implemented as a preservation-first native PCM16 ingest representation and as the external lossless output representation described below; neither changes APS1 as the S1 identity. CODEC-generated watermark derivatives must not mutate preserved S0/S1 truth.

`ingest_pcm16_wav` is the additive preservation-first C++ path for one bounded `audio/wav` source. It validates the request before opening the source or creating output, captures exactly once through the hardened URI boundary, appends the descriptor and byte-exact S0, and then interprets those same in-memory bytes. Successful PCM16 decoding appends deterministic `APS1` state plus `state_exact` provenance. A WAV/profile decode failure is reported in `Pcm16WavIngestReport::profile_error` while the finalized, verified S0-only archive remains a successful ingest result. Capture and archive I/O failures remain ordinary outer errors; this API does not claim filesystem transactions, automatic recovery, conversion, inference, CLI, or C ABI support.

`ingest_pcm16_flac` is the additive preservation-first C++ path for one bounded native `audio/flac` source. It validates before capture/output, captures one owned snapshot through the same hardened URI boundary, commits the descriptor and byte-exact FLAC S0, and only then decodes those same bytes through a private libFLAC decoder with MD5 checking and an independent caller-supplied decoded-PCM byte limit. Successful native signed 16-bit FLAC decoding appends the existing deterministic APS1 state plus exact `state_exact` provenance on the same stream and interval. Malformed native FLAC, unsupported non-16-bit FLAC, Ogg-FLAC/container input, or decoded-output exhaustion is reported in `Pcm16FlacIngestReport::profile_error` after finalizing a verified S0-only archive. The path does not rewrite the source FLAC, does not claim input/output FLAC byte identity, and adds no FFmpeg/general conversion, resampling/remixing, CLI/C ABI, or inference behavior.

`query_verified_pcm16_states` is the additive trusted read path for finalized archives. It filters `state_exact` provenance subjects to `pcm16`, resolves the exact subject and its single direct `source_bytes` input, requires the same logical stream and authenticated interval, enforces caller result and encoded-byte bounds before payload decode, and then decodes only the verified APS1 subject. Physical `pcm16` records without matching state-exact provenance are not promoted to S1. Contradictory selected lineage fails closed rather than being silently skipped. The returned `VerifiedPcm16State` retains both exact physical records and the full provenance object used to justify the state classification.

`export_verified_pcm16_wav` is the additive lossless WAV output path for those D.3-verified states. It preserves D.3 ordering, preflights an aggregate caller output-byte limit before generating any WAV result, reads only the exact APS1 subject needed for each export, and invokes the generic exporter contract through a private Audio Profile implementation. Each result carries deterministic in-memory PCM16 RIFF/WAVE bytes, the exact APS1 support link, and the D.3 state/source/provenance evidence. The API does not write files or archives and does not classify the external WAV bytes as a new CODA S1/D record.

`export_verified_pcm16_flac` is the additive native-FLAC output path for the same D.3-verified states. It uses libFLAC through a private Audio Profile encoder, keeps streamable-subset mode and encoder verification enabled, preserves the exact APS1 sample rate, channels, interleaving, frame/sample values, and retains the exact APS1 support link plus D.3 state/source/provenance evidence. Output is bounded in memory and returned as `audio/flac`; the API performs no archive, filesystem, or network write. Independent libFLAC decoder coverage proves sample-exact PCM round trips. The FLAC bitstream is an external representation rather than a new canonical S1: APS1 remains the S1 identity, and CODEC does not claim byte-for-byte stability across different compatible libFLAC versions. This path does not add FFmpeg/general media conversion, resampling, remixing, CLI or C ABI FLAC export, or neural inference.

`separate_verified_pcm16_offline` is the additive bounded offline processing path over an explicit interval of D.3-verified PCM16 S1. It invokes one caller-supplied `SeparationBackend` per selected state through the generic processor validator, requires bounded and geometry-compatible stems plus a mandatory residual, and returns each APS1-encoded output explicitly as D with exact physical S1 support and the full verified S1-to-S0 lineage. Every run retains backend/provider identity, a backend-reported SHA-256 model identity, a deterministic request-configuration hash, typed role metadata, and independently computed maximum-absolute and RMS sample-domain reconstruction error alongside the backend metric. Results are caller-persistable but this function does not write or mutate an archive. The default backend remains explicitly `model_incompatible`; this orchestration does not bundle a neural model/runtime, make neural/GPU capabilities available, add streaming/latency/quality claims, or perform identity fusion.

`encode_separation_model_bundle` and `decode_separation_model_bundle` implement the additive in-memory AMB1 structural and integrity boundary for Audio separation models. AMB1 deterministically binds bounded printable manifest identity, license and quality-domain metadata, exact sample/framing/source geometry, causal behavior, and distinct tensor names to opaque ONNX bytes by SHA-256; strict decode rejects unknown flags, noncanonical lengths, malformed metadata, truncation, trailing bytes, and model-hash mismatch before returning owned verified bytes plus whole-bundle identity. Version 1 fixes float32 input `[batch, channel, sample]`, float32 output `[batch, source, channel, sample]`, signed PCM16 divided by 32768.0, and source-waveform output semantics so a later runtime has one unambiguous compatibility target. The bundle hashes prove byte identity only: this API does not parse or execute ONNX, authenticate a signer, validate licensing or quality, load a provider, access a filesystem/network/archive, or make neural/GPU capabilities available.

`create_onnx_cpu_separation_backend` is the additive caller-activated D.9 execution boundary for a D.8 verified bundle. When CODEC is built with private ONNX Runtime headers, the factory re-encodes and rechecks exact model/bundle identities before loading the caller-selected runtime library, creates an in-memory sequential CPU session, and requires one named float32 `[1, channel, window]` input plus one fixed float32 `[1, source, channel, window]` output compatible with AMB1. The returned legacy backend applies checked input/window/output bounds, deterministic zero-padded rectangular overlap/add, PCM16 normalization and saturation, and mandatory residual construction; D.7 independently validates and records the resulting D artifacts and provenance. Builds without those headers return explicit `model_incompatible`, and the default backend remains unavailable because CODEC bundles neither a production model nor a runtime selection. `onnx_cpu_separation_runtime_compiled()` reports build support only—not library/model availability, quality, trust, or safety. CODEC does not authenticate, sandbox, download, or qualify caller models, hard-limit ONNX Runtime's internal graph allocations, expose GPU providers, or claim neural separation/streaming/latency/quality availability.

## Security and scope

- Capture only authorized sources; do not bypass DRM, encryption, access controls, paywalls, or provider restrictions.
- Keep secrets external where possible and redact configured secrets from descriptors/provenance.
- Treat signatures as authentication of statements, not automatic proof of physical-world conclusions.
- CODEC may record/enforce configured authorization metadata, but it does not create legal authority.

## Contribution rule

Before changing CODEC, use [`AI_WORKSHEET.md`](AI_WORKSHEET.md). Preserve the invariants above, add tests for new exactness/archive claims, keep current-status claims synchronized with code, and prefer generic stream primitives over profile-specific coupling.

## References

- [`AGENTS.md`](AGENTS.md) — cold-start instructions for repository-aware agents.
- [`AI_WORKSHEET.md`](AI_WORKSHEET.md) — canonical AI work loop and merge gate.
- [`docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`](docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md) — full stream-first architecture and rationale.
- [`CHANGELOG.md`](CHANGELOG.md) — released changes.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contribution basics.
