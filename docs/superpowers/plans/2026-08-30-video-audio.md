# H.1 Verified Audio for Video Ingest and MP4 Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend H.1 so FFmpeg video ingest canonicalizes a present mono/stereo audio track into CODEC's existing `Pcm16State` encoding and verified MP4 export muxes that verified audio instead of emitting muted video.

**Architecture:** Keep one logical camera stream ID. Add a Video-Profile custom record type `0x0102` whose payload is the existing `Pcm16State` encoding, with strict Video-Profile direct/HLS provenance verification. Direct and HLS demux paths decode video and audio in one controlled FFmpeg session; MP4 export queries only verified VFR1 plus verified H.1 PCM and encodes the latter as AAC.

**Tech Stack:** C++20, CODEC/CODA archive/provenance APIs, FFmpeg `libavformat`, `libavcodec`, `libavutil`, `libswscale`, new `libswresample`, existing `Pcm16State` codec, Bash CLI integration tests, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-08-30-video-audio-design.md`

## Global Constraints

- S0 remains exact captured source bytes; no export-time source re-fetch.
- S1 audio reuses `codec::Pcm16State` encoding; no new canonical sample representation.
- H.1 audio record type is `0x0102` on the same `StreamType::video` stream ID.
- New ingest supports at most one selected audio track, mono or stereo only.
- Sample rate and channel count must remain stable across the aggregate state.
- Direct audio provenance inputs are `[primary S0]`.
- HLS audio provenance inputs are `[primary manifest S0] + ordered captured secondary frontier`.
- Do not weaken `query_verified_pcm16_states()` or standalone Audio Profile semantics.
- Old H.1 archives without `0x0102` remain video-only exportable.
- If a new ingest detects source audio and audio canonicalization fails, `profile_error` is set and `state_exact()` is false; already-written video S1 and S0 remain preserved.
- If verified H.1 audio exists but AAC/export conversion is unavailable or fails, MP4 export fails; it must not silently emit muted output.
- Grouped ingest and bulk export retain their existing partial-success behavior.
- Exact-head verification must include GCC, Clang, sanitizers/LSan, FFmpeg-disabled, install, and installed package consumer evidence.

---

### Task 1: Public H.1 audio state and strict verified reader

**Files:**
- Modify: `include/codec/profiles/video.hpp`
- Create: `src/video/audio_state_reader.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/test_video_audio_state_reader.cpp`

**Interfaces:**
- Consumes: existing `codec::Pcm16State`, `encode_pcm16_state()`, `decode_pcm16_state()`, `CodaArchive::query_provenance()`, Video HLS child descriptor contract.
- Produces:

```cpp
inline constexpr RecordTypeCode video_pcm16_audio_state_record_type = 0x0102;

struct VideoAudioQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VerifiedVideoPcm16Audio {
  Pcm16State state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedVideoPcm16Audio>>
query_verified_video_pcm16_audio(
    const CodaArchive& archive, const VideoAudioQuery& query = {});
```

- [ ] **Step 1: Write the failing verified-reader tests**

Add tests that construct finalized CODA archives with one `StreamType::video` descriptor and a `0x0102` subject using `encode_pcm16_state()`.

Required test names and expectations:

```cpp
TEST(video_audio_state_reader_accepts_direct_pcm16_lineage);
TEST(video_audio_state_reader_accepts_hls_pcm16_frontier);
TEST(video_audio_state_reader_rejects_wrong_process_identity);
TEST(video_audio_state_reader_rejects_duplicate_s1_provenance);
TEST(video_audio_state_reader_rejects_invalid_hls_child_descriptor);
TEST(video_audio_state_reader_rejects_malformed_pcm16_payload);
TEST(video_audio_state_reader_rejects_interval_duration_mismatch);
TEST(video_audio_state_reader_rejects_multiple_states_per_stream_v1);
```

Use direct process identity:

```text
operation              = codec.video.pcm16.canonicalize
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.audio-canonicalization.v1
details                 = 0x01
```

Use HLS process identity:

```text
operation              = codec.video.pcm16.canonicalize.hls
implementation_id      = codec.video
implementation_version = 1
details_type            = application/vnd.codec.video.hls-audio-canonicalization.v1
details                 = 0x01
```

For interval consistency, compute the expected duration from `state.frames()` and `sample_rate` and permit at most one nanosecond rounding unit from integer rational conversion; reject zero sample rate, channels outside 1..2, empty samples, and sample vectors not divisible by channel count.

- [ ] **Step 2: Run the focused tests and verify RED**

Run:

```bash
cmake -S . -B build -DCODEC_BUILD_TESTS=ON
cmake --build build -j2
./build/codec_tests --include-prefix video_audio_state_reader_
```

Expected: compile failure because `video_pcm16_audio_state_record_type`, `VideoAudioQuery`, `VerifiedVideoPcm16Audio`, and `query_verified_video_pcm16_audio()` do not exist.

- [ ] **Step 3: Add the public API and minimal strict reader**

In `video.hpp`, include `<codec/audio.hpp>` and add the declarations above.

Implement `audio_state_reader.cpp` following `frame_state_reader.cpp` structure:

```cpp
enum class VideoAudioProvenanceContract { direct, hls };

Result<VideoAudioProvenanceContract> classify_video_audio_process(
    const ProvenanceProcess& process);
```

For each selected `TruthClass::state_exact` provenance subject of type `0x0102`:

- resolve every link exactly by stream/type/sequence/hash;
- require a unique `StreamType::video` descriptor for the subject stream;
- reject self-reference and duplicate subject provenance;
- direct: exactly one same-stream `RecordType::source_bytes` input overlapping the audio state interval;
- HLS: primary same-stream `source_bytes` first plus at least one unique child `source_bytes`, each child on another stream with exactly one opaque descriptor whose `source_id == "codec.video.hls-resource"`;
- enforce at most one state per stream in v1;
- bound aggregate encoded payload bytes before reading;
- decode with `decode_pcm16_state()` and convert decode failures to `archive_corrupt`;
- validate mono/stereo, nonempty complete frames, and interval/sample-duration consistency.

Add `src/video/audio_state_reader.cpp` and `tests/test_video_audio_state_reader.cpp` to CMake target lists.

- [ ] **Step 4: Run focused tests and verify GREEN**

Run the same focused command. Expected: all `video_audio_state_reader_` tests pass.

- [ ] **Step 5: Run existing audio and video reader regressions**

Run:

```bash
./build/codec_tests --include-prefix audio_state_reader_
./build/codec_tests --include-prefix video_state_reader_
```

Expected: pass; standalone Audio Profile behavior is unchanged.

- [ ] **Step 6: Commit**

```bash
git add include/codec/profiles/video.hpp src/video/audio_state_reader.cpp \
  tests/test_video_audio_state_reader.cpp CMakeLists.txt
git commit -m "Add verified H.1 PCM audio state reader"
```

---

### Task 2: Direct-media FFmpeg audio decode to existing Pcm16State

**Files:**
- Modify: `include/codec/profiles/video.hpp`
- Modify: `src/video/ffmpeg_ingest.cpp`
- Modify: `CMakeLists.txt`
- Create or modify: `tests/test_video_ffmpeg_audio.cpp`
- Modify: `tests/test_video_ffmpeg_ingest.cpp` only for report compatibility if required

**Interfaces:**
- Consumes: Task 1 `video_pcm16_audio_state_record_type`; existing `encode_pcm16_state()`.
- Produces request/report additions:

```cpp
std::uint64_t FfmpegVideoIngestRequest::maximum_decoded_audio_bytes{
    1024ULL * 1024ULL * 1024ULL};

bool FfmpegVideoIngestReport::audio_present{};
std::optional<RecordInfo> FfmpegVideoIngestReport::audio_state;
std::optional<RecordInfo> FfmpegVideoIngestReport::audio_provenance;

bool FfmpegVideoIngestReport::audio_state_exact() const noexcept;
```

`state_exact()` must additionally require `audio_state_exact()` when `audio_present == true`.

- [ ] **Step 1: Add an audiovisual direct-media fixture generator and failing ingest tests**

Create tests using FFmpeg libraries in-process, or extend the existing FFmpeg fixture helper, to generate a deterministic short MP4 with:

- video: 2x2 or other existing supported even-size test geometry;
- audio: mono PCM-derived tone encoded in an FFmpeg-supported input codec;
- stable sample rate, e.g. 48000 Hz;
- duration aligned with the video test interval.

Required tests:

```cpp
TEST(video_ffmpeg_audio_direct_ingest_writes_verified_pcm16);
TEST(video_ffmpeg_audio_direct_ingest_reuses_existing_pcm16_encoding);
TEST(video_ffmpeg_audio_direct_no_audio_remains_video_only_exact);
TEST(video_ffmpeg_audio_direct_multichannel_is_profile_error_but_keeps_video_s1);
TEST(video_ffmpeg_audio_direct_audio_limit_is_enforced);
```

The first test opens the produced CODA and calls `query_verified_video_pcm16_audio()`; assert one state, expected sample rate/channels, nonempty samples, and one direct S0 provenance input.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```bash
cmake -S . -B build -DCODEC_BUILD_TESTS=ON
cmake --build build -j2
./build/codec_tests --include-prefix video_ffmpeg_audio_direct_
```

Expected: failures because ingest does not decode audio or populate report fields.

- [ ] **Step 3: Add libswresample dependency**

In `CMakeLists.txt` under the existing FFmpeg option:

```cmake
pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
```

and link `PkgConfig::SWRESAMPLE` with the other FFmpeg libraries. Do not make FFmpeg mandatory when `CODEC_ENABLE_FFMPEG_VIDEO=OFF`.

- [ ] **Step 4: Extend direct demux to select and decode one audio stream**

In `ffmpeg_ingest.cpp`, replace the video-only decoded aggregate with an audiovisual aggregate that still preserves the existing video members and adds an optional canonical audio candidate.

Select video exactly as today. Then inspect audio with `av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, ...)`:

- `AVERROR_STREAM_NOT_FOUND`: `audio_present = false` and continue;
- an audio stream exists but no usable decoder/metadata: record an audio canonicalization error while continuing video packet processing where possible;
- supported audio: create a single-threaded decoder and a `SwrContext` converting to interleaved `AV_SAMPLE_FMT_S16` at the source sample rate and source mono/stereo channel count.

Use FFmpeg channel layout APIs compatible with the repository's supported FFmpeg version. Reject channel counts other than 1 or 2 rather than downmixing.

Route packets to both selected decoders during the same `av_read_frame()` loop. Flush both decoders at EOF.

- [ ] **Step 5: Build one bounded Pcm16State with timing continuity checks**

Accumulate converted samples into one `Pcm16State`:

```cpp
Pcm16State{
  .sample_rate = selected_rate,
  .channels = selected_channels,
  .samples = interleaved_s16_samples,
};
```

Track each decoded audio frame's best-effort timestamp and sample count. Relate timestamps to the selected video's first source timestamp. Trim samples before video origin, preserve positive audio offset, and trim after `request.end_ns`.

Reject:

- unstable sample rate or channels;
- timestamp regression;
- inter-frame discontinuity exceeding one output sample;
- byte accumulation above `maximum_decoded_audio_bytes`;
- unrelatable timestamps.

Do not fail the video decoder merely because audio canonicalization has failed; retain the first audio error and stop accumulating audio while continuing video decode where demux remains healthy.

- [ ] **Step 6: Append the H.1 audio state and direct provenance**

After video states are appended, if source audio was detected and canonicalization succeeded, encode with `encode_pcm16_state()` and append:

```cpp
writer.append_raw(video_pcm16_audio_state_record_type,
                  request.descriptor.id,
                  audio_start_ns,
                  audio_end_ns,
                  encoded_pcm);
```

Append `TruthClass::state_exact` provenance with one input `*source` and the exact direct audio process identity from Task 1.

If audio was detected but canonicalization failed, finalize with S0 plus any successfully written video S1, populate `profile_error`, and leave audio state/provenance absent.

- [ ] **Step 7: Run focused direct tests and existing video regressions**

Run:

```bash
./build/codec_tests --include-prefix video_ffmpeg_audio_direct_
./build/codec_tests --include-prefix video_ffmpeg_ingest_
./build/codec_tests --include-prefix video_ffmpeg_layouts_
```

Expected: all pass.

- [ ] **Step 8: Commit**

```bash
git add CMakeLists.txt include/codec/profiles/video.hpp \
  src/video/ffmpeg_ingest.cpp tests/test_video_ffmpeg_audio.cpp \
  tests/test_video_ffmpeg_ingest.cpp
git commit -m "Canonicalize direct video audio as PCM16"
```

---

### Task 3: HLS audio decode and preservation-frontier provenance

**Files:**
- Modify: `src/video/ffmpeg_ingest_hls.cpp`
- Modify: `tests/test_video_hls_ingest.cpp`
- Modify: `tests/test_video_hls_failures.cpp`
- Reuse: `tests/test_video_ffmpeg_audio.cpp` fixture helper if shared

**Interfaces:**
- Consumes: Task 2 audiovisual decode/canonicalization invariants and Task 1 HLS audio provenance reader.
- Produces: one optional H.1 PCM state for HLS using final ordered `HlsCaptureSession::resources` source frontier.

- [ ] **Step 1: Add failing audiovisual HLS tests**

Build a local deterministic HLS fixture using the same controlled child-resource callback tests already used by H.1. Required tests:

```cpp
TEST(video_hls_audio_ingest_writes_verified_pcm16);
TEST(video_hls_audio_provenance_uses_primary_plus_ordered_frontier);
TEST(video_hls_audio_uses_same_capture_session_not_second_fetch);
TEST(video_hls_audio_failure_keeps_video_s1_and_s0);
```

Assert the verified H.1 audio reader returns one state whose `source_records.front()` is the primary manifest and whose remaining records exactly match the captured child-resource order.

- [ ] **Step 2: Run HLS audio tests and verify RED**

Run:

```bash
./build/codec_tests --include-prefix video_hls_audio_
```

Expected: no H.1 audio state exists.

- [ ] **Step 3: Extend the HLS FFmpeg session to decode video and selected audio together**

Keep the existing `HlsCaptureSession`, `io_open` capture callback, same-origin policy, memory-only manifest replay, and child resource limits unchanged.

After `avformat_find_stream_info()`, configure video as today and configure optional mono/stereo audio using the same PCM16 conversion rules as Task 2. Route packets to both decoders in the same read loop; do not open a second AVFormatContext or network traversal for audio.

Retain the first audio canonicalization error while continuing video where possible.

- [ ] **Step 4: Append HLS audio state with final ordered frontier**

After successful demux/decode, append the aggregate PCM state using the computed audio interval. Its provenance inputs are:

```cpp
std::vector<RecordInfo> hls_audio_inputs;
hls_audio_inputs.push_back(*source);
for (const auto& resource : session.resources)
  hls_audio_inputs.push_back(resource->source_record);
```

Require at least one child when using the HLS audio process contract. Use the exact HLS audio process identity from Task 1.

- [ ] **Step 5: Re-run HLS security and lifecycle regressions**

Run:

```bash
./build/codec_tests --include-prefix video_hls_audio_
./build/codec_tests --include-prefix video_hls_ingest_
./build/codec_tests --include-prefix video_hls_failures_
./build/codec_tests --include-prefix video_hls_policy_
```

Expected: pass, including cross-origin/private/encrypted denial and existing callback failure/lifecycle tests.

- [ ] **Step 6: Commit**

```bash
git add src/video/ffmpeg_ingest_hls.cpp tests/test_video_hls_ingest.cpp \
  tests/test_video_hls_failures.cpp tests/test_video_ffmpeg_audio.cpp
git commit -m "Canonicalize HLS audio with captured frontier"
```

---

### Task 4: Verified PCM16 to AAC and audiovisual MP4 mux

**Files:**
- Modify: `include/codec/profiles/video_export.hpp`
- Modify: `src/video/ffmpeg_export.cpp`
- Modify: `tests/test_video_export.cpp`
- Modify: `tests/test_video_ffmpeg_layouts.cpp` only if shared fixture coverage requires it
- Modify: `tests/package_consumer/video_ffmpeg.cpp`

**Interfaces:**
- Consumes: `query_verified_raw_video_frames()` and Task 1 `query_verified_video_pcm16_audio()`.
- Produces additive evidence in `VerifiedVideoMp4Export`:

```cpp
std::optional<RecordInfo> audio_state_record;
std::optional<StreamProvenance> audio_provenance;
```

- [ ] **Step 1: Write failing audiovisual export tests**

Required tests:

```cpp
TEST(video_export_muxes_verified_pcm16_as_aac);
TEST(video_export_audio_track_decodes_nonempty);
TEST(video_export_preserves_positive_audio_start_offset);
TEST(video_export_old_archive_without_h1_audio_stays_video_only);
TEST(video_export_invalid_verified_audio_fails_instead_of_muting);
```

For the first test, inspect the generated MP4 with libavformat and assert exactly one video stream and one audio stream. Decode at least one audio frame or otherwise verify nonzero audio packets. For positive offset, create verified video starting at T and H.1 PCM state starting later; assert first audio packet PTS maps to the expected positive offset within one AAC frame tolerance.

- [ ] **Step 2: Run focused export tests and verify RED**

Run:

```bash
./build/codec_tests --include-prefix video_export_
```

Expected: audiovisual assertions fail because exporter creates only one video stream.

- [ ] **Step 3: Query strict H.1 audio alongside video**

Inside `export_verified_video_mp4()`, after resolving verified video frames for the selected stream, call:

```cpp
query_verified_video_pcm16_audio(
    archive,
    VideoAudioQuery{
      .stream = query.stream,
      .time = query.time,
      .maximum_results = 1,
      .maximum_encoded_bytes = query.maximum_encoded_bytes,
    });
```

If zero results, retain current video-only behavior. If one result, pass it to the exporter. Any reader corruption/error propagates; do not ignore it.

- [ ] **Step 4: Extend FfmpegMp4Exporter to create AAC track when PCM exists**

Refactor the exporter input so it receives verified video extracted records plus optional verified PCM state. Keep the current MPEG-4 Part 2/YUV420P video path unchanged.

When audio exists:

- find `AV_CODEC_ID_AAC`; absent => `model_incompatible`;
- create a second `AVStream` and audio `AVCodecContext`;
- choose an encoder-supported sample format and sample rate, preferring the verified PCM sample rate when supported;
- preserve mono/stereo channel count;
- use `SwrContext` to convert interleaved S16 to encoder format;
- use an `AVAudioFifo` or equivalent deterministic frame staging so fixed-size AAC frames are produced without dropping tail samples;
- set audio PTS from sample counts plus the verified positive start offset relative to the first video record start;
- interleave audio/video packets through the same `av_interleaved_write_frame()` muxer;
- include audio bytes under the existing `maximum_output_bytes` bound;
- flush video encoder, audio encoder, then trailer.

If audio setup or encoding fails after verified audio is known present, return the error; do not retry video-only.

- [ ] **Step 5: Populate audio evidence in VerifiedVideoMp4Export**

On audiovisual success:

```cpp
.audio_state_record = verified_audio->state_record,
.audio_provenance = verified_audio->provenance,
```

On video-only success, both remain `std::nullopt`.

- [ ] **Step 6: Run export and package-consumer tests**

Run:

```bash
./build/codec_tests --include-prefix video_export_
./build/codec_tests --include-prefix video_ffmpeg_layouts_
ctest --test-dir build --output-on-failure
```

Expected: all pass.

- [ ] **Step 7: Commit**

```bash
git add include/codec/profiles/video_export.hpp src/video/ffmpeg_export.cpp \
  tests/test_video_export.cpp tests/test_video_ffmpeg_layouts.cpp \
  tests/package_consumer/video_ffmpeg.cpp
git commit -m "Mux verified H.1 audio into MP4 export"
```

---

### Task 5: CLI limits, reporting, grouped ingest, and bulk export integration

**Files:**
- Modify: `src/cli/main.cpp`
- Modify: `src/cli/video_multi_ingest.hpp`
- Modify: `tests/video_cli_integration.sh`
- Modify: `tests/video_cli_concurrency.sh`
- Modify: `tests/video_cli_export_all.sh`
- Modify: `tests/cli_integration.sh` only if registration changes are required

**Interfaces:**
- Consumes Task 2 report fields and Task 4 export evidence.
- Produces CLI option:

```text
--maximum-decoded-audio-bytes N
```

and additive ingest/export JSON fields:

```json
{"audio_present":true,"audio_state_exact":true}
{"audio":true}
```

- [ ] **Step 1: Write failing CLI integration assertions**

Extend CLI fixtures to include deterministic audiovisual media and assert:

```bash
codec video ingest ... --maximum-decoded-audio-bytes N
codec video export ARCHIVE --stream UUID --output FILE
codec video export ARCHIVE --all --output-dir DIR
```

Required behavior:

- audiovisual ingest JSON says `audio_present:true` and `audio_state_exact:true`;
- no-audio ingest says both false appropriately without becoming an error;
- single export JSON includes `audio:true` when AAC is muxed;
- bulk export writes audible MP4s for all successful audiovisual cameras;
- grouped concurrent ingest of two audiovisual cameras merges both `0x0102` states/provenance correctly;
- one camera with an audio canonicalization profile error does not discard another exact audiovisual camera;
- overall grouped exit remains 1 when any profile error occurs;
- help lists `--maximum-decoded-audio-bytes` under VIDEO OPTIONS without introducing overlong lines.

- [ ] **Step 2: Run CLI integration and verify RED**

Run:

```bash
bash tests/cli_integration.sh ./build/codec 0.3.0
```

Expected: failures on missing option/report/export audio behavior.

- [ ] **Step 3: Add CLI parsing and JSON fields**

Parse `--maximum-decoded-audio-bytes` for legacy single ingest and each grouped `--video` block into `FfmpegVideoIngestRequest::maximum_decoded_audio_bytes` using the same strict unsigned parser as other limits.

Update help `VIDEO OPTIONS` with the new option.

For ingest JSON add:

```cpp
"audio_present": report.audio_present,
"audio_state_exact": report.audio_state_exact(),
```

For successful export JSON add:

```cpp
"audio": exported->audio_state_record.has_value(),
```

Bulk export uses the same field per successful stream.

- [ ] **Step 4: Ensure staged merge preserves H.1 audio provenance**

Do not special-case payload bytes. The existing merge already re-appends custom non-provenance records and rewrites provenance links. Add assertions proving `0x0102` subject links and its direct/HLS inputs point to the destination archive records after merge.

If the existing generic merge encounters a current-H.1 record type with internal record references, stop and constrain the merge whitelist; `0x0102` itself contains only PCM payload and requires no internal rewrite.

- [ ] **Step 5: Run all CLI integration tests**

Run:

```bash
ctest --test-dir build -R codec-cli-integration --output-on-failure
```

Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add src/cli/main.cpp src/cli/video_multi_ingest.hpp \
  tests/video_cli_integration.sh tests/video_cli_concurrency.sh \
  tests/video_cli_export_all.sh tests/cli_integration.sh
git commit -m "Expose verified video audio through the CLI"
```

---

### Task 6: Full regression, FFmpeg-disabled contract, and exact-head evidence

**Files:**
- Modify only if failures reveal genuine defects in files from Tasks 1-5.
- Update PR body/evidence after verification; no production changes solely for documentation convenience.

**Interfaces:**
- Consumes: all preceding tasks.
- Produces: exact green branch head and reviewable draft PR.

- [ ] **Step 1: Verify FFmpeg-disabled build before PR completion**

Run locally if available, and require CI coverage:

```bash
cmake -S . -B build-noffmpeg \
  -DCODEC_ENABLE_FFMPEG_VIDEO=OFF -DCODEC_BUILD_TESTS=ON
cmake --build build-noffmpeg -j2
ctest --test-dir build-noffmpeg --output-on-failure
cmake --install build-noffmpeg --prefix "$PWD/install-noffmpeg"
```

Expected: compile/tests/install succeed; public H.1 audio API remains available but FFmpeg ingest/export functionality returns `model_incompatible` consistently with existing Video Profile backend behavior.

- [ ] **Step 2: Run full normal build/tests**

```bash
cmake -S . -B build -DCODEC_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/install"
```

Expected: zero failures.

- [ ] **Step 3: Run sanitizer build/tests**

```bash
cmake -S . -B build-san \
  -DCODEC_BUILD_TESTS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san -j2
ctest --test-dir build-san --output-on-failure
```

Expected: zero test failures, no unsuppressed ASan/UBSan/LSan findings.

- [ ] **Step 4: Create or update draft PR from `codex/video-audio` to `main`**

PR body must document:

- exact approved spec and plan paths;
- PCM16 reuse and `0x0102` same-stream contract;
- direct/HLS provenance identities;
- no-audio compatibility and audio-failure non-silencing behavior;
- audiovisual MP4/AAC export;
- grouped/bulk partial success;
- RED commits/runs and final exact-head verification evidence;
- explicit non-goals from the spec.

- [ ] **Step 5: Request code review and address all Critical/Important findings**

Review diff from base `3c8ac8206665e6a8bdb5061ca5e65634caa142f0` to current head. Any fix requires rerunning the affected focused tests and invalidates prior exact-head CI evidence.

- [ ] **Step 6: Verify exact PR head CI**

Require one workflow run whose `head_sha` exactly equals the final PR head, with all of:

```text
build (gcc)       success: configure, build, test, install, package consumer
build (clang)     success: configure, build, test, install, package consumer
sanitizers        success: configure, build, test/LSan
ffmpeg-disabled   success: configure, build, test, install, package consumer
```

Do not claim completion while any exact-head job is pending or failed.

- [ ] **Step 7: Final evidence summary**

Report the exact final commit SHA, PR number/URL, exact CI run ID/number, and the user-facing commands unchanged except for the additive audio limit and audio-bearing output behavior.
