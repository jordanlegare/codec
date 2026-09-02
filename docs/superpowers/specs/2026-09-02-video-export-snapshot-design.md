# Video Export Verified Snapshot Design

## Problem

`codec video export ARCHIVE --all --output-dir DIR` exports video streams serially. Before this change, each per-stream verified video/audio reader independently called archive verification and metadata queries, causing the complete CODA archive to be rescanned and its record integrity recomputed repeatedly. For archives with many videos, metadata-verification work therefore scaled with the number of exports instead of being paid once per command.

## Goal

Make `video export --all` reuse one command-scoped verified archive snapshot so archive integrity, record metadata, stream descriptors, and provenance are established once and reused for every video export.

## Non-goals

- No global or persistent archive cache.
- No snapshot state stored in `CodaArchive` and no change to its object layout.
- No weakening, skipping, or deferring record CRC/SHA-256 verification.
- No change to CODA bytes, S0/S1/D semantics, provenance contracts, or single-stream export behavior.
- No parallel export worker pool in this change.
- No unqualified wall-clock speedup claim.

## Architecture

`VerifiedArchiveSnapshot` is an immutable metadata value built by `CodaArchive::verified_snapshot()`. Construction performs one complete `scan_archive()`, requires a valid finalized archive, and derives the same stream-descriptor and provenance projections used by the existing archive readers. The snapshot owns verification metadata, `RecordInfo` values, stream descriptors, and decoded provenance, and exposes `query_records()` and `query_provenance()` filters with the existing query semantics.

The snapshot does not own media payload bytes and does not cache `read_payload()`. Selected payloads continue to be read from the originating `CodaArchive`; `read_payload()` still rechecks the exact record envelope, trailer, and SHA-256 before returning bytes.

`CodaArchive` remains path-only. The snapshot is never attached to the archive object, so existing API callers retain fresh-scan behavior and the public object layout remains unchanged.

## Video profile integration

The existing verified raw-video, encoded-video, PCM16-audio, and encoded-audio readers remain unchanged. A snapshot-aware MP4 export overload accepts both the archive and `const VerifiedArchiveSnapshot&`. For the lifetime of that export call, a private current-thread RAII scope binds the snapshot to that exact `CodaArchive` object. Existing metadata calls made by the readers (`verify()`, `records()`, `streams()`, `query_records()`, and `query_provenance()`) then resolve from the verified snapshot instead of rescanning the file. Leaving the scope restores the previous state.

The binding is internal implementation machinery, is path-validated, supports nested restoration, and is intentionally not a global/persistent cache. Parallel export is deferred; a future parallel implementation must explicitly establish snapshot scope in each worker rather than relying on cross-thread propagation.

`video export --all` constructs one snapshot before enumerating videos, uses `snapshot.streams()` for selection, and passes the same snapshot to every per-stream export. Single-stream CLI export continues to use the existing fresh-verification overload.

## Integrity and mutation semantics

Snapshot construction performs the complete archive CRC/SHA-256/hash-chain scan and requires both `verification.ok` and `verification.finalized`. Provenance links are resolved against the snapshot's verified record vector. Media payloads are not trusted merely because metadata was snapshotted: every selected record is still re-read and hash-checked by `read_payload()`.

A new `video export --all` invocation always creates a new snapshot. There is no timestamp/size invalidation heuristic and no snapshot retained by `CodaArchive` after the command.

## Performance proof

Test-only scan instrumentation counts complete `scan_archive()` invocations. The regression test asserts that snapshot construction increments the count to one, direct snapshot queries do not increase it, payload reads do not count as complete scans, and repeated archive metadata calls inside the explicit snapshot scope leave the count at one. After the scope is destroyed, a normal archive metadata query increments the count again, proving existing fresh-scan behavior is restored.

The CLI integration suite exercises the `video export --all` routing and output behavior. CI wall-clock duration is not used as benchmark evidence.

## Compatibility

The snapshot type and MP4 export overload are additive C++ API. `CodaArchive` retains its pre-change path-only layout. Existing archive methods, C ABI, CODA format, Video Profile record formats, CLI syntax, filenames, JSONL output, truth semantics, and single-stream export semantics remain unchanged. FFmpeg-disabled builds continue to compile and test the archive/snapshot layer without requiring an FFmpeg backend.
