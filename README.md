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
    - C++ API, C ABI, CLI
  audio_profile:
    - PCM16 RIFF/WAVE exact read/write
    - Ed25519/COSE W0 signed statements
    - W1 reference sub-20-kHz carrier/detector
    - W2 reference >24-kHz carrier with sample-rate/Nyquist gating
    - identity candidates and explicit non-authoritative stateless fusion
  inference:
    - backend boundary exists
    - no bundled neural model/runtime; incompatible model returns model_incompatible

planned_not_implemented:
  - generalized Stream* engine/API migration beyond metadata primitives
  - persisted generic policy tags
  - generalized S1 canonical-state implementations beyond audio semantics
  - video, telemetry, sensor, document/event, network/system profiles
  - production neural separation/diarization/identity models
  - recovery/FEC profile
  - distributed/cloud execution profile
  - trust/selective-disclosure profile

profile_rule:
  generic_requirement_wins_core_conflict: true
  audio_is_first_reference_profile: true
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

For audio, S1 may mean sample-exact canonical integer PCM. Resampling, enhancement, concealment, separation, embeddings, and inferred labels remain D-class. CODEC-generated watermark derivatives must not mutate preserved S0/S1 truth.

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
