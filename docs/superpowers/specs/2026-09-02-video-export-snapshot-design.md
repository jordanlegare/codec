# Video Export Verified Snapshot Design

## Problem

`codec video export ARCHIVE --all --output-dir DIR` currently exports video streams serially. More importantly, each per-stream verified video/audio reader independently calls archive verification and metadata queries. Those generic queries rescan the entire CODA file, recompute record integrity, and decode provenance repeatedly. For an archive with many videos, work scales approximately with video count times whole-archive scan cost rather than with one verification plus the media actually exported.

## Goal

Make `video export --all` reuse one per-command verified archive snapshot so archive integrity, record metadata, stream descriptors, and provenance are established once and reused for every video export.

## Non-goals

- No global or persistent `CodaArchive` cache.
- No weakening, skipping, or deferring record CRC/SHA-256 verification.
- No change to CODA bytes, S0/S1/D semantics, provenance contracts, or single-stream export behavior.
- No parallel export worker pool in this change. Concurrency can be considered after the single-snapshot path is proved.
- No performance claim based only on wall-clock timing from CI.

## Architecture

Add a generic immutable `VerifiedArchiveSnapshot` value to the archive API. `CodaArchive::verified_snapshot()` performs one complete `scan_archive()` and fails on corrupt/incomplete complete-archive state. From that scan it derives the same stream descriptor projection and decoded provenance currently exposed through `streams()` and `provenance()`. The snapshot owns those metadata vectors and exposes const accessors plus `query_records()` and `query_provenance()` filters that apply the existing query semantics without reopening or rescanning the archive.

The snapshot does not own payload bytes and does not cache `read_payload()`. Exporters continue to read selected state payloads through the originating `CodaArchive`, which keeps payload-bound checks and avoids retaining potentially large media buffers. A snapshot is command-scoped: if the underlying file changes after snapshot creation, a future command obtains a new snapshot rather than reusing stale state.

## Video profile integration

Add internal snapshot-aware reader entry points for raw video frames, encoded video, PCM16 video audio, and encoded video audio. Existing public reader functions remain unchanged and continue to perform fresh verification through `CodaArchive`; this preserves their current semantics for API callers.

Add an overload of the MP4 export function that accepts `const VerifiedArchiveSnapshot&`. The overload routes all video/audio metadata queries through the snapshot-aware internal readers while keeping the same payload decoding, lineage validation, packet passthrough, raw-frame encoding, muxing, size limits, and error mapping.

`video export --all` will call `verified_snapshot()` before enumerating videos, use `snapshot.streams()` to select descriptors, and pass the same snapshot to every per-stream export. Single-stream CLI export remains on the existing fresh-query path.

## Integrity and mutation semantics

Snapshot construction performs the full complete-archive scan and requires both `verification.ok` and `verification.finalized`. This matches the verified video readers' existing requirement for a finalized archive. Every record link used by snapshot provenance queries is resolved against the snapshot's verified record vector. State payloads are still read by exact `RecordInfo`; `read_payload()` verifies the record envelope/hash relationship before returning bytes.

There is intentionally no cache attached to `CodaArchive`, no timestamp/size invalidation heuristic, and no mutable shared snapshot. A new `video export --all` invocation always constructs a new snapshot.

## Performance proof

Add test-only scan instrumentation inside the archive implementation, guarded by the test build, with reset/read helpers in the internal test namespace. A regression test will create a finalized archive with multiple video streams, reset the counter, build one snapshot, export multiple streams through the snapshot overload, and assert the full archive scan count remains exactly one. The same test also compares output/evidence with the existing verified export path on representative encoded and raw-frame cases where applicable.

This deterministic scan-count test is the performance evidence for the architectural claim. CI wall-clock duration is not used as a benchmark claim.

## Compatibility

The new archive snapshot API is additive. Existing archive methods, C ABI, CODA format, Video Profile record formats, CLI syntax, filenames, JSONL output, and single-stream export semantics remain unchanged. FFmpeg-disabled builds must continue to compile; snapshot construction itself has no FFmpeg dependency.
