# Stage C.1 Generic Record Query and Extraction Design

## Status

Approved for autonomous implementation under the user's standing decision and
publication authorization. The canonical work record is the `Planned work —
Stage C.1 generic record query/extraction boundary` comment on GitHub issue
#10.

## Context

Stage B established generic stream identifiers, opaque 16-bit record type
codes, persisted descriptors, continuity, and declared S1/D provenance. The
remaining public archive surface still has two gaps:

- `records()` can only return the whole committed record list; and
- `extract_stream_raw()` requires one stream/type pair and concatenates all
  matching payloads, erasing record boundaries.

Stage C requires generic query and extraction boundaries that a future
non-audio profile can use without modifying CODA or depending on `FeedInfo`,
PCM, watermarks, model semantics, or profile payload interpretation.

## Scope

This milestone adds a physical-record query boundary over fields already
authenticated by each CODA record envelope:

- exact logical stream ID;
- exact raw 16-bit record type code;
- half-open archive sequence range; and
- half-open envelope time range, using interval overlap.

It also adds boundary-preserving extraction that returns each matching
`RecordInfo` beside its individually verified payload.

The following remain out of scope:

- truth-class filtering or inference;
- provenance graph traversal;
- semantic clock, epoch, or gap queries;
- event identity, model/version, confidence, or policy filters;
- profile-specific exporters;
- adapter or processor registration;
- CLI or C ABI additions;
- indexes, performance claims, or a frozen CODA v1 format.

## Public API

Add these C++ types to `include/codec/archive.hpp` after `RecordInfo`:

```cpp
struct RecordSequenceRange {
  std::uint64_t begin{};
  std::uint64_t end{};
};

struct RecordTimeRange {
  std::int64_t begin_ns{};
  std::int64_t end_ns{};
};

struct RecordQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTypeCode> type;
  std::optional<RecordSequenceRange> sequence;
  std::optional<RecordTimeRange> time;
};

struct ExtractedRecord {
  RecordInfo record{};
  std::vector<std::byte> payload;
};
```

Add these methods to `CodaArchive`:

```cpp
Result<std::vector<RecordInfo>> query_records(
    const RecordQuery& query,
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;

Result<std::vector<ExtractedRecord>> extract_records(
    const RecordQuery& query,
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
```

No virtual interface, registry, callback, payload decoder, or new dependency is
introduced.

## Query Semantics

All present filters are combined with logical AND. Missing filters are
wildcards. Results remain in authenticated archive sequence order.

Sequence ranges are half-open: `[begin, end)`. A record matches when:

```text
begin <= record.sequence < end
```

Time ranges are half-open: `[begin_ns, end_ns)`. Record envelope times are
treated as half-open intervals. A non-point record matches on overlap:

```text
record.start_ns < query.end_ns && query.begin_ns < record.end_ns
```

An envelope point event has `start_ns == end_ns`; it matches when its timestamp
is contained by the query range:

```text
query.begin_ns <= record.start_ns && record.start_ns < query.end_ns
```

This is envelope-time selection only. It does not interpret `StreamClock` or
any profile payload.

An empty or inverted sequence/time range (`begin >= end`) is invalid and
returns `ErrorCode::invalid_argument` before archive scanning.

The query includes any committed record whose authenticated envelope fields
match, including unknown record types and metadata records. Callers that want
artifacts rather than metadata supply an exact type filter or post-filter the
returned `RecordInfo` values. The core does not invent a new artifact taxonomy.

## Extraction Semantics

`extract_records()` delegates selection to `query_records()`, calls the
existing hash-verifying `read_payload()` for each match, and returns one
`ExtractedRecord` per match. It does not concatenate payloads, decode them, or
rewrite their metadata.

Existing `extract_stream()` and `extract_stream_raw()` retain their signatures
and concatenating behavior. `extract_stream_raw()` may delegate to the new
boundary so the two selection paths cannot drift.

## Truth and Provenance Boundaries

The query operates on physical committed records. It does not label records as
S0, S1, or D. S1/D declaration and supporting inputs remain available through
`provenance()`.

- S0 payload bytes remain exact.
- An S1 subject payload can be selected and extracted opaquely without
  changing its separate provenance declaration.
- A D subject payload can be selected and extracted opaquely without dropping
  or rewriting its mandatory provenance sidecar.

The query and extraction methods do not call `provenance()`. Therefore a
structurally valid archive with semantically malformed optional provenance can
still recover exact physical payloads, matching Stage B failure isolation.

## Archive Policies and Compatibility

Both methods accept the existing `ArchiveReadPolicy`:

- `complete_archive` requires a complete structurally valid archive; and
- `verified_prefix` selects/extracts only records in the verified committed
  prefix when the tail is torn.

The archive header, envelope, record type registry, writer, final index, repair
behavior, C ABI, CLI, and installed CMake target remain unchanged. Existing
source remains compatible because the change only adds C++ aggregate types and
methods.

## Failure Model

- Invalid query ranges: `invalid_argument`.
- Archive structural failure under `complete_archive`: existing scan error.
- Invalid header under either policy: existing scan error.
- Payload changed after record scan: existing `archive_corrupt` from
  `read_payload()`.
- Allocation or aggregate size performance guarantees: no new claim.

An empty valid result is successful and returns an empty vector.

## Proof Contract

Tests must establish:

1. stream, unknown raw type, sequence, and time filters combine with AND;
2. results remain in archive order;
3. half-open overlap excludes an interval ending at the query start;
4. point events at the query start match and at the query end do not;
5. invalid/empty sequence and time ranges return `invalid_argument`;
6. extracted payloads are byte-exact and retain their corresponding stream,
   type, sequence, and hash metadata;
7. `verified_prefix` query and extraction ignore a torn tail;
8. existing typed/raw/feed extraction, C ABI, repair, and provenance tests stay
   green; and
9. an installed-package consumer can compile, link, query, and extract an
   unknown non-audio record.

Release, ASan/UBSan, GCC CI, Clang CI, sanitizer CI, diff/claim audit, and exact
published-tree verification remain mandatory before advancing `main`.
