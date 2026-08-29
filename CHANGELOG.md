# Changelog

All notable changes use semantic versioning.

## Unreleased

- Add Stage E.5 bounded `StreamingRepairSession` orchestration over the existing CMX1, E.2, and XRF1 contracts: register explicit recovery groups, accept complete canonical CMX1 frames and strict XRF1 symbols in either order, tolerate bounded source reordering, exact duplicates, and provisional late gap fills before seal, withhold observed frames until their exact XRF1 length/SHA-256 commitments plus CMX1 integrity and stream/epoch/sequence membership verify, and invoke E.4 reconstruction only after explicit sealing proves exactly one unresolved source slot. Zero or multiple erasures do not trigger recovery. The additive C++ API is installed-package tested and completes the Stage E architecture gate at this bounded XOR single-erasure scope. No sockets/network provider, retransmission/ARQ, automatic CODA persistence, authentication/authorization, CLI/C ABI recovery, multi-erasure correction, measured loss-tolerance/performance/scale claim, or S0/S1/D semantic change is added.
- Add Stage E.4 bounded deterministic XRF1 XOR repair symbols over explicit E.2 recovery groups: commit each complete CMX1 source frame's exact encoded length and SHA-256, serialize canonical slot metadata plus zero-padded XOR parity under a trailing integrity digest, and reconstruct exactly one known missing frame only after observed commitments, recovered hash, CMX1 integrity, and exact stream/epoch/sequence membership verify. The additive C++ API is caller-bounded and installed-package tested. No S0/S1/D, CODA, CMX1, E.2 tracker, or E.3 recording change; no bit-error location/correction, multi-erasure Reed–Solomon/fountain coding, network transport, retransmission, authentication, automatic persistence, CLI/C ABI, streaming orchestration, benchmark/scale claim, frozen normative wire claim, or Stage E completion is added.

## 0.2.0 — 2026-08-28

- Add Stage E.3 concurrent live multi-feed recording and verified-prefix follow extraction. Repeated `codec record --feed LABEL=URI` inputs are prepared and descriptor-committed before one producer per source starts; bounded per-stream/aggregate queues apply blocking backpressure while one writer thread serializes exact S0 bytes into CODA without deterministic cross-stream ordering claims. Add `<codec/archive_follow.hpp>` with bounded paginated `SourceExactCursor` extraction and CLI `extract --feed/--stream ... --fidelity source-exact --follow`, which emits only the selected stream's committed verified-prefix records, appends incrementally while the archive remains open, and exits when a committed final index becomes visible. A page boundary never skips an unreturned selected record; a single source record larger than `maximum_bytes` still fails with `resource_exhausted`. No CMX1-in-CODA wrapping, network provider, FEC/retransmission, S1/D generation, deterministic cross-stream order, hard real-time/scale claim, C ABI follow API, or Stage E completion claim is added.
- Add Stage E.2 bounded generic loss/recovery semantics in `<codec/recovery.hpp>`: `SequenceLossObserver` tracks provisional half-open missing sequence ranges independently per exact logical stream and connection/format epoch, allows late frames to fill those ranges without promoting every forward jump to permanent loss, and enforces transactional track/range bounds; `RecoveryGroupTracker` tracks caller-declared non-overlapping same-stream/same-epoch source ranges, accepts out-of-order members and duplicates, and explicitly seals them into deterministic complete/incomplete reports with exact unresolved ranges. Actual E.1 CMX1 decode and installed-package use are covered. No CMX1/CODA format change, automatic `StreamGap` persistence, FEC/parity/repair-symbol encoding, reconstruction, retransmission/ARQ, network I/O, authentication, CLI/C ABI recovery command, performance/scale claim, or Stage E completion claim is added; an incomplete sealed group is not itself a claim that repair is possible.
- Add Stage E.1 generic `CMX1` multiplex framing and incremental demultiplexing: interleave many logical `StreamId`s over one physical byte stream while preserving each frame's independent sequence, connection/format epochs, generic clock, authenticated interval metadata, and opaque payload exactly; enforce bounded buffering/backpressure, deterministic little-endian framing, early malformed-header rejection, and SHA-256 corruption detection without assigning S0/S1/D truth. No socket/network provider, cryptographic authentication, ordering/gap inference, retransmission, recovery/FEC, CODA layout, CLI/C ABI transport command, performance/scale claim, frozen normative wire standard, or Stage E completion claim is added.
- Add a caller-activated Audio Profile ONNX Runtime CPU separation backend: revalidate exact D.8 model/bundle identities, dynamically load an explicitly selected runtime, create and validate a real in-memory one-input/one-output float32 session, execute deterministic bounded CODEC-owned window/overlap buffers, return PCM16 stems plus the mandatory residual, and prove actual D.7 verified-S1-to-D integration. ONNX Runtime 1.29.0 is pinned only for CI proof; CODEC bundles no runtime or production model, keeps the default backend and neural/GPU capabilities unavailable, and makes no model trust/license/quality, sandbox/internal-runtime-memory, streaming, latency/scale, CLI/C ABI, CODA layout, or Stage D completion claim.
- Add bounded deterministic Audio Profile AMB1 separation ModelBundle encoding and strict in-memory decoding: canonically bind manifest identity, license/quality-domain metadata, exact input/framing/source geometry, causal behavior, and fixed float32 tensor/PCM semantics to opaque ONNX bytes by SHA-256; return exact model and whole-bundle identity only after bounded structural, semantic, and hash verification. No ONNX parsing/execution, compatible runtime or production model, signature/trust validation, filesystem/network/archive persistence, neural/GPU availability, CODA layout, CLI/C ABI, or Stage D completion claim is added; the default backend remains `model_incompatible`.
- Add bounded Audio Profile `separate_verified_pcm16_offline` orchestration over explicit intervals of D.3-verified APS1 PCM16 S1: invoke a caller-supplied separation backend through the generic processor validator, return bounded D-class APS1 stems plus mandatory residual with exact S1 support and full verified lineage, retain backend/provider/model/configuration identity, and independently compute maximum-absolute and RMS sample-domain reconstruction metrics without mutating the archive. The default backend remains explicitly `model_incompatible`; no bundled neural model/runtime, neural/GPU availability, streaming, quality/latency, CLI/C ABI, identity-fusion, CODA-layout, or Stage D completion claim is added.
- Add bounded preservation-first Audio Profile native PCM16 FLAC ingest through `codec::profiles::audio::ingest_pcm16_flac`: validate before output, capture one owned `audio/flac` snapshot, commit exact FLAC S0 before interpreting those same bytes, decode only native signed 16-bit FLAC under an independent decoded-PCM byte bound, append the existing APS1 S1 with exact `state_exact` provenance on success, and finalize a verified S0-only archive with explicit `decode` or `resource_exhausted` profile error for malformed, unsupported 24-bit, Ogg-container, or over-limit inputs. D.3 verifies the resulting state and D.5 round-trip tests prove exact PCM; no CODA layout, source-FLAC rewriting, FFmpeg/general conversion, resampling/remixing, CLI/C ABI, inference, or Stage D completion change.
- Add bounded Audio Profile `export_verified_pcm16_flac` output over finalized CODA archives using libFLAC: consume only D.3 provenance-verified APS1 PCM16 S1, emit native in-memory `audio/flac`, enable libFLAC encoder verification, prove sample-exact decode round trips, retain exact state/source/provenance evidence, and enforce aggregate output bounds; APS1 remains canonical S1 and cross-libFLAC-version byte determinism is not claimed. No CODA layout, FFmpeg/general conversion, resampling/remixing, CLI/C ABI export, or inference change.
- Add bounded finalized-archive verified PCM16 WAV export that returns deterministic in-memory audio/wav bytes for D.3-verified S1 with exact state/source/provenance evidence and performs no archive or filesystem write
- Add bounded finalized-archive verified PCM16 FLAC export that emits native in-memory audio/flac only for D.3-verified S1, enables libFLAC verification, proves sample-exact decode round trips, retains exact state/source/provenance evidence, and keeps APS1 as the canonical S1 identity
- Add bounded offline PCM16 separation orchestration over explicit intervals and D.3-verified S1 using a caller-supplied backend, returning D-class APS1 stems plus mandatory residual with exact S1 support, model/runtime/configuration identity, and independent reconstruction metrics without archive mutation or a bundled-neural-runtime claim
- Add bounded deterministic AMB1 Audio separation ModelBundle encoding and strict in-memory decoding with canonical manifest metadata, exact opaque ONNX-byte SHA-256 verification, whole-bundle identity, and no filesystem/archive persistence or model execution
- Add caller-activated ONNX Runtime CPU separation backend with D.8 identity revalidation, real in-memory graph/session compatibility checks, bounded CODEC-owned window/output buffers, deterministic PCM16 overlap/add, mandatory residual construction, and D.7 integration; runtime binaries and production models are not bundled
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
  - concrete transport repair-symbol encoding, FEC/parity generation, reconstruction, retransmission/ARQ, and measured loss-tolerance claims beyond implemented E.1/E.2 framing and recovery semantics
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
  - extend Stage E transport/recovery beyond implemented multiplex framing, recovery observation, and live concurrent capture/follow extraction
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
| `src/transport/` | Generic transport-profile framing, demultiplexing, loss observation, recovery groups, and bounded XOR single-erasure repair |
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

## Transport / Recovery Profile

Stage E started with the additive C++ `CMX1` multiplex boundary in `<codec/transport.hpp>`. `encode_multiplex_frame` deterministically frames one logical `StreamId` plus its independent sequence, connection/format epochs, generic `StreamClock`, interval, and opaque bytes; `MultiplexDecoder` incrementally demultiplexes arbitrary physical chunks in arrival order with caller-bounded buffering and frame-count backpressure. The version-1 frame SHA-256 detects corruption of its semantic header or payload but is not a signature, MAC, authentication, or authorization mechanism. E.1 deliberately does not assign S0/S1/D truth, infer gaps, reorder, retransmit, recover loss, generate parity/FEC, resynchronize after corruption, perform network I/O, write CODA, or claim throughput/latency/scale.

E.2 adds `<codec/recovery.hpp>` above CMX1 without changing CMX1 bytes. `SequenceLossObserver` keeps bounded provisional missing sequence ranges independently for each exact `(StreamId, connection epoch, format epoch)` track: the first observation establishes a baseline, forward jumps open provisional ranges, and late members can fill or split those ranges; older observations outside a current gap remain only `late_or_replayed`. `RecoveryGroupTracker` separately tracks caller-declared non-overlapping source-sequence ranges within one exact stream/epoch namespace, accepts out-of-order first observations and duplicates, and freezes exact unresolved ranges only when the caller explicitly seals a group. `collecting`/`observed_complete` are pre-seal observations; `sealed_complete`/`sealed_incomplete` are closure states. An incomplete group is **not** a claim that CODEC can or cannot reconstruct it. E.2 does not automatically persist `StreamGap` records, encode repair symbols, generate parity/FEC, reconstruct missing frames, retransmit/ARQ, perform network I/O, authenticate transport, expose recovery through CLI/C ABI, or claim measured loss tolerance, throughput, latency, scale, or Stage E completion.

E.3 adds live local multiplexed capture and verified-prefix follow extraction without changing CODA or CMX1 bytes. Repeated `codec record --feed LABEL=URI` inputs are prepared first, all feed/stream descriptors are committed before capture producers begin, and each prepared source runs concurrently behind caller-bounded queues. Backpressure blocks rather than drops; only the calling writer thread mutates CODA, so each logical stream retains exact S0 byte order while physical cross-stream archive order remains scheduler/I/O dependent. `<codec/archive_follow.hpp>` exposes `SourceExactCursor` plus bounded `extract_stream_source_exact_prefix()`: it reads only committed `verified_prefix` records, returns only exact-stream `source_bytes`, and treats record/byte limits as pagination. The cursor never advances past an unreturned selected record; if the first pending selected record alone exceeds `maximum_bytes`, the call returns `resource_exhausted`. CLI `extract --feed LABEL --fidelity source-exact --follow` and the equivalent `--stream STREAM_ID` path open output once, append newly committed selected bytes, remain attached while the archive is open, and exit after a committed final index becomes visible. E.3 does not wrap local CODA writes in CMX1, provide a network transport/provider, generate S1/D, add FEC/retransmission, promise deterministic cross-stream interleaving, expose a C ABI follow API, or claim hard real-time behavior, measured scale, or Stage E completion.

E.4 adds `<codec/xor_recovery.hpp>` as the first concrete bounded repair scheme without changing CMX1, E.2, or CODA bytes. `create_xor_repair_symbol` deterministically encodes every exact member of one explicit E.2 `RecoveryGroupDescriptor`, commits its exact CMX1 length and SHA-256, and XORs the complete encodings with zero padding into one parity vector. The canonical version-1 `XRF1` representation binds that table and parity under a trailing integrity digest. `recover_xor_single_erasure` accepts exactly all but one unique group member, verifies every observed commitment, reconstructs and truncates the missing exact encoding, verifies its committed SHA-256, decodes exactly one valid CMX1 frame, and rechecks its stream, epochs, and sequence before return. This is exact encoded-data recovery for one known erasure only; SHA-256 is not authentication. E.4 does not locate/correct bit errors, recover multiple erasures, add Reed–Solomon/fountain coding, provide network transport or retransmission, persist symbols or recovered frames automatically, change S0/S1/D truth, expose CLI/C ABI recovery, claim measured performance/scale, orchestrate streaming repair, freeze a normative wire standard, or complete Stage E.

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
- Keep secrets external where possible and redact configured secrets/provenance.
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
- Add an explicit unavailable neural separation backend instead of claiming inference without compatible model weights.