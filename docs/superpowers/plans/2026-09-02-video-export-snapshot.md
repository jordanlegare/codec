# Video Export Verified Snapshot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `codec video export ARCHIVE --all --output-dir DIR` perform one complete archive verification/metadata scan per command and reuse that verified state for every exported video.

**Architecture:** Add an additive generic `VerifiedArchiveSnapshot` built by one `CodaArchive::verified_snapshot()` scan. Keep existing public verified reader semantics unchanged; introduce internal snapshot-aware video/audio reader paths and a snapshot MP4-export overload used only by CLI `--all`.

**Tech Stack:** C++20, CODA archive reader, FFmpeg Video Profile, CMake/CTest, Bash CLI integration tests.

**Spec:** `docs/superpowers/specs/2026-09-02-video-export-snapshot-design.md`

## Global Constraints

- Preserve CODA format and S0/S1/D truth semantics exactly.
- Snapshot construction must run the same complete record CRC/SHA-256 verification as existing `CodaArchive::verify()`/`records()`.
- No global cache and no snapshot reuse across commands.
- Single-stream `video export --stream` behavior remains unchanged.
- FFmpeg-disabled builds must compile and pass.
- Performance proof is deterministic full-scan count, not an unqualified wall-clock speed claim.

---

### Task 1: Generic verified archive snapshot

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/archive/archive.cpp`
- Modify: `src/core/internal.hpp`
- Test: `tests/test_archive.cpp`

**Interfaces:**
- Produces: `class VerifiedArchiveSnapshot`
- Produces: `Result<VerifiedArchiveSnapshot> CodaArchive::verified_snapshot() const`
- Produces: `const VerificationReport& VerifiedArchiveSnapshot::verification() const noexcept`
- Produces: `const std::vector<RecordInfo>& VerifiedArchiveSnapshot::records() const noexcept`
- Produces: `const std::vector<StreamDescriptor>& VerifiedArchiveSnapshot::streams() const noexcept`
- Produces: `const std::vector<StreamProvenance>& VerifiedArchiveSnapshot::provenance() const noexcept`
- Produces: `Result<std::vector<RecordInfo>> VerifiedArchiveSnapshot::query_records(const RecordQuery&) const`
- Produces: `Result<std::vector<StreamProvenance>> VerifiedArchiveSnapshot::query_provenance(const ProvenanceQuery&) const`
- Test-only internal helpers: `void reset_archive_scan_count_for_tests() noexcept` and `std::uint64_t archive_scan_count_for_tests() noexcept`

- [ ] **Step 1: Write the failing archive snapshot test**

Add a test that creates a finalized archive with descriptors, S0/S1 state and provenance, resets the test scan counter, calls `verified_snapshot()`, and asserts: scan count is `1`; report is `ok` and finalized; records/descriptors/provenance match fresh public queries; snapshot record/provenance filters preserve archive order.

- [ ] **Step 2: Build the archive test to verify RED**

Run the existing archive test target. Expected: compile failure because `VerifiedArchiveSnapshot`, `verified_snapshot()`, and scan-count helpers do not exist.

- [ ] **Step 3: Implement snapshot construction and query helpers**

Refactor descriptor decoding into a helper taking `const std::vector<RecordInfo>&` so both `streams()` and `verified_snapshot()` share the same logic. Build provenance with the existing `decode_archive_provenance()` helper. `verified_snapshot()` must call `scan_archive(path_)` exactly once and reject `!report.ok` or `!report.finalized` before returning the snapshot.

Implement snapshot `query_records()` with existing `validate_record_query()`/`matches_record_query()` semantics. Implement snapshot `query_provenance()` by filtering the already-decoded provenance and resolving subject/input links against `records_` exactly as the current `CodaArchive::query_provenance()` does.

Increment a test-only atomic counter at the beginning of `scan_archive()`. Define the internal reset/read helpers only for test-enabled builds and wire the compile definition through CMake if required.

- [ ] **Step 4: Run archive tests to verify GREEN**

Run the archive test target and then the full Release CTest suite. Expected: snapshot regression passes and existing archive behavior remains green.

- [ ] **Step 5: Commit**

Commit with message `feat(archive): add verified snapshot view`.

---

### Task 2: Snapshot-aware Video Profile readers and export

**Files:**
- Create: `src/video/verified_snapshot_readers.hpp`
- Modify: `src/video/frame_state_reader.cpp`
- Modify: `src/video/encoded_video_state_reader.cpp`
- Modify: `src/video/audio_state_reader.cpp`
- Modify: `src/video/encoded_audio_state_reader.cpp`
- Modify: `src/video/ffmpeg_export.cpp`
- Modify: `src/video/ffmpeg_export_router.cpp`
- Modify: `include/codec/profiles/video_export.hpp`
- Test: `tests/test_video_export.cpp`
- Test: `tests/test_video_encoded_video_export.cpp`

**Interfaces:**
- Internal readers consume `const CodaArchive&`, `const VerifiedArchiveSnapshot&`, and the existing query type.
- Additive public export overload:
  `Result<VerifiedVideoMp4Export> export_verified_video_mp4(const CodaArchive&, const VerifiedArchiveSnapshot&, const VideoFrameQuery& = {}, VideoMp4ExportLimits = {});`
- Existing `export_verified_video_mp4(const CodaArchive&, const VideoFrameQuery&, VideoMp4ExportLimits)` remains unchanged.

- [ ] **Step 1: Write failing scan-count export tests**

Create two video streams in one finalized archive using existing fixtures/helpers. Reset scan count, construct one snapshot, export stream A and stream B with the new overload, and assert scan count remains `1` after both exports. For an encoded-video fixture, also assert packet-passthrough flags and exported payload type/evidence remain identical to the existing path.

- [ ] **Step 2: Build targeted video tests to verify RED**

Expected: compile failure because the snapshot export overload/internal reader entry points do not exist.

- [ ] **Step 3: Refactor readers behind shared implementations**

For each verified reader, keep its current public function as the fresh-verification wrapper. Move the existing body into an implementation that can obtain verification/records/descriptors/provenance from either the archive or an optional snapshot. Snapshot mode must never call `archive.verify()`, `archive.records()`, `archive.streams()`, `archive.query_records()`, or `archive.query_provenance()`.

The same lineage checks, duplicate checks, payload byte limits, payload decoders, interval validation, HLS child descriptor checks, and corruption errors must run in both modes.

- [ ] **Step 4: Route MP4 export through snapshot-aware readers**

Add the snapshot overload in the router. Encoded-video and encoded/PCM-audio queries use the snapshot readers. For raw-frame fallback, add an internal snapshot-capable raw MP4 export helper so it does not fall back to the fresh public raw-frame reader.

- [ ] **Step 5: Run targeted video tests to verify GREEN**

Run `test_video_export`, `test_video_encoded_video_export`, audio export tests, and then full Release CTest. Expected: exact output/evidence assertions pass and scan count stays at one for multiple snapshot exports.

- [ ] **Step 6: Commit**

Commit with message `perf(video): reuse verified archive snapshot`.

---

### Task 3: Route CLI `video export --all` through one snapshot

**Files:**
- Modify: `src/cli/main.cpp`
- Modify: `tests/video_cli_export_all.sh`

**Interfaces:**
- `video_export_all_command` consumes `const VerifiedArchiveSnapshot&` instead of a separately rescanned descriptor list.
- CLI syntax and JSONL output remain byte-compatible in structure.

- [ ] **Step 1: Extend the CLI regression test**

Keep the existing multi-video `--all` assertions and add coverage that the command exports all videos from a finalized archive through the same output naming/error behavior. The deterministic scan-count proof remains in C++ tests; CLI proof covers routing/output compatibility.

- [ ] **Step 2: Verify the proof fails against the old CLI routing where applicable**

Build/run the CLI integration target after the snapshot API exists but before changing `main.cpp`; targeted scan-count unit tests already demonstrate the old per-stream path performs fresh scans.

- [ ] **Step 3: Implement CLI routing**

After opening the archive and parsing limits, branch on `--all` before calling `archive->streams()`. For `--all`, call `archive->verified_snapshot()` once, pass `snapshot.streams()` into selection logic, and export every stream with the snapshot overload. For single-stream mode, retain the current `archive->streams()` and existing export overload.

- [ ] **Step 4: Run CLI and full tests**

Run the video CLI export-all script, full Release CTest, sanitizer build/tests, and `./build/codec capabilities`. Expected: all green and capability JSON unchanged.

- [ ] **Step 5: Commit**

Commit with message `perf(cli): snapshot video export all`.

---

### Task 4: Proof record, documentation truth, PR and merge gate

**Files:**
- Modify: `AI_WORKSHEET.md`
- Modify: `CHANGELOG.md`
- Optional modify: `README.md` only if it currently documents `video export --all` performance/behavior in a way that benefits from the implementation note.

**Interfaces:**
- No code interface changes beyond Tasks 1-3.

- [ ] **Step 1: Update the worksheet**

Record base SHA `298b542873ccc4a02198c526a93d03e7c0ef6ac4`, branch `codex/video-export-snapshot`, issue #65, change class `performance_or_scale`, touched truth classes `[]`, BEFORE/AFTER behavior, deterministic scan-count proof, exact verification commands/results, and the final head SHA.

- [ ] **Step 2: Update changelog**

Under Unreleased, state narrowly that `video export --all` reuses one command-scoped verified archive snapshot instead of re-verifying/rescanning the complete archive for each video stream. Do not state a percentage or throughput multiplier.

- [ ] **Step 3: Audit the diff**

Confirm no CODA format changes, no truth-class changes, no C ABI changes, no unrelated refactor, and FFmpeg-disabled compatibility.

- [ ] **Step 4: Open the PR**

PR body must link #65, summarize the one-scan snapshot design, list the deterministic scan-count test, and state that parallel export is intentionally deferred.

- [ ] **Step 5: Verify exact-head CI and merge**

Require all applicable CI jobs green on the exact PR head. Recheck PR head SHA and mergeability immediately before merge. Merge only that exact SHA, then verify `main` points at the merge commit and close #65 as completed.
