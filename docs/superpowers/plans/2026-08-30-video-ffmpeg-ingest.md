# FFmpeg Video Ingest Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an optional FFmpeg-backed Video Profile ingest bridge that preserves encoded/container bytes as S0 and emits bounded canonical H.1 VFR1 S1 frames with exact provenance.

**Architecture:** Capture remains CODEC-owned and preservation-first. FFmpeg only interprets already captured in-memory bytes through custom AVIO with secondary opens denied; converted frames reuse the existing H.1 schema and verified-reader provenance contract. The backend is compile-time optional and exposes no FFmpeg types in installed headers.

**Tech Stack:** C++20, CMake 3.20+, pkg-config, FFmpeg libavformat/libavcodec/libavutil/libswscale, existing CODA capture/archive/provenance APIs.

**Spec:** `docs/superpowers/specs/2026-08-30-video-ffmpeg-ingest-design.md`

## Global Constraints

- Base is `main` at `226e7d099a3ebaf8fc12b38a8464881ed7608b04`.
- Generic CODA envelopes, `RecordType`, S0/S1/D semantics, and H.1 VPD1/VFR1 bytes do not change.
- FFmpeg support is optional through `CODEC_ENABLE_FFMPEG_VIDEO`, default `OFF`.
- FFmpeg interprets only captured in-memory bytes and must deny secondary resource opens.
- Source bytes are committed before decode; post-capture profile failure yields a finalized source-only archive.
- No CLI, playback, export/transcoding, inference, GPU, quality, performance, or Stage H.2-H.6 claim is added.

---

### Task 1: Public API and RED proof

**Files:**
- Modify: `include/codec/profiles/video.hpp`
- Create: `tests/test_video_ffmpeg_ingest.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `FfmpegVideoIngestRequest`, `FfmpegVideoIngestReport`, `ffmpeg_video_ingest_available()`, `ingest_video_ffmpeg()`.
- Consumes: existing `PixelLayout`, `StreamDescriptor`, `RecordInfo`, `Error`, and archive types.

- [ ] **Step 1: Add failing tests for the wished-for API**

Add tests that compile against:

```cpp
video::FfmpegVideoIngestRequest request{
    .source_uri = source.string(),
    .archive_path = archive_path,
    .descriptor = codec::StreamDescriptor{
        .id = stream,
        .type = codec::StreamType::video,
        .label = "ffmpeg fixture",
        .source_id = "fixture",
        .payload_type = "video/mp4",
    },
    .start_ns = 1'000'000'000,
    .end_ns = 2'000'000'000,
    .output_layout = video::PixelLayout::yuv420p8,
};

auto report = video::ingest_video_ffmpeg(request);
```

The enabled-backend test embeds a deterministic MP4/H.264 fixture and requires byte-exact source extraction plus one verified canonical frame. The disabled-backend test requires `ffmpeg_video_ingest_available() == false` and explicit unavailability without archive creation.

- [ ] **Step 2: Register the test translation unit**

Add `tests/test_video_ffmpeg_ingest.cpp` to `codec_tests`; do not add FFmpeg libraries yet.

- [ ] **Step 3: Push RED commit and run CI**

Expected enabled/default build failure: missing public request/report/functions. Record the exact RED run in `AI_WORKSHEET.md` and issue #10.

---

### Task 2: Optional build/package boundary and unavailable stub

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/codec-config.cmake.in`
- Modify: `include/codec/profiles/video.hpp`
- Create: `src/video/ffmpeg_ingest.cpp`

**Interfaces:**
- Produces all Task 1 API declarations and a backend-unavailable implementation when `CODEC_HAS_FFMPEG_VIDEO` is absent.

- [ ] **Step 1: Add the public structs/functions**

Use exactly:

```cpp
struct FfmpegVideoIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  PixelLayout output_layout{PixelLayout::yuv420p8};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_decoded_bytes{1024ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames{4096};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
};

struct FfmpegVideoIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::vector<RecordInfo> states;
  std::vector<RecordInfo> provenance;
  std::optional<Error> profile_error;
  bool state_exact() const noexcept {
    return !states.empty() && states.size() == provenance.size() &&
           !profile_error.has_value();
  }
};

bool ffmpeg_video_ingest_available() noexcept;
Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request);
```

- [ ] **Step 2: Add CMake option and optional dependencies**

Add `CODEC_ENABLE_FFMPEG_VIDEO` default OFF. When ON, require imported pkg-config targets for `libavformat`, `libavcodec`, `libavutil`, and `libswscale`, define `CODEC_HAS_FFMPEG_VIDEO=1`, and link them so installed static consumers resolve symbols.

- [ ] **Step 3: Make installed package dependency recreation conditional**

Configure `CODEC_PACKAGE_HAS_FFMPEG_VIDEO` into `codec-config.cmake.in`; only when true run the four FFmpeg `pkg_check_modules(... REQUIRED IMPORTED_TARGET ...)` calls.

- [ ] **Step 4: Implement validation and unavailable behavior**

`ffmpeg_video_ingest_available()` returns compile-time availability. Without the macro, a valid request returns `ErrorCode::model_incompatible` with `"FFmpeg video ingest backend is unavailable"`; invalid requests still return `invalid_argument` first.

- [ ] **Step 5: Run the dependency-free tests**

Configure with the default option OFF. Expected: full project builds, the disabled-backend proof passes, all prior tests remain green.

---

### Task 3: Preservation-first FFmpeg demux/decode

**Files:**
- Modify: `src/video/ffmpeg_ingest.cpp`
- Modify: `tests/test_video_ffmpeg_ingest.cpp`

**Interfaces:**
- Consumes: `detail::PreparedCapture`, `CodaWriter`, `encode_raw_video_frame_state()`.
- Produces: source-first archive plus buffered canonical frames or source-only `profile_error` report.

- [ ] **Step 1: Capture and commit S0 before interpretation**

Follow the existing FLAC ingest pattern: bounded capture into `std::vector<std::byte>`, `CodaWriter::create`, append descriptor, append one `RecordType::source_bytes` record, initialize report, and define a `finish_source_only(Error)` helper that finalizes and stores `profile_error`.

- [ ] **Step 2: Implement custom read-only AVIO**

Create a small memory cursor with read and seek callbacks. `AVSEEK_SIZE` returns source size. All arithmetic is bounds checked. Install a format `io_open` callback that always returns `AVERROR(EPERM)` for secondary opens.

- [ ] **Step 3: Open media and select one video stream**

Use `avformat_open_input`, `avformat_find_stream_info`, `av_find_best_stream(AVMEDIA_TYPE_VIDEO)`, `avcodec_find_decoder`, `avcodec_parameters_to_context`, single-thread decoder configuration, and `avcodec_open2`.

- [ ] **Step 4: Decode packets into bounded frame candidates**

Process only the selected video stream. Enforce `maximum_frames`, H.1 dimension limits, and aggregate `maximum_decoded_bytes`. Buffer candidate frame state plus source-relative start timestamp; no S1 record is appended until decode completes successfully.

- [ ] **Step 5: Flush decoder and fail source-only on interpretation errors**

Any malformed packet/decode/format/stream/resource error after S0 commit returns through `finish_source_only`.

---

### Task 4: H.1 canonical conversion, time mapping, and provenance

**Files:**
- Modify: `src/video/ffmpeg_ingest.cpp`
- Modify: `tests/test_video_ffmpeg_ingest.cpp`

**Interfaces:**
- Produces: canonical `RawVideoFrameState` values and exact verified S1 sidecars accepted by `query_verified_raw_video_frames()`.

- [ ] **Step 1: Map requested layout to FFmpeg pixel format**

Use Gray8/RGB24/RGBA/YUV420P. Reject odd YUV420 dimensions before allocation.

- [ ] **Step 2: Convert every decoded frame through libswscale**

Allocate a destination frame/buffer for the target format, run `sws_scale`, and copy rows/planes into exact H.1 canonical bytes with no padding. Build `VideoProfileDescriptor` from decoded geometry, usable SAR/frame-rate metadata, and only exact color metadata mappings.

- [ ] **Step 3: Map frame times monotonically into the caller interval**

Use first valid `best_effort_timestamp` as zero, `av_rescale_q` for relative nanoseconds, average frame rate for missing timestamps/duration, next-frame start for prior frame end, and caller `end_ns` for the final fallback. Reject any non-monotonic/out-of-interval mapping.

- [ ] **Step 4: Append VFR1 + provenance only after successful full decode**

For each buffered frame call `encode_raw_video_frame_state`, append raw type `0x0101`, then append `TruthClass::state_exact` provenance with the one S0 source input and the existing exact H.1 process fields. Hash `av_version_info()` plus output layout into `configuration_hash`.

- [ ] **Step 5: Verify enabled-backend fixture**

With FFmpeg option ON, the embedded MP4/H.264 fixture must produce byte-identical extracted S0 and the expected one-frame YUV420P8 canonical state through the verified reader.

---

### Task 5: CI, packaging, docs, and exact-head verification

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`

**Interfaces:**
- Proves the installed public API and both enabled/unavailable configurations.

- [ ] **Step 1: Enable FFmpeg in normal CI jobs**

Install `libavformat-dev libavcodec-dev libavutil-dev libswscale-dev` and configure normal GCC, Clang, and sanitizer jobs with `-DCODEC_ENABLE_FFMPEG_VIDEO=ON`.

- [ ] **Step 2: Add a dependency-free configuration gate**

Add a configure/build/test invocation with `-DCODEC_ENABLE_FFMPEG_VIDEO=OFF` and run the video FFmpeg tests' unavailable path.

- [ ] **Step 3: Extend installed package consumer**

Include `<codec/profiles/video.hpp>`, construct the FFmpeg ingest request type, call the availability probe, and verify no FFmpeg header is required by consumer code.

- [ ] **Step 4: Update README/CHANGELOG truthfully**

Document optional bounded FFmpeg demux/decode from already captured bytes, exact S0 preservation, H.1 VFR1 output, build option/dependencies, and explicit non-claims.

- [ ] **Step 5: Run final exact-head gates**

Require GCC warnings-as-errors build/full CTest/install consumer, Clang equivalent, ASan/UBSan, dependency-free OFF configuration, CLI/C ABI regressions, and AI contract on one exact head SHA.

- [ ] **Step 6: Audit, record issue #10 evidence, and merge only if exact-head CI is green**

Do not auto-merge if any gate is unknown or the PR head moves after verification.
