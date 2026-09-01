# H.1 Encoded Video Preservation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace new H.1 per-frame raw-pixel persistence with exact compressed H.264 packet preservation and direct H.264/AAC MP4 remux while retaining legacy VFR1/PCM16 compatibility.

**Architecture:** Add profile-owned EVP1 (`0x0104`) schema/verified reader parallel to EAP1, refactor direct/HLS FFmpeg ingest to capture H.264 packets while streaming decoded frames only for validation, and make MP4 export prefer EVP1 packet remux. Use FFmpeg compressed-domain bitstream filters only when required to recover H.264 extradata or AAC AudioSpecificConfig; never decode/re-encode compatible stored streams during export.

**Tech Stack:** C++20, CODEC/CODA archive/provenance APIs, FFmpeg libavformat/libavcodec/libavutil, CMake/CTest, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-08-31-h1-encoded-video-preservation-design.md`

## Global Constraints

- Base is `main` at `2900742a52c0dd8c6dea1277e767a8592db1d840`.
- Work stays on `codex/encoded-video-state` until exact-head CI is green.
- S0 source/container/HLS-resource bytes remain unchanged and authoritative.
- EVP1 v1 supports H.264 only; no HEVC/AV1 scope expansion.
- New FFmpeg audiovisual ingest writes no VFR1 raw-pixel state.
- Legacy VFR1 and `0x0102` PCM16 readers/export remain supported.
- Compatible export uses H.264/AAC packet remux or bitstream filtering only; no H.264/AAC encoder path.
- Profile semantics remain outside generic CODA core.
- FFmpeg-disabled builds retain schema/readers and explicit backend-unavailable runtime behavior.

---

### Task 1: EVP1 schema and public model

**Files:**
- Modify: `include/codec/profiles/video.hpp`
- Create: `src/video/encoded_video_state.cpp`
- Modify: `CMakeLists.txt`
- Create: `tests/test_video_encoded_video_state.cpp`

**Interfaces:**
- Produces `video_encoded_video_state_record_type = 0x0104`.
- Produces `EncodedVideoCodec`, `EncodedVideoPacketFraming`, `EncodedVideoPacket`, `EncodedVideoState`, `EncodedVideoDecodeLimits`.
- Produces `encode_encoded_video_state()` / `decode_encoded_video_state()`.

- [ ] **Step 1: Write failing schema tests**

Cover exact round-trip with H.264 profile/level/dimensions/SAR, `presentation_lead_ns`, empty and non-empty decoder config, B-frame PTS/DTS ordering, packet flags/payloads, corrupt magic/version/lengths, zero dimensions/frames/packets, unknown codec/framing, unsupported flags, packet/count/config/aggregate limits, overflow, and truncated payloads.

- [ ] **Step 2: Run CI to verify RED**

Commit only the test/CMake registration changes and run the branch through GitHub Actions. Expected failure: missing EVP1 symbols/types/source registration.

- [ ] **Step 3: Implement minimal EVP1 codec**

Use a fixed big-endian header and 32-byte packet entries analogous to EAP1. Validate lengths before allocation. Allow empty decoder config. Require non-negative monotonic DTS offsets and positive durations; PTS may differ from DTS for reorder.

- [ ] **Step 4: Verify GREEN**

Run targeted EVP1 tests plus full CI matrix.

- [ ] **Step 5: Commit**

Commit message: `feat: add encoded video state format`.

### Task 2: Verified EVP1 reader and provenance contract

**Files:**
- Create: `src/video/encoded_video_state_reader.cpp`
- Modify: `include/codec/profiles/video.hpp`
- Modify: `CMakeLists.txt`
- Create: `tests/test_video_encoded_video_state_reader.cpp`

**Interfaces:**
- Produces `VerifiedVideoEncodedVideo`.
- Produces `query_verified_video_encoded_video(const CodaArchive&, const VideoFrameQuery&)`.

- [ ] **Step 1: Write failing reader tests**

Create archives with exact S0 + EVP1 + provenance and assert successful query. Add failures for missing/duplicate/wrong provenance, non-S0 inputs, wrong process contract/details, forged links, corrupt EVP1 payload, stream/time filtering, result and encoded-byte limits.

- [ ] **Step 2: Verify RED in CI**

Expected failure: reader API not implemented.

- [ ] **Step 3: Implement reader**

Mirror the strict EAP1 reader structure. Required process contract:

```text
operation=codec.video.encoded-video.preserve
implementation_id=codec.video
implementation_version=1
details_type=application/vnd.codec.video.encoded-video.v1
details=0x01
```

- [ ] **Step 4: Verify GREEN**

Targeted reader tests then full CI.

- [ ] **Step 5: Commit**

Commit message: `feat: verify encoded video state`.

### Task 3: Direct and HLS ingest preserve packets instead of pixels

**Files:**
- Create: `src/video/encoded_video_capture.cpp`
- Create: `src/video/ffmpeg_video_capture.hpp`
- Modify: `src/video/ffmpeg_ingest.cpp`
- Modify: `src/video/ffmpeg_ingest_hls.cpp`
- Modify: `src/video/ffmpeg_ingest_dispatch.cpp` only if shared packet/framing helpers are required
- Modify: `CMakeLists.txt`
- Modify: `tests/test_video_ffmpeg_ingest.cpp`
- Modify: `tests/test_video_ffmpeg_layouts.cpp`
- Modify: `tests/test_video_hls_ingest.cpp`
- Modify: `tests/test_video_ffmpeg_audio.cpp`
- Create: `tests/test_video_encoded_video_capture.cpp`

**Interfaces:**
- Internal `FfmpegCapturedEncodedVideoPacket` stores absolute nanosecond PTS/DTS/duration/flags/payload.
- Internal finalizer converts captured absolute packet timing + first validated presented frame into one canonical EVP1 state and state interval.

- [ ] **Step 1: Change ingest tests first**

Assert successful new ingest produces exactly one `0x0104` state, zero `0x0101` records, exact packet payloads/config metadata, non-zero validated frame count, and existing EAP1 audio. HLS provenance must include primary manifest plus all accepted child S0 resources supporting the capture. Existing source-only failure behavior remains.

- [ ] **Step 2: Verify RED in CI**

Expected failure: current ingest still writes VFR1 frames.

- [ ] **Step 3: Implement streaming validation and packet capture**

For selected H.264 stream:

```text
av_read_frame packet
  -> copy packet bytes/timing/flags into bounded capture
  -> avcodec_send_packet same packet
  -> avcodec_receive_frame
  -> validate frame dimensions/timeline only
  -> av_frame_unref immediately
```

Remove new ingest calls to `canonicalize_frame()`, `sws_scale()`, `av_image_copy_to_buffer()`, and VFR1 append loops. Keep those helpers for legacy/manual paths where still referenced.

For HLS, aggregate one EVP1 provenance input set from primary S0 and all child S0 resources captured during the EVP1 interval.

- [ ] **Step 4: Verify GREEN**

Run direct/HLS/audio/limits/layout suites plus full CI.

- [ ] **Step 5: Commit**

Commit message: `feat: preserve encoded H264 packets on ingest`.

### Task 4: Direct EVP1 MP4 remux and AAC config recovery

**Files:**
- Modify: `include/codec/profiles/video_export.hpp`
- Modify: `src/video/ffmpeg_export_dispatch.cpp`
- Modify: `tests/test_video_export.cpp`
- Modify: `tests/test_video_audio_review_regressions.cpp`

**Interfaces:**
- Add `VerifiedVideoMp4Export::video_packet_passthrough`.
- Add internal encoded-video MP4 mux path consuming `VerifiedVideoEncodedVideo` and optional EAP1/PCM16 compatibility audio.

- [ ] **Step 1: Write failing export tests**

Add EVP1-only and EVP1+EAP1 fixtures. Assert output contains decodable H.264 and AAC, report sets video/audio packet passthrough true, supporting records include encoded state/provenance, and no raw-frame state is required.

Add regression fixture where EAP1 has empty decoder config and ADTS AAC packets; expected export succeeds rather than returning `archive_corrupt`.

Add incompatible unknown-framing/irrecoverable-config cases returning `model_incompatible`.

- [ ] **Step 2: Verify RED in CI**

Expected failures: exporter currently requires VFR1 and rejects empty AAC decoder config.

- [ ] **Step 3: Implement encoded-video mux**

Create MP4 stream directly from EVP1 H.264 codec parameters. Use stored packet bytes unchanged for length-prefixed H.264 with valid config. For Annex-B H.264 with missing config, run packet copies through `extract_extradata` to obtain global parameter-set metadata while leaving archive bytes untouched; feed MP4 muxer packet copies without video decoding/encoding.

- [ ] **Step 4: Implement AAC ADTS-to-ASC fallback**

If EAP1 decoder config is empty, initialize FFmpeg `aac_adtstoasc`, feed stored packet copies, use filtered output packets plus derived `par_out` extradata for MP4. If recovery is impossible, return `model_incompatible`.

- [ ] **Step 5: Preserve legacy fallback**

If no EVP1 exists, execute the existing VFR1 H.264 encode path unchanged. Fail closed on contradictory verified EVP1+VFR1 state forms for the selected interval.

- [ ] **Step 6: Verify GREEN**

Run export/audio regression tests, inspect output streams with FFmpeg-backed test helpers, then full CI.

- [ ] **Step 7: Commit**

Commit message: `feat: remux encoded H264 and AAC on export`.

### Task 5: CLI/storage assertions, documentation, and compatibility audit

**Files:**
- Modify: `tests/test_cli_integration.sh` or current CLI integration source if encoded video export is covered elsewhere
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`

**Interfaces:**
- CLI syntax remains unchanged.
- `codec video ingest` description changes from raw-frame canonicalization to encoded H.264 preservation with validation.

- [ ] **Step 1: Add/adjust CLI regression proof**

Prove `video export --all` succeeds for new EVP1 archive fixtures and produces no `verified encoded video audio is invalid before MP4 mux` error for recoverable ADTS AAC.

- [ ] **Step 2: Verify RED if a new CLI assertion is introduced**

Confirm failure against pre-documentation implementation state when behavior is not yet wired.

- [ ] **Step 3: Update runtime-truth documentation**

README and changelog state only tested capabilities: exact S0 remains, EVP1 H.264 packets replace new raw-pixel persistence, decoding is validation-only, compatible export remuxes packets, legacy VFR1/PCM16 compatibility remains.

- [ ] **Step 4: Verify package/FFmpeg-disabled compatibility**

Run default GCC/Clang, sanitizer, FFmpeg-disabled, installed-package consumer, and CLI capability checks in CI.

- [ ] **Step 5: Commit**

Commit message: `docs: document encoded video preservation`.

### Task 6: Exact-head review and merge gate

**Files:**
- Review all changed files; no planned source changes unless remediation is required.

- [ ] **Step 1: Audit diff**

Confirm no generic core changes, no unrelated refactor, no new VFR1 writes in FFmpeg audiovisual ingest, no H.264/AAC encoder use on EVP1/EAP1 passthrough, and no credentials/generated artifacts.

- [ ] **Step 2: Run exact-head CI**

Require Release GCC/Clang, CTest, sanitizer, FFmpeg-disabled, package consumer, and repository contract checks on the immutable branch head SHA.

- [ ] **Step 3: Review failures before changing code**

Use systematic debugging for every failing check; add a regression test before remediation.

- [ ] **Step 4: Update worksheet evidence**

Record exact tested SHA/run IDs and only claims proven by that head.

- [ ] **Step 5: Merge only if every worksheet gate is true**

If head moves after review, rerun required CI. Do not auto-merge an unverified SHA.
