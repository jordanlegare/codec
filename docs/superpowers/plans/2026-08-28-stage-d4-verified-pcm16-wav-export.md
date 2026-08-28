# Stage D.4 Provenance-Verified PCM16 WAV Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded Audio Profile export path that returns deterministic in-memory PCM16 WAV bytes only for D.3 verified APS1 S1 state and preserves exact state/source/provenance evidence.

**Architecture:** Add one profile-only public export header and one Audio Profile implementation file. The high-level API delegates all truth selection to D.3, uses a private `StreamExporter` through the existing C.5 `invoke_exporter()` boundary, and shares one internal in-memory WAV encoder with `WavPcm16::write()` so file and archive export bytes remain identical.

**Tech Stack:** C++20, existing CODEC `Result`, `CodaArchive`, D.3 verified PCM16 query, C.5 `StreamExporter`, RIFF/WAVE PCM16 implementation, CMake/CTest, GitHub Actions GCC/Clang/ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d4-verified-pcm16-wav-export-design.md`

## Global Constraints

- Start from `main` SHA `48aa5ff43c6364f3dd7f373d9f93cc2a4df35cf8` and its already-green push CI #65.
- Work only on `automation/stage-d4-verified-pcm16-wav-export` until exact-head verification is complete.
- No production code before a failing D.4 test is observed in CI.
- D.3 remains the sole D.4 S1 verification gate; never infer S1 from `RecordType::pcm16` alone.
- Return one independent WAV per verified state; do not concatenate, resample, remix, or infer continuity.
- D.4 performs no archive, filesystem, network, CLI, or C ABI writes.
- Preserve CODA header/envelope/version/record types and all generic `StreamExporter` API signatures.
- Keep neural/GPU capability output unchanged and false.
- No FLAC, new media adapter, identity-fusion, inference, recovery, scale, deployment, frozen CODA v1, or Stage D completion claim.

---

### Task 1: Establish the RED verified-export contract

**Files:**
- Create: `tests/test_audio_export.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes existing public APIs:
  - `codec::profiles::audio::ingest_pcm16_wav()`
  - `codec::profiles::audio::Pcm16StateQuery`
  - `codec::profiles::audio::query_verified_pcm16_states()`
  - `codec::WavPcm16`
  - `codec::CodaWriter` / `codec::CodaArchive`
- Produces test expectations for:
  - `codec::profiles::audio::Pcm16WavExportLimits`
  - `codec::profiles::audio::VerifiedPcm16WavExport`
  - `codec::profiles::audio::export_verified_pcm16_wav()`

- [ ] **Step 1: Add the tests-only D.4 translation unit.**

Create helpers that generate a small PCM16 WAV source, ingest it through actual D.2, and read returned bytes through the existing public WAV reader by writing only the test copy to a temporary file.

The first success test must compile against the wished-for API:

```cpp
const codec::profiles::audio::Pcm16StateQuery query{
    .stream = stream,
    .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
    .maximum_results = 4,
    .maximum_encoded_bytes = 1024 * 1024,
};
const codec::profiles::audio::Pcm16WavExportLimits limits{
    .maximum_output_bytes = 1024 * 1024,
};
auto exported = codec::profiles::audio::export_verified_pcm16_wav(
    *archive, query, limits);
EXPECT_TRUE(exported);
EXPECT_EQ(exported->size(), std::size_t{1});
EXPECT_EQ(exported->front().output.payload_type, std::string{"audio/wav"});
EXPECT_EQ(exported->front().output.supporting_records.size(), std::size_t{1});
EXPECT_EQ(exported->front().state_record.sequence, report->state->sequence);
EXPECT_EQ(exported->front().source_record.sequence, report->source.sequence);
EXPECT_EQ(exported->front().provenance.subject_truth,
          codec::TruthClass::state_exact);
```

Decode the returned WAV and require exact sample rate, channels, and signed sample vector equality.

- [ ] **Step 2: Add byte-identity coverage for existing WAV writing.**

For the same `Pcm16State`, write a `WavPcm16` through the existing `write()` API, read those file bytes in the test, and require exact equality with D.4 `output.payload`.

- [ ] **Step 3: Add truth/failure/resource contract tests.**

Cover all of the following with real archive fixtures:

```text
valid unprovenanced APS1 pcm16 -> successful call with zero exports
state_exact pcm16 with non-source input -> archive_corrupt
state_exact pcm16 with cross-stream source -> archive_corrupt
state_exact pcm16 with mismatched interval -> archive_corrupt
state_exact pcm16 with malformed APS1 -> decode
maximum_output_bytes == 0 -> invalid_argument
valid state whose WAV is larger than maximum_output_bytes -> resource_exhausted
```

Also create two valid state-exact records and prove D.4 preserves provenance order and returns two independent WAV outputs. Then reduce the aggregate limit so it can fit one WAV but not both and require the whole call to fail with `resource_exhausted` rather than return a partial vector.

- [ ] **Step 4: Register `tests/test_audio_export.cpp` in `codec_tests`.**

Only add the test source to the existing CMake test executable. Do not add production sources yet.

- [ ] **Step 5: Open a draft PR and verify RED on the exact tests-only head.**

Expected CI behavior: configure succeeds, existing production sources compile, and GCC/Clang/sanitizer builds fail at the new D.4 test because `Pcm16WavExportLimits`, `VerifiedPcm16WavExport`, and/or `export_verified_pcm16_wav` do not exist.

- [ ] **Step 6: Record the RED head/run in PR #17 and roadmap issue #10.**

Record the exact tests-only SHA, CI run ID/number, and the first missing-symbol compiler error. Do not add production implementation before this evidence exists.

---

### Task 2: Share the existing WAV encoder without changing WAV bytes

**Files:**
- Modify: `src/audio/wav_codec.hpp`
- Modify: `src/audio/wav.cpp`
- Test: `tests/test_audio_export.cpp`

**Interfaces:**
- Produces internal-only helpers:

```cpp
namespace codec::detail {

Result<std::size_t> encoded_wav_pcm16_size(
    std::uint32_t sample_rate,
    std::uint16_t channels,
    std::size_t sample_count);

Result<std::vector<std::byte>> encode_wav_pcm16(
    std::uint32_t sample_rate,
    std::uint16_t channels,
    std::span<const std::int16_t> samples);

}  // namespace codec::detail
```

- [ ] **Step 1: Extract exact-size validation from `WavPcm16::write()`.**

Implement `encoded_wav_pcm16_size()` with the existing validation semantics:

```cpp
if (channels == 0 || sample_rate == 0 || sample_count % channels != 0) {
  return fail<std::size_t>(
      ErrorCode::invalid_argument,
      "PCM16 audio requires a rate, channels, and complete frames");
}
if (sample_count >
    (std::numeric_limits<std::uint32_t>::max() - 36U) /
        sizeof(std::int16_t)) {
  return fail<std::size_t>(ErrorCode::resource_exhausted,
                           "WAV exceeds the RIFF 32-bit size limit");
}
return std::size_t{44} + sample_count * sizeof(std::int16_t);
```

Preserve the exact existing error codes/messages.

- [ ] **Step 2: Move the canonical 44-byte RIFF/WAVE construction into `encode_wav_pcm16()`.**

Use the existing `put16`/`put32` helpers and exact layout:

```text
RIFF + size
WAVE
fmt  + 16-byte PCM format chunk
format=1
channels
sample_rate
byte_rate=sample_rate*channels*2
block_align=channels*2
bits=16
data + sample bytes
```

Return owned bytes; perform no filesystem write.

- [ ] **Step 3: Refactor `WavPcm16::write()` to delegate.**

Replace its inline byte construction with:

```cpp
auto encoded = detail::encode_wav_pcm16(sample_rate, channels, samples);
if (!encoded) return encoded.error();
return detail::write_file(path, *encoded);
```

- [ ] **Step 4: Keep the byte-identity test green.**

The test from Task 1 must prove the refactor did not change one byte of existing canonical WAV output. Existing WAV regression tests must remain green.

---

### Task 3: Implement the D.4 verified WAV export boundary

**Files:**
- Create: `include/codec/profiles/audio_export.hpp`
- Create: `src/audio/pcm16_wav_export.cpp`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_audio_export.cpp`

**Interfaces:**
- Consumes:
  - `query_verified_pcm16_states(const CodaArchive&, const Pcm16StateQuery&)`
  - `CodaArchive::read_payload(const RecordInfo&)`
  - `StreamExporter` / `invoke_exporter()`
  - `detail::encoded_wav_pcm16_size()` / `detail::encode_wav_pcm16()`
- Produces:

```cpp
namespace codec::profiles::audio {

struct Pcm16WavExportLimits {
  std::uint64_t maximum_output_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16WavExport {
  ExportResult output;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16WavExport>> export_verified_pcm16_wav(
    const CodaArchive& archive,
    const Pcm16StateQuery& query = {},
    Pcm16WavExportLimits limits = {});

}  // namespace codec::profiles::audio
```

- [ ] **Step 1: Add the public profile header and facade include.**

`audio_export.hpp` includes `codec/processing.hpp` and
`codec/profiles/audio_state_reader.hpp`. Include it from
`include/codec/profiles/audio.hpp`. Add no root alias.

- [ ] **Step 2: Add the private Audio Profile exporter.**

Inside `src/audio/pcm16_wav_export.cpp` define:

```cpp
class Pcm16WavExporter final : public StreamExporter {
 public:
  std::string name() const override { return "audio-pcm16-wav"; }

  Result<ExporterOutput> export_records(
      std::span<const ExtractedRecord> inputs) override {
    if (inputs.size() != 1 ||
        inputs.front().record.type != RecordType::pcm16) {
      return Error{ErrorCode::invalid_argument,
                   "PCM16 WAV export requires exactly one APS1 pcm16 record",
                   false};
    }
    auto state = decode_pcm16_state(inputs.front().payload);
    if (!state) return state.error();
    auto wav = detail::encode_wav_pcm16(
        state->sample_rate, state->channels, state->samples);
    if (!wav) return wav.error();
    return ExporterOutput{
        .payload_type = "audio/wav",
        .payload = std::move(*wav),
    };
  }
};
```

Keep this class translation-unit private.

- [ ] **Step 3: Validate D.4 limits before querying.**

At the top of `export_verified_pcm16_wav()`:

```cpp
if (limits.maximum_output_bytes == 0) {
  return fail<std::vector<VerifiedPcm16WavExport>>(
      ErrorCode::invalid_argument,
      "PCM16 WAV export output limit must be non-zero");
}
```

Then call `query_verified_pcm16_states(archive, query)` and propagate its error unchanged.

- [ ] **Step 4: Preflight every selected output before allocating WAV bytes.**

For every D.3 state, call:

```cpp
auto size = detail::encoded_wav_pcm16_size(
    verified.state.sample_rate,
    verified.state.channels,
    verified.state.samples.size());
if (!size) return size.error();
if (*size > limits.maximum_output_bytes - total_output_bytes) {
  return fail<std::vector<VerifiedPcm16WavExport>>(
      ErrorCode::resource_exhausted,
      "verified PCM16 WAV exports exceed the configured output limit");
}
total_output_bytes += *size;
```

Guard the subtraction by first ensuring `total_output_bytes <= limits.maximum_output_bytes`. Build a lightweight preflight list or retain the D.3 vector; do not invoke the exporter until every selected state has passed the aggregate size check. This guarantees no partial output allocation when a later item would exceed the aggregate limit.

- [ ] **Step 5: Read each exact APS1 subject and invoke the generic exporter.**

For each preflighted D.3 result:

```cpp
auto payload = archive.read_payload(verified.state_record);
if (!payload) return payload.error();
const std::array inputs{ExtractedRecord{
    .record = verified.state_record,
    .payload = std::move(*payload),
}};
Pcm16WavExporter exporter;
auto result = invoke_exporter(
    exporter,
    inputs,
    ExporterRunLimits{
        .maximum_inputs = 1,
        .maximum_input_bytes = query.maximum_encoded_bytes,
        .maximum_output_bytes = limits.maximum_output_bytes,
        .maximum_payload_type_bytes = 32,
    });
if (!result) return result.error();
```

D.3 already proved `query.maximum_encoded_bytes` non-zero; `invoke_exporter()` independently revalidates state payload size/hash and returns one exact support link.

- [ ] **Step 6: Return the exporter bytes with full D.3 evidence.**

Append:

```cpp
VerifiedPcm16WavExport{
    .output = std::move(*result),
    .state_record = verified.state_record,
    .source_record = verified.source_record,
    .provenance = verified.provenance,
}
```

Preserve D.3 order. Do not read the S0 source payload solely to add another generic exporter support link.

- [ ] **Step 7: Register `src/audio/pcm16_wav_export.cpp` in `codec_core` and run GREEN CI.**

Expected: all D.4 tests pass; GCC/Clang build-test-install and sanitizer build-test pass; existing D.1/D.2/D.3 and generic exporter tests remain green.

---

### Task 4: Synchronize claims, audit, and integrate the exact tested head

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Update: `docs/superpowers/specs/2026-08-28-stage-d4-verified-pcm16-wav-export-design.md` only if implementation reality requires a factual correction
- Update: `docs/superpowers/plans/2026-08-28-stage-d4-verified-pcm16-wav-export.md` only to correct execution facts, never to broaden scope

**Interfaces:**
- Consumes: proven D.4 behavior and CI evidence.
- Produces: truthful current capability status and integration record.

- [ ] **Step 1: Update README implemented Audio Profile status.**

Add one narrow claim: bounded finalized-archive PCM16 WAV export from D.3 verified S1, deterministic in-memory WAV bytes, exact state/source/provenance evidence, and no filesystem/archive write. Keep FLAC, neural runtime/models, streaming/offline inference, and other Stage D objectives explicitly unimplemented.

- [ ] **Step 2: Update CHANGELOG Unreleased.**

Record D.4 and explicitly state no CODA layout, generic exporter API, CLI, C ABI, FLAC, inference, or persistence change.

- [ ] **Step 3: Audit the final PR diff.**

Require only the intended D.4 files: D.4 spec/plan, profile header/facade, Audio implementation, internal WAV helper refactor, dedicated tests/CMake registration, README, and CHANGELOG. Confirm no temporary files/generated artifacts and that README References/AI contract text remains intact.

- [ ] **Step 4: Require exact-head CI.**

On the final branch SHA require one complete successful PR workflow: GCC configure/build/test/install, Clang configure/build/test/install, and sanitizer configure/build/test. Record the exact feature tree SHA.

- [ ] **Step 5: Review merge state.**

Require no unresolved review threads; PR head must still equal the exact green SHA. Re-fetch `main` and ensure it still equals the expected base or incorporate only verified non-conflicting drift before integration.

- [ ] **Step 6: Squash-merge the exact expected head and verify the published tree.**

Use the authorized non-force merge path with `expected_head_sha`. After merge, require the new `main` commit tree to equal the exact PR-tested feature tree byte-for-byte.

- [ ] **Step 7: Require push-triggered main CI and record D.4 completion.**

Verify the workflow on the actual published `main` SHA completes success across GCC, Clang, and sanitizers. Add the D.4 completion record to roadmap issue #10 with base, RED head/run, final tested head/tree/run, merged main SHA, post-merge run, delivered API, truth semantics, preserved boundaries, and the next smallest Stage D dependency. Do not declare Stage D complete.
