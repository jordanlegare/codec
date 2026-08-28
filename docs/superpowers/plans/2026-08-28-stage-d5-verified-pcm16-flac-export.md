# Stage D.5 Provenance-Verified PCM16 FLAC Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export only D.3-verified APS1 PCM16 S1 as bounded native in-memory FLAC with exact state/source/provenance evidence and sample-exact decode proof.

**Architecture:** Add libFLAC as a required native dependency, isolate in-memory FLAC encoding behind a private Audio Profile detail boundary, and expose one profile-only trusted wrapper that delegates truth selection to `query_verified_pcm16_states()`. The public result mirrors D.4 evidence retention while FLAC remains an external lossless representation rather than canonical S1.

**Tech Stack:** C++20, CMake 3.20+, libFLAC C API discovered through CMake `FindPkgConfig` / imported `PkgConfig::FLAC`, existing CODEC `StreamExporter`, `CodaArchive`, APS1, GitHub Actions GCC/Clang/ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d5-verified-pcm16-flac-export-design.md`

## Global Constraints

- APS1 remains the sole canonical PCM16 S1 identity.
- Public D.5 truth selection must delegate to `query_verified_pcm16_states()`.
- Native FLAC only; no Ogg FLAC, FFmpeg, shell-out, or filesystem/network output.
- PCM remains 16-bit, sample-exact, same sample rate, same channel count, same interleaved order.
- libFLAC encoder verify mode must be enabled.
- Compression level is fixed at 5; streamable subset is enabled.
- D.5 output byte limit is aggregate and each encoder receives only the remaining budget.
- libFLAC codec failures map to `ErrorCode::internal`; budget refusal maps to `resource_exhausted`.
- No CODA layout, generic exporter API, CLI, C ABI, inference, identity, or Stage D completion change.
- Do not claim cross-libFLAC-version byte determinism.

## Execution discovery record

The first RED attempt used `find_package(FLAC CONFIG REQUIRED)` and failed at CMake configure on Ubuntu 24.04 because distro `libflac-dev` 1.4.3 does not ship `FLACConfig.cmake`. That run is not accepted as TDD RED. Dependency discovery was corrected, without production code, to:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
```

A second attempt reached the new test but also exposed test-only warnings; those were fixed before accepting RED. Clean RED is head `06ce819b8f41052fcfbb1c651ffde5d032f40491`, CI #82 / `33165323693`: all three jobs configured with libFLAC 1.4.3, built existing production code, and failed because the D.5 public API was absent.

---

### Task 1: Establish the D.5 RED contract and libFLAC test dependency

**Files:**
- Create: `tests/test_audio_flac_export.cpp`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: existing `codec::profiles::audio::ingest_pcm16_wav`, `Pcm16StateQuery`, D.3 lineage rules, `ExportResult`.
- Produces test expectations for `Pcm16FlacExportLimits`, `VerifiedPcm16FlacExport`, and `export_verified_pcm16_flac(...)`.

- [x] **Step 1: Add libFLAC to CI test prerequisites**

Both GitHub Actions dependency-install commands install `libflac-dev` alongside the existing packages.

- [x] **Step 2: Make libFLAC available to the test target without implementing D.5**

The proven distro-compatible configuration is:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
```

At the RED checkpoint the test binary linked `PkgConfig::FLAC` while `codec_core` still had no FLAC production linkage.

- [x] **Step 3: Write an in-memory libFLAC decoder fixture in the new test file**

The fixture uses `FLAC__stream_decoder_init_stream` over `std::span<const std::byte>` and records sample rate, channels, bits per sample, and exact interleaved signed samples. It rejects non-16-bit decoded values and decoder errors.

- [x] **Step 4: Write the D.2 → D.3 → D.5 integration test**

The test ingests an actual PCM16 WAV through D.2, opens the finalized archive, exports through D.5, verifies `audio/flac`, native `fLaC`, exact D.2 state/source identities, exact APS1 support link, `state_exact` provenance, and independently decoded sample-exact PCM.

- [x] **Step 5: Write truth-gating and failure tests**

Coverage includes:

```text
unprovenanced pcm16 -> success with empty result
non-source provenance input -> archive_corrupt
cross-stream provenance -> archive_corrupt
mismatched source/state interval -> archive_corrupt
provenance-backed malformed APS1 -> decode
```

- [x] **Step 6: Write output-limit tests**

Coverage proves:

```text
maximum_output_bytes = 0 -> invalid_argument
one valid FLAC size - 1 -> resource_exhausted
two verified states with aggregate budget below both outputs -> resource_exhausted
```

- [x] **Step 7: Register the new test file**

`tests/test_audio_flac_export.cpp` is registered in `codec_tests`.

- [x] **Step 8: Commit and run RED**

Clean RED is `06ce819b8f41052fcfbb1c651ffde5d032f40491`, CI #82 / `33165323693`. The accepted failure is the absent `Pcm16FlacExportLimits` / `export_verified_pcm16_flac` API after successful libFLAC discovery and existing production compilation.

---

### Task 2: Add the private bounded libFLAC encoder

**Files:**
- Create: `src/audio/flac_encoder.hpp`
- Create: `src/audio/flac_encoder.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `codec::Pcm16State`, `codec::Result`, libFLAC C stream encoder API.
- Produces:

```cpp
namespace codec::profiles::audio::detail {
Result<std::vector<std::byte>> encode_flac_pcm16(
    const Pcm16State& state, std::uint64_t maximum_output_bytes);
}
```

- [x] **Step 1: Define the private header**

The libFLAC C API remains private; no FLAC header is exposed from CODEC's public include tree.

- [x] **Step 2: Implement bounded callback storage**

The encoder owns output bytes, a logical cursor, the byte limit, and an exhausted flag. Write growth is checked before resize; libFLAC's metadata rewrite seeks can only target already-owned bytes; tell returns the current cursor.

- [x] **Step 3: Implement encoder configuration**

The implementation validates the output limit and complete PCM16 geometry and requires:

```cpp
FLAC__stream_encoder_set_verify(encoder, true)
FLAC__stream_encoder_set_streamable_subset(encoder, true)
FLAC__stream_encoder_set_channels(encoder, state.channels)
FLAC__stream_encoder_set_bits_per_sample(encoder, 16)
FLAC__stream_encoder_set_sample_rate(encoder, state.sample_rate)
FLAC__stream_encoder_set_compression_level(encoder, 5)
FLAC__stream_encoder_set_total_samples_estimate(
    encoder, state.samples.size() / state.channels)
```

- [x] **Step 4: Process exact interleaved PCM**

Every signed 16-bit sample is promoted directly to `FLAC__int32` with no scaling. Interleaved frames are processed in bounded chunks, then `FLAC__stream_encoder_finish` completes and verifies the stream. Callback budget refusal maps to `resource_exhausted`; other libFLAC failures map to `internal`.

- [x] **Step 5: Link the production library to FLAC**

Production now links:

```cmake
target_link_libraries(codec_core PUBLIC
  OpenSSL::Crypto CURL::libcurl PkgConfig::FLAC)
```

- [x] **Step 6: Run focused/full tests**

First full GREEN is implementation/package head `8182d1d65b23136827d9e79a0cfa2c14807deac3`, CI #89 / `33165550749`: GCC and Clang build/test/install plus sanitizer build/test all passed.

---

### Task 3: Implement the profile-only trusted FLAC export wrapper

**Files:**
- Create: `include/codec/profiles/audio_flac_export.hpp`
- Create: `src/audio/pcm16_flac_export.cpp`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `detail::encode_flac_pcm16`, D.3 `query_verified_pcm16_states`, C.5 `StreamExporter`/`invoke_exporter`.
- Produces the exact public API from the D.5 spec.

- [x] **Step 1: Add the profile-only public types/function**

`Pcm16FlacExportLimits`, `VerifiedPcm16FlacExport`, and `export_verified_pcm16_flac(...)` are exposed only from the Audio Profile; no root alias is added.

- [x] **Step 2: Implement private `Pcm16FlacExporter`**

The private exporter accepts exactly one `pcm16` physical record, decodes APS1, invokes the private bounded libFLAC encoder, and emits `audio/flac` through the generic exporter contract.

- [x] **Step 3: Implement aggregate bounded trusted wrapper**

The public wrapper rejects a zero output limit, delegates selection to `query_verified_pcm16_states()`, reads only each exact APS1 subject, gives every exporter invocation only the remaining aggregate budget, and retains exact D.3 state/source/provenance evidence. Any later failure returns an error rather than a partial result vector.

- [x] **Step 4: Register production source**

`src/audio/pcm16_flac_export.cpp` and `src/audio/flac_encoder.cpp` are registered in `codec_core`.

- [x] **Step 5: Run the D.5 tests and full suite**

CI #89 established full GREEN including the independent FLAC decode round trip and unchanged existing suites.

---

### Task 4: Complete package propagation, documentation, and exact verification

**Files:**
- Modify: `cmake/codec-config.cmake.in`
- Create: `tests/package_consumer/CMakeLists.txt`
- Create: `tests/package_consumer/main.cpp`
- Modify: `.github/workflows/ci.yml`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/superpowers/specs/2026-08-28-stage-d5-verified-pcm16-flac-export-design.md` for the discovered pkg-config boundary.
- Modify: this plan only for factual execution corrections, never to erase RED/GREEN history.

**Interfaces:**
- Consumes: completed D.5 implementation and libFLAC dependency.
- Produces truthful installed-package discovery and current-status documentation.

- [x] **Step 1: Propagate FLAC through installed package config**

Before target import, CODEC recreates the transitive imported target:

```cmake
find_dependency(PkgConfig)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
```

This matches the build-tree dependency instead of assuming a distro `FLACConfig.cmake`.

- [x] **Step 2: Update README**

README documents D.3 as the truth gate, native in-memory `audio/flac`, sample-exact independent decode proof, exact evidence retention, APS1 as canonical S1, no cross-libFLAC-version byte determinism, and preserved FLAC-ingest/FFmpeg/resampling/inference non-claims.

- [x] **Step 3: Update CHANGELOG Unreleased**

CHANGELOG records the bounded FLAC exporter, libFLAC verification/dependency, exact PCM round trip, aggregate bounds, and non-claims.

- [x] **Step 4: Verify package install consumer**

A minimal external project under `tests/package_consumer` calls `find_package(codec 0.1 CONFIG REQUIRED)`, includes the public Audio Profile, links `codec::codec`, and references the D.5 public API type. The GCC/Clang CI jobs configure/build/run it against the fresh install prefix.

- [ ] **Step 5: Run final exact-head CI**

Require on one exact final head SHA:

```text
GCC configure/build/test/install/package-consumer: success
Clang configure/build/test/install/package-consumer: success
ASan/UBSan configure/build/test: success
codec-ai-contract: success through CTest
```

Record the exact head SHA, tree SHA, and workflow run.

- [ ] **Step 6: Review and merge**

Recheck PR changed files, review threads, mergeability, exact head, and `main` drift. Mark ready and squash-merge with `expected_head_sha`. Verify published `main` tree equals the tested feature tree and GitHub verification is valid.

- [ ] **Step 7: Post-merge verification and roadmap record**

Require push-triggered `main` CI to pass GCC, Clang, package-consumer, and sanitizers. Add a Stage D.5 completion comment to issue #10 containing base SHA, RED SHA/run, final tested SHA/tree/run, merged main SHA, post-merge run, touched truth class S1 (read/export only), CODA layout delta `none`, and preserved non-claims. Stage D remains active.