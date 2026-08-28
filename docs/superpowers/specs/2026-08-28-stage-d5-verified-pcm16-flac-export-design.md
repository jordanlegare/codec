# Stage D.5 Provenance-Verified PCM16 FLAC Export Design

## Status

Approved for implementation on 2026-08-28 as the next Audio Stream Profile 1.0 milestone after D.4.

## Context

Stage D is active. D.1 defines deterministic APS1 PCM16 S1, D.2 preserves exact WAV S0 before canonicalization, D.3 exposes a provenance-verified finalized-archive S1 read boundary, and D.4 proves a concrete trusted exporter path by emitting deterministic in-memory PCM16 WAV only from D.3-verified state.

The Stage D exit scope still requires a lossless external representation/FLAC where appropriate. CODEC currently has no FLAC dependency or FLAC public surface. The smallest dependency-consuming D.5 milestone is therefore a FLAC output path over the existing D.3 trusted state boundary. FLAC ingest, general media conversion, and production media adapters remain separate later work.

## Goal

Add a bounded Audio Profile API that exports D.3-verified APS1 PCM16 S1 as native lossless FLAC in memory while retaining the exact state record, exact S0 source record, and the provenance object that justified the S1 classification.

## Truth model

APS1 remains the sole canonical PCM16 S1 identity. D.5 does not create a second canonical audio state and does not classify the produced FLAC bytes as a new CODA S1 or D record.

A D.5 output is an external lossless representation of already-verified S1. Public D.5 selection must therefore delegate to `query_verified_pcm16_states()` rather than independently querying physical `pcm16` records or rejoining provenance.

Unprovenanced physical `pcm16` records remain unclassified and are not exportable through the D.5 API. Contradictory selected lineage continues to fail closed through D.3.

## Public API

Create `include/codec/profiles/audio_flac_export.hpp` and include it from `include/codec/profiles/audio.hpp`.

```cpp
namespace codec::profiles::audio {

struct Pcm16FlacExportLimits {
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16FlacExport {
  ExportResult output;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16FlacExport>> export_verified_pcm16_flac(
    const CodaArchive& archive,
    const Pcm16StateQuery& query = {},
    Pcm16FlacExportLimits limits = {});

}
```

No root-level alias is added. This is new profile-only behavior.

## Internal architecture

### 1. FLAC encoder boundary

Add `src/audio/flac_encoder.hpp` and `src/audio/flac_encoder.cpp` under `codec::profiles::audio::detail`.

The encoder accepts one validated `Pcm16State` plus a caller-provided maximum output byte count and returns `Result<std::vector<std::byte>>`.

It uses libFLAC's stream encoder API with:

- native FLAC stream output, not Ogg FLAC;
- 16 bits per sample;
- the exact APS1 sample rate and channel count;
- exact interleaved sample order;
- total sample count declared from APS1 frame count;
- streamable-subset mode enabled;
- a fixed compression level of 5;
- encoder verify mode enabled;
- in-memory write/seek/tell callbacks;
- no filename, archive, network, or filesystem output.

The private callback state owns the output vector and maximum byte budget. The write callback rejects any growth that would exceed the supplied limit. Seek/tell callbacks operate only within the currently owned vector and reject impossible offsets. Any libFLAC setup, init, process, finish, verify, or callback failure maps to a CODEC error and returns no output payload.

PCM16 samples are promoted losslessly to libFLAC's signed 32-bit input type; no scaling, resampling, remixing, dither, normalization, channel reinterpretation, or floating-point conversion occurs.

### 2. Private StreamExporter

Add a private `Pcm16FlacExporter final : StreamExporter` in `src/audio/pcm16_flac_export.cpp`.

It accepts exactly one `ExtractedRecord`, requires physical type `RecordType::pcm16`, decodes APS1 with `decode_pcm16_state()`, invokes the private FLAC encoder, and returns:

```cpp
ExporterOutput{
  .payload_type = "audio/flac",
  .payload = ...,
};
```

The exporter is invoked only through the generic C.5 `invoke_exporter()` contract, preserving the exact APS1 support link in `ExportResult::supporting_records`.

### 3. Trusted D.5 wrapper

`export_verified_pcm16_flac()` validates a non-zero aggregate output limit and calls `query_verified_pcm16_states()` with the caller's exact query.

For each verified state, in D.3 order:

1. compute the remaining aggregate output budget;
2. reject if no budget remains;
3. read only the exact APS1 subject with `CodaArchive::read_payload()` so the archive/trailer hash is checked on the exact state record;
4. invoke the private FLAC exporter with one input and the remaining aggregate byte budget;
5. add the emitted payload size to the aggregate count with overflow-safe arithmetic;
6. retain the D.3 state record, source record, and provenance beside the generic export result.

The function returns the vector only after every selected state succeeds. If a later state exceeds the aggregate limit or encoding fails, the function returns an error and no partial result vector is observable to the caller.

Unlike D.4 WAV, exact FLAC byte identity is not a cross-libFLAC-version contract. FLAC encoder metadata, including vendor/version information, may vary by compatible libFLAC release. D.5's fidelity claim is exact decoded PCM plus exact D.3 lineage. Tests may prove deterministic bytes within one CI environment but documentation must not elevate that to a stable cross-version format guarantee.

## Dependency and packaging

Add libFLAC as a required native dependency for `codec_core`.

CMake must discover the installed libFLAC package and link its canonical imported target `FLAC::FLAC`. The repository CI image must install `libflac-dev` before configure. The installed `codec-config.cmake` must rediscover FLAC before importing `codec-targets.cmake`, so an external `find_package(codec 0.1 CONFIG REQUIRED)` consumer remains valid.

Do not vendor libFLAC, shell out to the `flac` executable, or introduce FFmpeg.

If the CI distribution's packaged libFLAC CMake package requires an extra dependency such as `Threads`, resolve that dependency in CODEC's CMake/config layer rather than leaking it into source code.

## Resource limits and failure semantics

- `maximum_output_bytes == 0` returns `ErrorCode::invalid_argument` before state selection or encoding.
- D.3 query bounds remain authoritative for result count and aggregate APS1 encoded bytes.
- D.5's output limit is aggregate across all returned FLAC payloads.
- Each encoder invocation receives only the remaining aggregate byte budget.
- Callback refusal caused by the byte limit returns `ErrorCode::resource_exhausted`.
- Malformed APS1 propagates D.3's `ErrorCode::decode` before FLAC encoding.
- Contradictory provenance propagates D.3 archive-integrity errors.
- libFLAC allocation, setup, init, process, finish, or verify failures return `ErrorCode::internal`, because CODEC's current public `ErrorCode` enum has no encode-specific member and D.5 does not broaden that generic API.
- No failure writes to the archive or filesystem.

## Validation and tests

Create `tests/test_audio_flac_export.cpp`.

Required proofs:

1. **D.2 → D.3 → D.5 integration**
   - ingest a PCM16 WAV through the actual D.2 API;
   - open finalized CODA;
   - export through D.5;
   - assert `audio/flac`, one exact APS1 support link, and exact returned state/source/provenance evidence;
   - decode the FLAC with libFLAC in the test and prove exact sample rate, channels, frame count, and signed samples match the D.1 state.

2. **Native FLAC shape**
   - output begins with the native `fLaC` marker;
   - no filesystem encoder path is involved.

3. **Truth gating**
   - an unprovenanced physical `pcm16` record yields no D.5 output;
   - non-`source_bytes`, cross-stream, and mismatched-interval lineage fail through D.3 exactly as D.4 does.

4. **Malformed state**
   - provenance-backed malformed APS1 returns `ErrorCode::decode`.

5. **Bounds**
   - zero D.5 output limit is `invalid_argument`;
   - a limit smaller than one produced FLAC stream is `resource_exhausted`;
   - multiple selected states with an aggregate limit that cannot fit all results return `resource_exhausted` and no partial vector.

6. **Compatibility**
   - D.4 WAV byte identity test remains unchanged and green;
   - existing archive, Audio Profile, Engine, CLI, C ABI, watermark, processing/exporter, and inference capability tests remain green;
   - installed package consumer still configures, links, and runs with the FLAC dependency present.

7. **Toolchain**
   - warnings-as-errors GCC and Clang builds pass;
   - sanitizer build/test passes;
   - `codec-ai-contract` passes.

## Documentation

README should describe D.5 as a provenance-verified, lossless external FLAC representation of APS1 S1. CHANGELOG Unreleased should record the bounded API, libFLAC dependency, exact PCM round-trip proof, and preserved non-claims.

Do not describe FLAC bytes as canonical S1 and do not claim cross-libFLAC-version byte determinism.

## Non-claims

D.5 does not add or claim:

- CODA header/envelope/version or record-type changes;
- a new S1 representation or new truth class;
- FLAC ingest;
- production media adapters beyond existing bounded PCM16 WAV ingest;
- FFmpeg or general media conversion;
- resampling, remixing, dither, enhancement, float PCM, or channel-layout semantics;
- archive/filesystem/network persistence of FLAC output;
- CLI or C ABI FLAC export;
- neural runtime/model bundles;
- streaming or offline inference;
- diarization, embeddings, or identity fusion;
- recovery/FEC;
- performance, scale, or deployment guarantees;
- frozen CODA v1;
- Stage D completion.

## Stage D continuation

After D.5, Stage D remains active. The next milestone should select the smallest remaining dependency from production media adapters, identity fusion, compatible neural runtime/model bundles, streaming inference, offline re-inference, and broader audio-specific validation. It should consume the D.3 trusted state boundary rather than create a parallel truth-selection path.