# Stage C.4 Generic Stream Recording Design

## Status

Approved for autonomous implementation under the user's standing global
decision and publication authorization. This document defines the smallest
compatibility migration that lets a non-audio profile use the hardened engine
capture path without adopting the legacy `FeedSpec` identity and descriptor
model.

## Context

Stages B and C.1-C.3 established payload-agnostic stream descriptors, exact S0
storage, S1/D provenance, record and provenance queries, extraction, and
generic adapter/processor contracts. The remaining engine capture surface is
compatibility-oriented:

- `Engine::record()` accepts `FeedSpec`;
- stream identity is derived from the feed label and transport URI;
- capture persists a legacy `feed_descriptor`; and
- `RecordingReport` counts feeds.

The capture implementation already preserves opaque bytes and does not decode
audio. Its public identity and descriptor contract nevertheless prevents a
typed telemetry, sensor, video, document/event, or other profile from using the
same hardened file/HTTP capture path with a stable caller-owned `StreamId` and
generic `StreamDescriptor`.

## Decision

Add a separately named generic recording entry point while retaining the
legacy entry point unchanged. Both entry points share one private prepared
source loop, but each retains its own public validation and descriptor policy.

This approach is selected over two broader alternatives:

1. Converting `FeedSpec` calls into generic descriptors would silently change
   legacy archive record types, ordering, `feeds()` results, and compatibility
   expectations.
2. Adding automatic `StreamAdapter` or processor persistence would require
   archive transaction, retry, descriptor, continuity, and provenance policies
   that are outside this capture migration.

The selected boundary makes typed non-audio URI capture independently usable
without changing CODA encodings or implying a runtime registry.

## Public API

Add the following values to `<codec/engine.hpp>`:

```cpp
struct StreamSpec {
  std::string uri;
  StreamDescriptor descriptor;
  bool preserve_source{true};
  std::uint64_t maximum_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
};

struct StreamRecordingReport {
  std::filesystem::path archive;
  std::size_t streams_recorded{};
  std::uint64_t source_bytes{};
  std::uint64_t source_records{};
};
```

Add a separately named engine method:

```cpp
Result<StreamRecordingReport> record_streams(
    const std::vector<StreamSpec>& streams,
    const std::filesystem::path& archive_path) const;
```

The method is not an overload of `record()`. A separate name avoids ambiguous
initializer-list calls in existing source and makes the compatibility boundary
visible.

`StreamSpec::uri` is the authorized transport location used for this capture.
It is not the logical stream identity and is not persisted by the generic
path. `descriptor.id` is caller-owned and remains stable across transport,
connection, worker, and archive changes. `descriptor.source_id` is the
profile-defined persisted source identity, and `descriptor.payload_type`
declares the preserved S0 payload representation.

## Validation Semantics

`record_streams()` validates the complete request before creating an archive:

- at least one stream is required;
- every URI must be non-empty;
- S0 preservation must remain enabled;
- every per-stream byte limit must be non-zero;
- descriptor source ID and payload type must satisfy the existing generic
  descriptor encoder; and
- descriptor stream IDs must be unique within the request.

Labels are descriptive and may be empty or duplicated. They do not define
identity. Unknown compatible `StreamType` values remain serializable as their
underlying 16-bit values; this milestone does not introduce a closed type
registry.

Invalid request metadata returns `invalid_argument` before source preparation
or archive creation. Descriptor resource errors propagate unchanged.

After metadata validation, every source is prepared before `CodaWriter::create`
is called. This preserves the existing protections against an input aliasing
the requested archive path, including dangling symlinks to a future archive.
File, stdin, HTTP/HTTPS, redirect, DNS/private-network, chunk, and byte limits
remain governed by the existing `PreparedCapture` implementation and
`EngineConfig`. Textually recognizable private hosts are rejected during
preparation. DNS and alternate numeric representations can be rejected by the
socket policy only when capture starts; as with compatibility recording, that
later rejection can leave a structurally verified but unfinalized
descriptor-only archive and cannot append denied source bytes.

## Recording Flow

For each generic stream, in request order:

1. append exactly one versioned `stream_descriptor` for the supplied
   `StreamDescriptor`;
2. read the prepared source in bounded chunks;
3. append every accepted chunk as an S0 `source_bytes` record under the exact
   supplied `StreamId`; and
4. update `StreamRecordingReport` only after successful appends.

After all streams finish, finalize the archive. The report is returned only
after successful finalization.

The implementation will share a private engine routine for prepared-source
iteration, S0 append, counters, and finalization. Descriptor emission remains a
caller supplied private callback so the two public entry points can preserve
their distinct archive contracts:

- `record_streams(StreamSpec)` emits generic `stream_descriptor` records; and
- `record(FeedSpec)` continues to emit legacy `feed_descriptor` records.

The generic path does not persist the transport URI. A caller that intentionally
needs it in profile metadata can choose an appropriate `source_id`; credentials,
queries, and fragments are not copied implicitly.

## Truth and Failure Boundaries

Every captured payload remains S0: the exact byte span accepted from the source
and passed to `CodaWriter`. The engine performs no decoding, canonicalization,
inference, or truth reclassification.

All sources are prepared before archive creation, but capture remains
sequential and append-only once recording begins. A later read, network, or
writer failure can leave an unfinalized archive containing already committed
records, matching the existing compatibility behavior. This milestone does not
claim an all-stream transaction. It never fabricates a successful report after
partial failure.

Generic descriptor failure occurs before that stream's S0 bytes are appended.
Optional profile processing remains entirely separate, so processing failure
cannot block or rewrite completed S0 preservation.

## Compatibility

The following behavior remains unchanged:

- `FeedSpec`, `RecordingReport`, and `Engine::record()` signatures;
- legacy stream-ID derivation from `label + "\n" + uri`;
- printable unique feed-label validation;
- legacy redacted URI and `feed_descriptor` encoding;
- legacy descriptor/source record order;
- CLI and C ABI recording behavior;
- `CodaArchive::feeds()` and legacy `extract_feed()` behavior;
- CODA header, envelopes, record payload encodings, and repair;
- capabilities and audio/inference APIs; and
- installed CMake target name and version.

The new method is additive C++ API. Existing braced `record()` calls remain
unambiguous because the generic method has a different name.

## Proof Contract

Tests must establish:

1. a telemetry `StreamSpec` records a caller-owned stable ID, telemetry type,
   source ID, payload type, label, and byte-exact S0 payload;
2. the generic archive contains a `stream_descriptor`, no legacy
   `feed_descriptor`, and source records under the exact descriptor ID;
3. generic transport URI and descriptor source identity can differ without
   changing the supplied stream ID;
4. invalid/empty requests, duplicate stream IDs, disabled preservation, zero
   limits, and invalid descriptor fields fail before archive creation;
5. generic input/archive aliasing and private-network rejection preserve the
   current security behavior, including structurally verified but unfinalized
   descriptor-only output for a denial discovered after archive creation and
   no appended denied S0 bytes;
6. legacy `record(FeedSpec)` still emits a feed descriptor, populates
   `feeds()`, projects through `streams()`, and extracts identical source bytes;
7. an installed-package consumer can call `record_streams()`, reopen the
   archive, inspect the exact descriptor, and extract exact source bytes; and
8. Release, ASan/UBSan, C ABI, CLI integration, AI contract, archive,
   processing, inference, and audio tests remain green.

## Documentation Claims

README and changelog may claim only that installed C++ consumers can use the
hardened engine capture path with generic typed stream metadata and stable
caller-owned identity. They must continue to identify `FeedSpec`, CLI, and C ABI
as compatibility surfaces and must not claim a built-in telemetry or other
non-audio profile.

## Explicit Non-Claims

This milestone does not implement or claim:

- automatic persistence for arbitrary `StreamAdapter` instances;
- processor execution or atomic subject/provenance persistence;
- registry, discovery, dynamic loading, or a stable plugin ABI;
- asynchronous capture, scheduling, queues, retries, or distributed workers;
- automatic timing/continuity records or descriptor updates across epochs;
- a built-in non-audio adapter, decoder, processor, or profile;
- profile exporters or payload interpretation;
- CLI or C ABI access to `StreamSpec`;
- migration or availability of audio neural inference;
- performance, throughput, scale, deployment, or frozen CODA v1 guarantees;
  or
- completion of Stage C.

Stage C remains active after this milestone. Profile export boundaries and the
remaining organization of audio-specific APIs behind the Audio Stream Profile
require separate evidence.
