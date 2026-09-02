# Video Export Verified Snapshot Implementation Plan

**Goal:** Make `codec video export ARCHIVE --all --output-dir DIR` pay for one complete archive verification/metadata scan per command and reuse that verified metadata for every exported video.

**Architecture:** Build one immutable `VerifiedArchiveSnapshot`. Keep `CodaArchive` path-only and preserve its existing object layout. The snapshot-aware MP4 export overload activates a private current-thread RAII metadata scope for the exact archive object, allowing the existing verified video/audio readers to reuse snapshot metadata unchanged. Payloads continue to be re-read and hash-checked from disk. Single-stream export remains on the existing fresh-verification path.

**Spec:** `docs/superpowers/specs/2026-09-02-video-export-snapshot-design.md`

## Constraints

- Preserve CODA format and S0/S1/D semantics exactly.
- Keep `CodaArchive` free of snapshot/cache state.
- Preserve existing public reader semantics outside an explicit snapshot export scope.
- Keep record payload revalidation in `CodaArchive::read_payload()`.
- Keep `video export --all` serial in this change; parallelism is deferred.
- Use deterministic full-scan-count evidence rather than a universal timing/speedup claim.
- Require GCC, Clang, sanitizer, and FFmpeg-disabled CI on the exact final head before merge.

## Task 1 — Establish RED proof

- [x] Add `tests/test_archive_snapshot.cpp` and test-only complete-scan instrumentation.
- [x] Require one verified snapshot to serve record/stream/provenance metadata without further complete scans.
- [x] Add an ABI guard requiring `CodaArchive` to remain the size of its existing `std::filesystem::path` state.
- [x] Observe build failure before the explicit snapshot query/scope API exists.

## Task 2 — Implement the generic snapshot

- [x] Add additive `VerifiedArchiveSnapshot` with verification, records, descriptors, provenance, `query_records()`, and `query_provenance()`.
- [x] Make `CodaArchive::verified_snapshot()` perform one complete scan and reject non-finalized/corrupt archives.
- [x] Keep media payloads out of the snapshot.
- [x] Keep `CodaArchive` path-only; do not attach a snapshot member or persistent cache.

## Task 3 — Reuse the snapshot through existing verified readers

- [x] Add private `VerifiedArchiveSnapshotScope` RAII binding in `src/archive/verified_snapshot_scope.hpp`.
- [x] Validate that the snapshot belongs to the same archive path before activation.
- [x] Make archive metadata methods consult the scope only for the exact bound archive object; otherwise retain fresh-scan behavior.
- [x] Restore prior binding on scope destruction, including nested scopes.
- [x] Leave existing raw-video, encoded-video, PCM16-audio, and encoded-audio reader implementations unchanged.
- [x] Add `export_verified_video_mp4(const CodaArchive&, const VerifiedArchiveSnapshot&, ...)` and delegate to the existing exporter while the scope is active.

## Task 4 — Route CLI `--all`

- [x] Branch on `--all` before the normal `archive->streams()` query.
- [x] Build one `verified_snapshot()` and enumerate `snapshot.streams()`.
- [x] Pass the same snapshot to every per-video MP4 export.
- [x] Leave single-stream `--stream/--output` behavior unchanged.
- [x] Preserve output naming, JSONL output, resource limits, and per-stream failure behavior.

## Task 5 — Verification and documentation

- [x] Align the design document with the ABI-safe scoped implementation.
- [x] Keep deterministic scan-count proof in the unit suite and CLI routing/output proof in integration tests.
- [ ] Add the narrow Unreleased changelog entry after implementation proof.
- [ ] Audit the final diff for unrelated changes, public compatibility, and CODA/truth-class invariants.
- [ ] Update the PR description from its initial TDD-RED state.
- [ ] Verify all required CI jobs on the exact final head.
- [ ] Mark the PR ready for review after all merge gates are satisfied.

## Merge gate

The change is merge-ready only when the exact final head has:

- requested behavior proven by the scan-count and CLI tests;
- GCC and Clang release builds/tests green;
- sanitizer build/tests green;
- FFmpeg-disabled build/tests green;
- `CodaArchive` ABI-layout guard green;
- truthful design/changelog/PR description;
- scoped diff with no CODA format, C ABI, S0/S1/D, or single-stream behavior change;
- GitHub mergeability true.

Do not merge a draft or a head that moved after verification.
