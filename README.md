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
  - TruthClass
  - StreamDescriptor
  - StreamClock
  - StreamEpoch

implemented_audio_profile:
  - deterministic APS1 PCM16 S1 canonicalization and encoding
  - preservation-first PCM16 WAV ingest
  - provenance-verified PCM16 state query
  - provenance-verified PCM16 WAV export
  - provenance-verified PCM16 FLAC export
  - preservation-first native PCM16 FLAC ingest
  - verified-S1-to-D offline PCM16 separation orchestration with caller-supplied backend
  - deterministic verified AMB1 separation ModelBundle encoding/decoding and exact model/bundle identity
  - caller-activated ONNX Runtime CPU separation backend for compatible verified AMB1 bundles

capabilities:
  neural_separation: false
  gpu_inference: false
```

The ONNX Runtime CPU backend is an additive Audio Profile execution boundary. A build configured with `CODEC_ONNXRUNTIME_ROOT` can validate and execute a caller-supplied compatible verified AMB1 bundle through a caller-selected ONNX Runtime shared library. CODEC does not bundle a runtime or production model, does not enable the default neural backend, and does not claim model quality, trust, licensing validity, GPU support, or production-scale performance. Sanitizer CI keeps leak detection enabled globally; only the isolated ONNX runtime test process carries a documented `dlopen()` loader-lifetime suppression for released third-party runtime allocations.

See the detailed design and implementation records under `docs/superpowers/` for milestone-specific contracts, proof, and non-claims.
