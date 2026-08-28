# Stage D.6 Preservation-First Native FLAC Ingest Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded preservation-first native FLAC ingest that commits exact FLAC S0 before decoding supported PCM16 into the existing APS1 S1 with exact provenance.

**Architecture:** Reuse D.2's capture/commit/failure-isolation transaction pattern, add one private in-memory libFLAC decoder returning `Pcm16State`, then expose a profile-only `ingest_pcm16_flac` wrapper. D.3 remains the downstream trust gate; D.6 never creates a parallel S1 representation.

**Tech Stack:** C++20, CMake 3.20+, existing `PkgConfig::FLAC` libFLAC dependency, `PreparedCapture`, `CodaWriter`, APS1, D.3 verified state query, GitHub Actions GCC/Clang/ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d6-preservation-first-flac-ingest-design.md`

## Global Constraints

- Exact accepted FLAC bytes are committed as S0 before profile decoding.
- APS1 remains the sole PCM16 S1 representation.
- Native FLAC only; reject Ogg-FLAC and arbitrary containers.
- Supported canonicalization input is signed 16-bit PCM only.
- Preserve exact decoded sample rate, channel count, frame order, and interleaved samples.
- Enforce source-byte and decoded-PCM byte limits independently.
- Profile decode/resource failure after S0 commit finalizes a verified S0-only archive and is reported in `profile_error`.
- No resampling, remixing, dither, gain, normalization, floating PCM, FFmpeg, CLI, C ABI, inference, identity, CODA layout, or Stage D completion change.

---

### Task 1: Establish the D.6 RED contract

**Files:**
- Create: `tests/test_audio_flac_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `codec::profiles::audio::query_verified_pcm16_states`, `export_verified_pcm16_flac`, archive APIs, and libFLAC test linkage already present through CODEC.
- Produces test expectations for `Pcm16FlacIngestRequest`, `Pcm16FlacIngestReport`, and `ingest_pcm16_flac(...)`.

- [ ] **Step 1: Add a deterministic test-local native FLAC encoder fixture**

In `tests/test_audio_flac_ingest.cpp`, use libFLAC's stream encoder callbacks over a byte vector. Parameterize bits-per-sample so the fixture can create both 16-bit and unsupported 24-bit native FLAC without filesystem I/O. For 16-bit fixtures, promote signed `std::int16_t` samples directly to `FLAC__int32` and encode interleaved frames.

- [ ] **Step 2: Add the happy-path preservation/trust test**

Construct an `audio/flac` `StreamDescriptor`, create deterministic PCM16 samples, encode native FLAC with the fixture, write those bytes to a temporary source file, and call the wished-for API:

```cpp
codec::profiles::audio::Pcm16FlacIngestRequest request{
    .source_uri = source.string(),
    .archive_path = archive_path,
    .descriptor = descriptor,
    .start_ns = 100,
    .end_ns = 200,
    .maximum_decoded_pcm_bytes = 1024 * 1024,
};
auto report = codec::profiles::audio::ingest_pcm16_flac(request);
```

Assert `state_exact()`, finalized archive verification, exact `source_bytes` payload equality with the input FLAC bytes, and D.3 verified state equality for sample rate/channels/samples and same-stream/same-interval lineage.

- [ ] **Step 3: Add round-trip integration coverage**

From the D.6 archive, call `export_verified_pcm16_flac()` and independently decode the returned FLAC with a test-local libFLAC decoder or compare through D.3 state. Prove decoded PCM equality; explicitly do not assert source/export FLAC byte identity.

- [ ] **Step 4: Add preservation-first profile failure cases**

Cover:

```text
native magic + malformed body -> finalized verified S0-only, profile_error=decode
24-bit native FLAC -> finalized verified S0-only, profile_error=decode
OggS-prefixed input -> finalized verified S0-only, profile_error=decode
```

For every case assert exact S0 payload bytes, no state, no provenance.

- [ ] **Step 5: Add decoded-output resource tests**

Encode a valid compressed 16-bit FLAC whose decoded PCM is larger than a small `maximum_decoded_pcm_bytes`; expect a successful ingest report with exact S0 and `profile_error.code == resource_exhausted`. Set `maximum_decoded_pcm_bytes = 0`; expect outer `invalid_argument` before archive creation.

- [ ] **Step 6: Add request validation cases**

Assert pre-capture outer failure for non-audio descriptor, non-`audio/flac` payload type, inverted interval, invalid chunk, zero source bound, excessive redirects, and pre-existing archive output.

- [ ] **Step 7: Register the test and establish RED**

Add `tests/test_audio_flac_ingest.cpp` to `codec_tests`, commit tests only, open a draft PR, and require CI to reach the new test and fail specifically because `Pcm16FlacIngestRequest` / `ingest_pcm16_flac` are absent. Test-fixture/build failures must be corrected before accepting RED.

---

### Task 2: Add the private bounded native FLAC decoder

**Files:**
- Create: `src/audio/flac_decoder.hpp`
- Create: `src/audio/flac_decoder.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::Pcm16State`, `codec::Result`, libFLAC decoder C API.
- Produces:

```cpp
namespace codec::profiles::audio::detail {
Result<Pcm16State> decode_flac_pcm16(
    std::span<const std::byte> source,
    std::uint64_t maximum_decoded_pcm_bytes);
}
```

- [ ] **Step 1: Define the private decoder header**

Include only CODEC PCM/result definitions plus `<cstdint>` and `<span>`. Keep `<FLAC/...>` headers out of the public include tree.

- [ ] **Step 2: Implement read-only in-memory decoder callbacks**

Maintain:

```cpp
struct DecoderState {
  std::span<const std::byte> source;
  std::size_t offset{};
  std::uint64_t maximum_decoded_pcm_bytes{};
  Pcm16State pcm;
  bool saw_streaminfo{false};
  bool decode_failed{false};
  bool exhausted{false};
};
```

Implement libFLAC read/seek/tell/length/eof callbacks over `source`. Reject impossible offsets and never mutate source bytes.

- [ ] **Step 3: Validate STREAMINFO and decoded frame geometry**

The metadata callback must require non-zero sample rate/channels and exactly 16 bits per sample. Record sample rate/channels once. The frame callback must require 16-bit samples and geometry consistent with STREAMINFO.

Before appending each decoded block, compute:

```text
new_samples = blocksize * channels
new_pcm_bytes = (existing_samples + new_samples) * 2
```

with overflow-safe arithmetic. If the configured byte limit would be exceeded, set `exhausted=true` and abort. Range-check every `FLAC__int32` before conversion to `std::int16_t`.

- [ ] **Step 4: Initialize and run libFLAC with integrity checking**

Before initialization require `source` to begin with `fLaC` and `maximum_decoded_pcm_bytes != 0`. Allocate the decoder with RAII, enable `FLAC__stream_decoder_set_md5_checking(decoder, true)`, call `FLAC__stream_decoder_init_stream`, `FLAC__stream_decoder_process_until_end_of_stream`, and `FLAC__stream_decoder_finish`.

Return `resource_exhausted` when decoded bound/allocation exhaustion is recorded; otherwise return `decode` for malformed, unsupported, callback, process, finish/MD5, or geometry failure.

- [ ] **Step 5: Register the production source and compile**

Add `src/audio/flac_decoder.cpp` to `codec_core`. Run the focused test build; public ingest tests may still fail because Task 3 is absent, but the decoder must compile warning-free under GCC/Clang.

---

### Task 3: Implement preservation-first FLAC ingest

**Files:**
- Modify: `include/codec/profiles/audio_ingest.hpp`
- Create: `src/audio/pcm16_flac_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `detail::decode_flac_pcm16`, existing `PreparedCapture`, `CodaWriter`, `encode_pcm16_state`.
- Produces the exact D.6 public API from the spec.

- [ ] **Step 1: Add public request/report declarations**

Add `Pcm16FlacIngestRequest`, `Pcm16FlacIngestReport::state_exact()`, and `ingest_pcm16_flac(...)` exactly as specified. Do not change D.2 WAV signatures or add root aliases.

- [ ] **Step 2: Implement pre-capture validation**

Mirror D.2 bounds exactly for source URI, archive path, interval, capture chunk (`4096..16 MiB`), redirects (`<=20`), source byte limit, descriptor encoding, and existing output refusal. Additionally require `maximum_decoded_pcm_bytes != 0`, representable by `size_t`, `StreamType::audio`, and payload type `audio/flac`.

- [ ] **Step 3: Capture one owned source snapshot**

Use `detail::PreparedCapture::prepare` with the request's capture/network limits and append callback bytes into one vector using overflow-safe checks against `maximum_source_bytes`. Do not decode during capture and do not read the source again.

- [ ] **Step 4: Commit S0 before decoding**

After capture succeeds, create `CodaWriter`, append the exact descriptor, then append exact `RecordType::source_bytes` over the caller's stream and interval. Construct the report before profile interpretation.

- [ ] **Step 5: Implement S0-only profile failure finalization**

Create a local `finish_source_only(Error)` closure identical in transaction meaning to D.2: finalize writer, store the profile error, and return the successful report.

Call `detail::decode_flac_pcm16(source_bytes, maximum_decoded_pcm_bytes)`. On any decoder error, return through `finish_source_only`.

- [ ] **Step 6: Append APS1 and exact provenance on success**

Encode with `encode_pcm16_state`, append `RecordType::pcm16` on the same stream/interval, then append provenance:

```cpp
ProvenanceProcess{
  .operation = "audio.flac.decode-pcm16",
  .implementation_id = "codec-audio-profile",
  .implementation_version = "1",
  .created_utc_ns = request.end_ns,
};
```

with exactly the S0 source record as input and `TruthClass::state_exact`. Finalize, populate state/provenance, return.

- [ ] **Step 7: Run D.6 and full tests**

Require the new test to pass plus unchanged D.2/D.3/D.4/D.5, archive, CLI, C ABI, watermark, processing/exporter, inference capability, AI contract, and package consumer tests.

---

### Task 4: Documentation, exact verification, and merge

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/superpowers/plans/2026-08-28-stage-d6-preservation-first-flac-ingest.md` only to record factual execution discoveries.
- Modify spec only if runtime/CI proves a factual detail needs correction.

- [ ] **Step 1: Update README current Audio Profile status**

State that native PCM16 FLAC ingest now preserves exact FLAC S0 before decoding to APS1 S1; malformed/native-unsupported/Ogg input remains S0-only with profile error. Keep APS1 canonical and retain explicit non-claims for broader conversion/inference.

- [ ] **Step 2: Update CHANGELOG Unreleased**

Record the profile-only FLAC ingest API, exact S0-first transaction, decoded PCM bound, native 16-bit limitation, and D.3/D.5 round-trip proof.

- [ ] **Step 3: Run final exact-head CI**

Require one exact final head SHA to pass:

```text
GCC configure/build/test/install/package-consumer
Clang configure/build/test/install/package-consumer
ASan/UBSan configure/build/test
```

Record final head SHA, tree SHA, workflow run, changed files, and review-thread state.

- [ ] **Step 4: Review and merge**

Recheck `main` has not drifted, PR head/mergeability/review threads, and exact-head CI. Mark ready, squash-merge with `expected_head_sha`, and verify the published merge commit tree exactly equals the tested feature tree and GitHub signature verification is valid.

- [ ] **Step 5: Post-merge verification and roadmap record**

Require push-triggered `main` CI to pass all GCC/Clang/package-consumer/sanitizer gates. Add a D.6 completion record to issue #10 with base SHA, accepted RED SHA/run, first/final GREEN evidence, tested/published tree, merged main SHA, post-merge run, truth-class effects (`S0` preservation + `S1` exact derivation), CODA layout delta `none`, and preserved non-claims. Stage D remains active.