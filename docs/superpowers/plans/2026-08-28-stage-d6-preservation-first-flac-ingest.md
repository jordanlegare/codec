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

## Execution record

- Accepted RED: head `a470e9beb4252cec1f9fb2f59eb091beb1643ba9`, CI #98 / `33168843144`. libFLAC discovery and existing production compilation succeeded; the new test failed specifically because `Pcm16FlacIngestRequest` and `ingest_pcm16_flac` were absent.
- First implementation candidate: head `b76122f5c1ca78fcaa56baae6b4a99ed4c44a4bc`, CI #103 / `33169026361`. This is not GREEN: GCC found that `detail::` inside `codec::profiles::audio` resolved to the profile-private detail namespace rather than generic `codec::detail` for descriptor/capture helpers.
- Namespace correction: generic helpers are explicitly qualified as `::codec::detail`; the profile-private FLAC decoder remains `codec::profiles::audio::detail::decode_flac_pcm16`.
- First full GREEN: head `9f2d69b2d312286666aefa77df2bf66ade6eee55`, CI #104 / `33169097456`. GCC and Clang build/test/install/package-consumer and sanitizer build/test all succeeded.
- Post-GREEN plan audit found missing explicit tests for several already-implemented pre-capture validation branches. `tests/test_audio_flac_ingest_validation.cpp` was added to cover non-audio descriptor, inverted interval, invalid capture chunk, zero source bound, and excessive redirects before the final exact-head CI.

---

### Task 1: Establish the D.6 RED contract

**Files:**
- Create: `tests/test_audio_flac_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `codec::profiles::audio::query_verified_pcm16_states`, `export_verified_pcm16_flac`, archive APIs, and libFLAC test linkage already present through CODEC.
- Produces test expectations for `Pcm16FlacIngestRequest`, `Pcm16FlacIngestReport`, and `ingest_pcm16_flac(...)`.

- [x] **Step 1: Add a deterministic test-local native FLAC encoder fixture**

In `tests/test_audio_flac_ingest.cpp`, use libFLAC's stream encoder support to produce deterministic native test fixtures. Parameterize bits-per-sample so the fixture can create both 16-bit and unsupported 24-bit native FLAC. For 16-bit fixtures, promote signed values directly to `FLAC__int32` and encode interleaved frames.

- [x] **Step 2: Add the happy-path preservation/trust test**

Construct an `audio/flac` `StreamDescriptor`, create deterministic PCM16 samples, encode native FLAC with the fixture, write those bytes to a temporary source file, and call:

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

The test asserts `state_exact()`, finalized archive verification, exact S0 payload equality, and D.3 verified state equality for sample rate/channels/samples and lineage.

- [x] **Step 3: Add round-trip integration coverage**

The D.6 archive is exported through `export_verified_pcm16_flac()` and independently decoded with libFLAC to prove exact PCM equality. No source/export FLAC byte identity is asserted.

- [x] **Step 4: Add preservation-first profile failure cases**

Coverage includes:

```text
native magic + malformed body -> finalized verified S0-only, profile_error=decode
24-bit native FLAC -> finalized verified S0-only, profile_error=decode
OggS-prefixed input -> finalized verified S0-only, profile_error=decode
```

Each case asserts exact preserved S0 and no state/provenance.

- [x] **Step 5: Add decoded-output resource tests**

A valid compressed 16-bit FLAC whose decoded PCM exceeds `maximum_decoded_pcm_bytes` yields a successful S0-only ingest report with `resource_exhausted`; zero decoded bound is rejected before archive creation.

- [x] **Step 6: Add request validation cases**

Coverage proves pre-capture outer failure for non-audio descriptor, non-`audio/flac` payload type, inverted interval, invalid chunk, zero source bound, excessive redirects, zero decoded bound, and pre-existing archive output.

- [x] **Step 7: Register the test and establish RED**

Accepted RED is head `a470e9beb4252cec1f9fb2f59eb091beb1643ba9`, CI #98 / `33168843144`.

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

- [x] **Step 1: Define the private decoder header**

The libFLAC C API remains private; no FLAC decoder header is exposed in CODEC's public include tree.

- [x] **Step 2: Implement read-only in-memory decoder callbacks**

The decoder owns a source span/cursor, decoded PCM, independent decoded-byte limit, and failure/exhaustion state. Read/seek/tell/length/eof callbacks operate only on the owned source snapshot.

- [x] **Step 3: Validate STREAMINFO and decoded frame geometry**

STREAMINFO requires non-zero sample rate/channels and exactly 16 bits per sample. Frame geometry must remain consistent. Decoded growth is checked with overflow-safe exact two-bytes-per-sample accounting, and each `FLAC__int32` is range-checked before conversion to `std::int16_t`.

- [x] **Step 4: Initialize and run libFLAC with integrity checking**

The implementation requires native `fLaC`, enables `FLAC__stream_decoder_set_md5_checking`, processes metadata and stream data, and finishes the decoder. Decoded bound/allocation exhaustion maps to `resource_exhausted`; malformed/unsupported/integrity/geometry failure maps to `decode`.

- [x] **Step 5: Register the production source and compile**

`src/audio/flac_decoder.cpp` is registered in `codec_core` and compiled warning-free on the first full GREEN head after the namespace correction.

---

### Task 3: Implement preservation-first FLAC ingest

**Files:**
- Modify: `include/codec/profiles/audio_ingest.hpp`
- Create: `src/audio/pcm16_flac_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `detail::decode_flac_pcm16`, existing `PreparedCapture`, `CodaWriter`, `encode_pcm16_state`.
- Produces the exact D.6 public API from the spec.

- [x] **Step 1: Add public request/report declarations**

`Pcm16FlacIngestRequest`, `Pcm16FlacIngestReport::state_exact()`, and `ingest_pcm16_flac(...)` are profile-only additions. D.2 WAV signatures remain unchanged and no root alias is added.

- [x] **Step 2: Implement pre-capture validation**

D.2 capture bounds are preserved and D.6 additionally validates the decoded PCM bound plus exact `StreamType::audio` / `audio/flac` descriptor semantics before capture.

- [x] **Step 3: Capture one owned source snapshot**

D.6 uses `::codec::detail::PreparedCapture` with the request's source/network bounds and builds one owned snapshot without decoding during capture or re-reading the source.

- [x] **Step 4: Commit S0 before decoding**

After capture, D.6 appends the descriptor and exact `source_bytes` record before invoking the private FLAC decoder.

- [x] **Step 5: Implement S0-only profile failure finalization**

Any post-S0 decoder/profile error finalizes the archive, stores `profile_error`, and returns the successful preservation report.

- [x] **Step 6: Append APS1 and exact provenance on success**

Successful decoded PCM is encoded with existing APS1 and appended as `pcm16`; exact provenance uses:

```cpp
ProvenanceProcess{
  .operation = "audio.flac.decode-pcm16",
  .implementation_id = "codec-audio-profile",
  .implementation_version = "1",
  .created_utc_ns = request.end_ns,
};
```

with exactly the committed FLAC S0 record as the direct input and `TruthClass::state_exact`.

- [x] **Step 7: Run D.6 and full tests**

First full GREEN is head `9f2d69b2d312286666aefa77df2bf66ade6eee55`, CI #104 / `33169097456`; final exact-head verification follows after documentation and validation-coverage completion.

---

### Task 4: Documentation, exact verification, and merge

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/superpowers/plans/2026-08-28-stage-d6-preservation-first-flac-ingest.md` only to record factual execution discoveries.
- Modify spec only if runtime/CI proves a factual detail needs correction.

- [x] **Step 1: Update README current Audio Profile status**

README now states that native PCM16 FLAC ingest preserves exact FLAC S0 before decoding to APS1 S1 and that malformed/unsupported/Ogg/over-limit profile input remains S0-only with explicit profile error.

- [x] **Step 2: Update CHANGELOG Unreleased**

CHANGELOG records the profile-only FLAC ingest API, exact S0-first transaction, decoded PCM bound, native 16-bit limitation, and D.3/D.5 round-trip proof.

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
