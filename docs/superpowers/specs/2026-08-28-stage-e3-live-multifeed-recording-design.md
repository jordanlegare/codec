# Stage E.3 — Live multi-feed recording and verified-prefix follow extraction

Date: 2026-08-28
Status: approved for implementation under the standing repository design/integration authorization

## Purpose

Stage E.3 connects CODEC's existing multi-feed CLI, preservation archive, and Stage E transport-era stream semantics into an actually usable live workflow:

```text
codec record --archive live.coda \
  --feed LABEL1=URI1 \
  --feed LABEL2=URI2

codec extract live.coda --feed LABEL1 \
  --fidelity source-exact --follow --output LABEL1.bin
```

All requested feeds within the configured concurrency bound must capture concurrently. Their exact S0 chunks are serialized through one CODA writer and may therefore be physically interleaved while retaining distinct `StreamId`s. A concurrent extractor must read only fully committed records from CODA's verified prefix and append only newly committed S0 bytes for the selected feed or stream.

E.3 does not embed CMX1 envelopes inside CODA. CODA already multiplexes physical records by `StreamId`; E.1 remains the framing boundary for an external shared byte transport.

## Existing constraints

The current CLI already accepts repeated `--feed LABEL=URI` arguments and maps them to `Engine::record()`. The current engine prepares every source first but then captures each source to completion before starting the next one. An endless first feed therefore prevents later feeds from ever being recorded.

`CodaWriter::append()` already writes envelope, payload, and commit trailer and `fsync()`s each record. `CodaArchive` already supports `ArchiveReadPolicy::verified_prefix`. These two existing properties are the preservation boundary for live following.

## Scope

E.3 adds:

1. concurrent capture for all prepared sources in one `Engine::record()` / `Engine::record_streams()` operation, up to an explicit configured source/thread bound;
2. bounded per-source buffering and fair single-writer draining;
3. cooperative internal cancellation so one failed source or writer failure can stop the other capture workers instead of waiting for unrelated live sources forever;
4. descriptor-first persistence: all feed/stream descriptors are committed before any source chunk;
5. CLI compatibility for repeated `--feed`, plus `--feeds` as an accepted alias;
6. `codec extract ... --follow` for source-exact extraction by feed label or exact stream ID;
7. verified-prefix cursoring so a follower never duplicates a previously emitted archive record and never consumes an uncommitted tail;
8. tests that prove a second live feed is committed while the first remains open, and that a follower emits bytes before recording finishes.

## Concurrent recording architecture

### Preparation

All input specs are validated and all `PreparedCapture`s are created before `CodaWriter::create()` is called. This preserves the current failure isolation rule: a pre-capture validation/open failure must not create an archive.

After writer creation, every descriptor is committed before capture workers start. For legacy feeds, the existing deterministic stream identity remains:

```text
derive_stream_id(label + "\n" + uri)
```

### Explicit concurrency and queue bounds

`EngineConfig` gains:

```cpp
std::size_t maximum_concurrent_streams{64};
std::size_t maximum_queued_chunks_per_stream{4};
```

Both values must be nonzero. `maximum_concurrent_streams` may not exceed 4096 and `maximum_queued_chunks_per_stream` may not exceed 1024. A recording request whose source count exceeds `maximum_concurrent_streams` fails with `resource_exhausted` before archive creation.

Each per-source queue therefore holds at most:

```text
maximum_queued_chunks_per_stream * capture_chunk_bytes
```

plus allocator/container overhead. Producers block on queue capacity rather than growing memory without bound.

### Worker model

One capture worker thread owns each prepared source. A worker runs the existing capture engine and copies each delivered chunk into that source's bounded queue.

### Single writer and fairness

`CodaWriter` remains single-thread-owned. The recording thread drains at most one queued chunk per source during each round-robin pass and commits it as `RecordType::source_bytes` with that source's `StreamId`.

This yields bounded fairness between sources without claiming real-time scheduling. Physical archive order is the order in which the writer commits chunks; it does not redefine logical stream sequence or identity.

### Completion

If all capture workers reach EOF successfully, the writer drains all remaining queued chunks, joins all workers, finalizes CODA, and returns the existing recording report.

The report counts bytes and records actually committed to CODA.

## Failure and cancellation semantics

### Capture failure

The first non-cancellation capture error becomes the operation error. A shared cancellation flag is set. Other workers cooperatively stop, already queued chunks are drained and committed when possible, workers are joined, and the archive is left unfinalized. The exact committed prefix remains recoverable/readable under existing CODA semantics.

### Writer failure

A writer error becomes the operation error immediately, sets cancellation, stops further queue production, joins the workers, and returns without finalization. No later capture error may replace the writer error.

### Cooperative cancellation in capture

The internal `PreparedCapture::run()` path gains an optional cancellation flag.

Local descriptor capture uses `poll()` with a bounded wake interval before blocking reads, checks cancellation on every wake, and returns `ErrorCode::cancelled` when requested. Regular files remain effectively immediately readable. HTTP capture enables libcurl progress callbacks (`CURLOPT_NOPROGRESS=0`, `CURLOPT_XFERINFOFUNCTION`) so cancellation does not depend solely on receiving another body chunk.

Cancellation errors from peer workers are not allowed to overwrite the original operation failure.

E.3 does not add a public cancellation token or signal-handling contract.

## Live extraction architecture

### CLI surface

Existing snapshot behavior remains:

```text
codec extract ARCHIVE --feed LABEL --fidelity source-exact --output FILE
codec extract ARCHIVE --stream STREAM_ID --fidelity source-exact --output FILE
```

E.3 adds follow mode for either existing selector:

```text
codec extract ARCHIVE --feed LABEL --fidelity source-exact --follow --output FILE
codec extract ARCHIVE --stream STREAM_ID --fidelity source-exact --follow --output FILE
```

No new fidelity is introduced.

### Output creation

Follow mode creates a new output file with exclusive/no-follow semantics and refuses to replace an existing path. E.3 does not add resume-to-an-existing-output behavior.

### Feed resolution

A feed-label follower repeatedly inspects `feeds(ArchiveReadPolicy::verified_prefix)` until the requested label is available. Duplicate labels mapping to different streams are treated as archive corruption, matching `extract_feed()` semantics.

If the archive finalizes before the requested feed descriptor ever appears, follow extraction fails with `invalid_argument` / feed-not-found.

A stream-ID follower already has its exact logical identity and therefore skips label resolution.

### Record cursor

The follower keeps the next archive sequence it has not emitted. Each polling iteration queries only `source_bytes` records for the selected stream from the verified prefix and ignores already emitted sequences.

For every returned record:

1. `read_payload()` re-verifies the exact record at its physical offset;
2. bytes are appended to the output;
3. the archive-sequence cursor advances only after the output write succeeds.

The cursor is an archive-record cursor, not a logical transport sequence number.

### Tail handling

An incomplete physical record at the writer's tail is not an error in follow mode. `ArchiveReadPolicy::verified_prefix` exposes only the last completely committed prefix.

When verification reports a valid finalized archive, the follower drains any remaining selected records and exits successfully.

A crashed writer that leaves an incomplete tail is not automatically distinguishable from an active writer. E.3 therefore makes no automatic abandoned-writer timeout claim; the follower may continue until interrupted.

### Polling

The CLI sleeps for 100 milliseconds between scans that make no progress. This is an implementation cadence, not a latency guarantee.

## CLI feed option compatibility

`--feed LABEL=URI` remains the documented canonical spelling. `--feeds LABEL=URI` is accepted as an additive alias because users naturally use the plural when supplying several entries. Both forms may be mixed, and duplicate labels remain invalid at the engine layer.

## Truth and provenance

E.3 changes no truth semantics.

- Captured source chunks remain S0 `source_bytes` records.
- Feed labels remain lookup metadata and never replace `StreamId` as logical identity.
- Concurrent physical interleaving does not assign S1/D truth.
- Follow extraction is exact concatenation of committed S0 source records for one selected stream.
- CMX1 is not persisted merely to implement concurrent recording.

## Tests

### Engine concurrency proof

A deterministic FIFO test creates two live feeds. Both FIFO writers are connected before recording begins. Feed A writes one chunk and remains open. Feed B writes a chunk and closes.

Before allowing A to close, the test polls the growing archive under `verified_prefix` and must observe B's committed `source_bytes` record. The pre-E.3 sequential engine cannot satisfy this condition.

The same test verifies all feed descriptors precede every source record.

### Bounds and failure tests

Tests cover:

- zero or excessive concurrency/queue bounds rejected at `Engine::create()`;
- a source count above `maximum_concurrent_streams` rejected before archive creation;
- bounded queue configuration accepted;
- concurrent finite feeds preserve exact bytes for every label;
- duplicate labels remain rejected;
- source failure returns an error and leaves an unfinalized verified prefix rather than fabricating finalization;
- cancellation of peer workers does not replace the first real failure;
- existing single-feed and generic stream recording behavior remains compatible.

### CLI follow integration proof

The integration test uses two FIFO feeds and a background recorder. It starts a `--follow` extractor while the archive is still open, verifies the selected feed's first bytes appear in the output before recording finalizes, then allows the feeds to end. The follower must exit after finalization and the resulting output must byte-match the selected input stream.

The test also exercises the `--feeds` alias.

### Existing gates

E.3 must keep all existing GCC, Clang, installed-package consumer, C ABI, CLI integration, AI-contract, and sanitizer gates green.

## Non-claims

E.3 does not add:

- CMX1 storage inside CODA;
- network transport multiplexing or sockets beyond the existing URI capture path;
- FEC, reconstruction, retransmission, or ARQ;
- persistence of E.2 recovery observations;
- public thread/scheduler APIs;
- hard real-time fairness, latency, throughput, or scale guarantees;
- graceful SIGINT/SIGTERM finalization;
- output resume/checkpoint files for `--follow`;
- abandoned-writer detection/timeouts;
- automatic transcoding/profile export while following;
- S1/D inference while recording;
- CODA binary-layout changes;
- C ABI additions;
- Stage E completion.

## Stage boundary

E.3 makes the existing preservation archive operational for simultaneous live feeds and live source-exact consumers. A later Stage E milestone may transport the same logical streams over CMX1 across an actual shared external channel and then apply E.2/E.3 recovery/recording semantics at the receiving side.
