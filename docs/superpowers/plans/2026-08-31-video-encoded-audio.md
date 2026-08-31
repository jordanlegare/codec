# H.1 Encoded-Audio Preservation Implementation Plan

> Execute this plan test-first on `codex/encoded-audio-state`, based on
> `bbc286348d0a78474a4569588b91743300483c16`.

**Goal:** Stop new H.1 audiovisual ingest from persisting aggregate PCM16 and
instead store and verify the original AAC packet bytes, with packet-passthrough
MP4 export and legacy `0x0102` compatibility.

**Architecture:** Add a profile-owned, media-library-independent EAP1 schema
and strict verified reader. Rework the existing FFmpeg audio boundary to copy
bounded encoded packets while streaming decoder validation discards frames.
Write `0x0103` with new provenance. Export prefers verified encoded packets,
falls back only to the existing legacy PCM16 path, and never re-fetches S0.

**Technology:** C++20, CODA raw profile records/provenance, FFmpeg
libavformat/libavcodec/libavutil, CMake/CTest, GCC/Clang, ASan/UBSan/LSan.

---

## Task 1: EAP1 schema and bounded codec

**Files:**

- Modify: `include/codec/profiles/video.hpp`
- Create: `src/video/encoded_audio_state.cpp`
- Create: `tests/test_video_encoded_audio_state.cpp`
- Modify: `CMakeLists.txt`

1. Add failing tests for exact AAC configuration/packet round-trip, signed
   timing offsets, unknown enum/flags, reserved fields, truncation, trailing
   bytes, count/size overflow, and caller decode limits.
2. Run the focused test and confirm RED because the EAP1 API is absent.
3. Add `0x0103`, public state/packet/limit structures, and encode/decode APIs.
4. Implement the smallest deterministic big-endian EAP1 codec with checked
   arithmetic and exact payload consumption.
5. Run the focused test and the FFmpeg-disabled build until GREEN.
6. Commit the schema unit.

## Task 2: Strict verified encoded-audio reader

**Files:**

- Modify: `include/codec/profiles/video.hpp`
- Create: `src/video/encoded_audio_state_reader.cpp`
- Create: `tests/test_video_encoded_audio_state_reader.cpp`
- Modify: `CMakeLists.txt`

1. Add RED tests for valid direct and HLS lineage plus wrong process/details,
   unprovenanced/non-S1 state, bad descriptor, invalid/repeated HLS child,
   multiple `0x0103` states, malformed EAP1, interval mismatch, and query
   bounds.
2. Implement `query_verified_video_encoded_audio()` without changing the
   legacy PCM16 reader.
3. Run the two focused EAP1 suites and existing PCM16 strict suites.
4. Commit the verified-reader unit.

## Task 3: Packet-preserving ingest with streaming validation

**Files:**

- Modify: `src/video/ffmpeg_audio_capture.hpp`
- Modify: `src/video/ffmpeg_ingest_dispatch.cpp`
- Modify: `src/video/ffmpeg_ingest_hls.cpp`
- Modify: `tests/test_video_ffmpeg_audio.cpp`
- Modify as required: CLI/grouped/package-consumer audio expectations only

1. Replace ingest expectations with RED proofs that direct and HLS audiovisual
   fixtures write verified `0x0103`, write no `0x0102`, retain exact packet
   bytes/configuration, and remain smaller than PCM16-equivalent bytes.
2. Keep no-audio and audio-error behavior tests unchanged. Retain the existing
   limit option as the new encoded working-set bound and prove limit failure
   preserves S0/video S1.
3. Replace the resampler/sample vector boundary with bounded AAC packet capture
   and decoder-only validation counters/timestamps.
4. Produce EAP1 timing/trim metadata from the existing video-origin interval
   logic; write new direct/HLS process identities and frontiers.
5. Run focused direct/HLS, lifecycle, grouped, and CLI tests.
6. Commit the ingest unit.

## Task 4: MP4 packet passthrough and legacy fallback

**Files:**

- Modify: `include/codec/profiles/video_export.hpp`
- Modify: `src/video/ffmpeg_export_dispatch.cpp`
- Modify: `tests/test_video_export.cpp`
- Modify as required: video CLI integration assertions without changing syntax

1. Add RED coverage for an actual newly ingested archive exporting one
   decodable MP4 audio stream with encoded-packet passthrough evidence.
2. Add RED conflict/corruption tests for simultaneous verified `0x0102` and
   `0x0103`, unsupported/invalid encoded state, and unrepresentable leading
   trim; require explicit failure rather than mute.
3. Implement bounded MP4 audio stream construction and AVPacket payload copy,
   keeping the current PCM16-to-AAC function intact for old archives.
4. Preserve supporting-record/provenance evidence and output-byte bounds.
5. Run export, CLI single/grouped/bulk, and FFmpeg-disabled tests.
6. Commit the export unit.

## Task 5: Documentation, compatibility, and complete verification

**Files:**

- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify: `AI_WORKSHEET.md`
- Modify relevant current H.1 audio documentation references

1. Document `0x0102` as read/export-only compatibility and `0x0103` as the new
   write contract. State workload-specific storage math and avoid universal
   performance claims.
2. Run formatting/diff review and ensure generic core, standalone Audio
   Profile, unrelated CLI commands, and existing HLS/video-frame semantics did
   not change.
3. Run Release GCC warnings-as-errors configure/build/CTest and capabilities.
4. Run Clang warnings-as-errors configure/build/CTest.
5. Run Debug ASan/UBSan/LSan configure/build/CTest.
6. Run FFmpeg-disabled configure/build/CTest/install/package consumer.
7. Run normal install and external package-consumer proof.
8. Update the worksheet with exact commands/results and commit the exact tested
   tree.
9. Push the branch with the GitHub plugin, open a PR, obtain exact-head GitHub
   CI and review evidence, and merge only if every repository merge gate is
   green for the unchanged head SHA.
