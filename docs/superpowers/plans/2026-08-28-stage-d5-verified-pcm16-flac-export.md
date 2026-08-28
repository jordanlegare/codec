# Stage D.5 Provenance-Verified PCM16 FLAC Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export only D.3-verified APS1 PCM16 S1 as bounded native in-memory FLAC with exact state/source/provenance evidence and sample-exact decode proof.

**Architecture:** Add libFLAC as a required native dependency, isolate in-memory FLAC encoding behind a private Audio Profile detail boundary, and expose one profile-only trusted wrapper that delegates truth selection to `query_verified_pcm16_states()`. The public result mirrors D.4 evidence retention while FLAC remains an external lossless representation rather than canonical S1.

**Tech Stack:** C++20, CMake 3.20+, libFLAC C API (`FLAC::FLAC`), existing CODEC `StreamExporter`, `CodaArchive`, APS1, GitHub Actions GCC/Clang/ASan/UBSan.

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

---

### Task 1: Establish the D.5 RED contract and libFLAC test dependency

**Files:**
- Create: `tests/test_audio_flac_export.cpp`
- Modify: `CMakeLists.txt`
- Modify: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: existing `codec::profiles::audio::ingest_pcm16_wav`, `Pcm16StateQuery`, D.3 lineage rules, `ExportResult`.
- Produces test expectations for `Pcm16FlacExportLimits`, `VerifiedPcm16FlacExport`, and `export_verified_pcm16_flac(...)`.

- [ ] **Step 1: Add libFLAC to CI test prerequisites**

Change both GitHub Actions dependency-install commands to install `libflac-dev` alongside the current packages.

- [ ] **Step 2: Make libFLAC available to the test target without implementing D.5**

At configure scope add:

```cmake
find_package(FLAC CONFIG REQUIRED)
```

Link only the test binary at this RED checkpoint:

```cmake
target_link_libraries(codec_tests PRIVATE codec::codec FLAC::FLAC)
```

Do not yet link `codec_core` to FLAC or change installed package metadata.

- [ ] **Step 3: Write an in-memory libFLAC decoder fixture in the new test file**

Use `FLAC__stream_decoder_init_stream` with callbacks over `std::span<const std::byte>`. The fixture returns:

```cpp
struct DecodedFlac {
  std::uint32_t sample_rate{};
  std::uint32_t channels{};
  std::vector<std::int16_t> samples;
};
```

The metadata callback records STREAMINFO sample rate/channels/bits-per-sample and rejects anything other than 16-bit PCM. The write callback interleaves the decoder's per-channel `FLAC__int32` buffers into `std::int16_t` with range checks. Any decoder error causes the helper to fail the test.

- [ ] **Step 4: Write the D.2 → D.3 → D.5 integration test**

Create a PCM16 WAV fixture, ingest it through actual D.2, open the finalized archive, then call:

```cpp
codec::profiles::audio::Pcm16FlacExportLimits limits{
    .maximum_output_bytes = 1024 * 1024,
};
auto exported = codec::profiles::audio::export_verified_pcm16_flac(
    *archive, query, limits);
```

Assert one result, `audio/flac`, exact state/source hashes and sequences matching the D.2 report, one exact APS1 support link, `state_exact` provenance, native leading bytes `fLaC`, and decoded sample rate/channels/samples exactly equal to the D.1 state.

- [ ] **Step 5: Write truth-gating and failure tests**

Add tests equivalent to D.4 for:

```text
unprovenanced pcm16 -> success with empty result
non-source provenance input -> archive_corrupt
cross-stream provenance -> archive_corrupt
mismatched source/state interval -> archive_corrupt
provenance-backed malformed APS1 -> decode
```

- [ ] **Step 6: Write output-limit tests**

Use one valid verified state to obtain its encoded FLAC size under a generous limit, then reopen/query the same archive with:

```text
maximum_output_bytes = 0 -> invalid_argument
maximum_output_bytes = encoded_size - 1 -> resource_exhausted
```

Create a two-state verified archive, encode one state to determine one-stream size, then set aggregate limit to `2 * one_stream_size - 1`; expect `resource_exhausted` and no returned vector.

- [ ] **Step 7: Register the new test file**

Add `tests/test_audio_flac_export.cpp` to `codec_tests`.

- [ ] **Step 8: Commit and run RED**

Commit only the new test plus the minimum libFLAC test-build/CI prerequisite changes. Open a draft PR. Run GitHub Actions and require the compiler to reach the new test and fail because `Pcm16FlacExportLimits` / `export_verified_pcm16_flac` do not exist. If configure fails on FLAC package discovery, fix only discovery before proceeding; RED is not established until the missing D.5 API is the cause.

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

- [ ] **Step 1: Define the private header**

Include only `codec/profiles/audio.hpp` dependencies needed for `Pcm16State`/`Result`, `<cstdint>`, and `<vector>`. Keep libFLAC headers out of the public include tree.

- [ ] **Step 2: Implement bounded callback storage**

Use:

```cpp
struct EncoderOutput {
  std::vector<std::byte> bytes;
  std::uint64_t limit{};
  bool exhausted{false};
};
```

The write callback must check `bytes <= limit - current_size` before any resize/write. If the check fails, set `exhausted = true` and return `FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR`.

The seek callback accepts only offsets representable as `size_t` and within the current vector; otherwise return `FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR`. The tell callback reports the current logical position used by the writer. If libFLAC's callback behavior requires overwriting earlier STREAMINFO bytes, maintain an explicit cursor and resize only under the same limit check.

- [ ] **Step 3: Implement encoder configuration**

Validate `maximum_output_bytes != 0`, `state.sample_rate != 0`, `state.channels != 0`, and complete frames. Allocate the libFLAC encoder with RAII cleanup and require all of:

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

Initialize with `FLAC__stream_encoder_init_stream` and the in-memory callbacks.

- [ ] **Step 4: Process exact interleaved PCM**

Promote every signed 16-bit sample to `FLAC__int32` without scaling:

```cpp
std::vector<FLAC__int32> pcm;
pcm.reserve(state.samples.size());
for (const auto sample : state.samples) {
  pcm.push_back(static_cast<FLAC__int32>(sample));
}
```

Call `FLAC__stream_encoder_process_interleaved` with frame count `samples.size() / channels`, then `FLAC__stream_encoder_finish`.

Return `resource_exhausted` when callback state says the byte limit was hit; otherwise map allocation/setup/init/process/finish/verify failure to `internal` with a stable diagnostic.

- [ ] **Step 5: Link the production library to FLAC**

Move/retain `find_package(FLAC CONFIG REQUIRED)` at configure scope and change:

```cmake
target_link_libraries(codec_core PUBLIC OpenSSL::Crypto CURL::libcurl FLAC::FLAC)
```

Because FLAC is now a public transitive link dependency of the exported static/shared target, the installed config will be updated in Task 4.

- [ ] **Step 6: Run focused compile/tests**

The D.5 tests may still fail for missing public wrapper, but `flac_encoder.cpp` must compile warning-free under the current compiler. Commit the private encoder separately.

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

- [ ] **Step 1: Add the profile-only public types/function**

Create the exact `Pcm16FlacExportLimits`, `VerifiedPcm16FlacExport`, and `export_verified_pcm16_flac(...)` declarations from the spec and include the header from `codec/profiles/audio.hpp`.

- [ ] **Step 2: Implement private `Pcm16FlacExporter`**

Mirror the D.4 private exporter shape:

```cpp
class Pcm16FlacExporter final : public StreamExporter {
 public:
  explicit Pcm16FlacExporter(std::uint64_t maximum_output_bytes)
      : maximum_output_bytes_(maximum_output_bytes) {}

  std::string name() const override { return "audio-pcm16-flac"; }

  Result<ExporterOutput> export_records(
      std::span<const ExtractedRecord> inputs) override;

 private:
  std::uint64_t maximum_output_bytes_;
};
```

Require exactly one `pcm16` record, decode APS1, call `detail::encode_flac_pcm16`, and emit payload type `audio/flac`.

- [ ] **Step 3: Implement aggregate bounded trusted wrapper**

Reject zero output limit first. Call `query_verified_pcm16_states(archive, query)`. For each verified state in returned order:

```cpp
const auto remaining = limits.maximum_output_bytes - total_output_bytes;
auto payload = archive.read_payload(verified.state_record);
const std::array inputs{ExtractedRecord{verified.state_record,
                                        std::move(*payload)}};
Pcm16FlacExporter exporter{remaining};
auto exported = invoke_exporter(
    exporter, inputs,
    ExporterRunLimits{
      .maximum_inputs = 1,
      .maximum_input_bytes = query.maximum_encoded_bytes,
      .maximum_output_bytes = remaining,
      .maximum_payload_type_bytes = 32,
    });
```

Before subtraction, require `total_output_bytes <= maximum_output_bytes`. After success, require `output.payload.size() <= remaining`, then add it. Preserve the exact D.3 `state_record`, `source_record`, and `provenance` in the returned object.

- [ ] **Step 4: Register production source**

Add `src/audio/pcm16_flac_export.cpp` and `src/audio/flac_encoder.cpp` to `codec_core`.

- [ ] **Step 5: Run the D.5 tests and full suite**

Require the new integration/round-trip/truth/bounds tests to pass and existing D.4 WAV byte-identity coverage to remain green. Commit only after the focused test is green.

---

### Task 4: Complete package propagation, documentation, and exact verification

**Files:**
- Modify: `cmake/codec-config.cmake.in`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/superpowers/specs/2026-08-28-stage-d5-verified-pcm16-flac-export-design.md` only if execution revealed a factual implementation detail requiring synchronization.
- Modify: `docs/superpowers/plans/2026-08-28-stage-d5-verified-pcm16-flac-export.md` only for factual execution corrections, never to erase RED/GREEN history.

**Interfaces:**
- Consumes: completed D.5 implementation and libFLAC dependency.
- Produces truthful installed-package discovery and current-status documentation.

- [ ] **Step 1: Propagate FLAC through installed package config**

Add before target import:

```cmake
find_dependency(FLAC CONFIG)
```

If CI proves the distro FLAC config itself requires `Threads`, add `find_dependency(Threads)` before FLAC in CODEC's package config and mirror it at configure scope. Do not add it speculatively.

- [ ] **Step 2: Update README**

Add D.5 to implemented Audio Profile status and document:

- D.3 is still the truth gate;
- output is native in-memory `audio/flac`;
- decoded PCM is sample-exact;
- returned evidence includes exact APS1 state/source/provenance;
- FLAC bytes are an external representation, not canonical S1;
- cross-libFLAC-version byte determinism is not claimed;
- no FLAC ingest/FFmpeg/resampling/inference.

- [ ] **Step 3: Update CHANGELOG Unreleased**

Record the bounded FLAC exporter, libFLAC dependency, exact PCM round-trip validation, aggregate output bound, and non-claims.

- [ ] **Step 4: Verify package install consumer**

GitHub CI's existing install step must succeed. If repository CI does not currently compile an external `find_package(codec)` consumer, add the smallest existing-pattern package-consumer test rather than creating a new packaging subsystem.

- [ ] **Step 5: Run final exact-head CI**

Require on one exact final head SHA:

```text
GCC configure/build/test/install: success
Clang configure/build/test/install: success
ASan/UBSan configure/build/test: success
codec-ai-contract: success through CTest
```

Record the exact head SHA, tree SHA, and workflow run.

- [ ] **Step 6: Review and merge**

Recheck PR changed files, review threads, mergeability, exact head, and `main` drift. Mark ready and squash-merge with `expected_head_sha`. Verify published `main` tree equals the tested feature tree and GitHub verification is valid.

- [ ] **Step 7: Post-merge verification and roadmap record**

Require push-triggered `main` CI to pass GCC, Clang, and sanitizers. Add a Stage D.5 completion comment to issue #10 containing base SHA, RED SHA/run, final tested SHA/tree/run, merged main SHA, post-merge run, touched truth class S1 (read/export only), CODA layout delta `none`, and preserved non-claims. Stage D remains active.