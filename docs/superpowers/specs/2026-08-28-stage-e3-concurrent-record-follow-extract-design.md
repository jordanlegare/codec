# Stage E.3 — Concurrent multi-feed recording and verified-prefix follow extraction

Date: 2026-08-28

## Status

Approved architectural design for Stage E.3.

## Purpose

Stage E.3 makes the existing multi-feed CLI semantics genuinely multiplexed for live/long-running sources and makes source-exact extraction usable while an archive is still being written.

The user-facing target is:

```bash
codec record --archive live.coda \
  --feed LABEL1=URI1 \
  --feed LABEL2=URI2 \
  --feed LABEL3=URI3
```

with all feeds captured concurrently into one CODA archive, and concurrently:

```bash
codec extract live.coda \
  --feed LABEL1 \
  --fidelity source-exact \
  --follow \
  --output LABEL1.bin
```

which appends only newly committed source-exact bytes for `LABEL1` until the archive becomes finalized.

E.3 uses CODA's existing per-record `StreamId` multiplexing for local persistence. It does not encode local archive writes into CMX1 and immediately decode them again. CMX1 remains the generic external physical-transport multiplexing boundary introduced in E.1.

## Architectural invariants

E.3 must preserve these existing CODEC invariants:

1. S0 bytes are preserved exactly; multiplexing/scheduling never transforms payload bytes.
2. Logical stream identity remains independent of thread, worker, connection, physical record position, or archive.
3. Only one serialized `CodaWriter` owns archive mutation.
4. A reader following an open archive observes only fully committed, cryptographically verified record prefixes.
5. A partially written tail is never emitted by `--follow`.
6. Feed labels remain a compatibility lookup surface; stable `StreamId` remains the logical identity.
7. Existing one-shot `record`, `extract --feed`, and `extract --stream` behavior remains source-compatible unless an explicit new option is selected.
8. E.3 does not assign S1/D truth, perform profile decoding, or weaken E.1/E.2 transport semantics.

## Scope

E.3 contains two coupled capabilities:

- concurrent multi-source capture into one serialized CODA writer;
- verified-prefix follow extraction by feed label or stream id.

They are one milestone because the primary user story is to record many live feeds into one archive while extracting any one logical feed before finalization.

## Public C++ changes

### Engine configuration

`EngineConfig` gains explicit queue bounds for concurrent recording:

```cpp
struct EngineConfig {
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_feed_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};

  std::size_t maximum_pending_chunks_per_stream{8};
  std::uint64_t maximum_pending_bytes{64ULL * 1024ULL * 1024ULL};
};
```

Both new limits must be nonzero. The aggregate pending-byte bound applies across all producers. Queue limits are resource/backpressure controls, not scale claims.

No new overload is required: `Engine::record()` and `record_streams()` adopt concurrent capture when more than one prepared source is present while preserving the same synchronous call contract.

### Follow extraction helper

The archive layer gains a bounded incremental source-exact read primitive rather than embedding polling/sleep behavior into `CodaArchive`:

```cpp
struct SourceExactCursor {
  std::uint64_t next_archive_sequence{};
};

struct SourceExactFollowBatch {
  std::vector<ExtractedRecord> records;
  SourceExactCursor cursor;
  bool finalized{false};
};

Result<SourceExactFollowBatch> extract_stream_source_exact_prefix(
    const CodaArchive& archive,
    const StreamId& stream,
    SourceExactCursor cursor = {},
    std::size_t maximum_records = 1024,
    std::uint64_t maximum_bytes = 64ULL * 1024ULL * 1024ULL);
```

Semantics:

- always reads with `ArchiveReadPolicy::verified_prefix`;
- selects only `RecordType::source_bytes` for the exact stream;
- selects archive record sequences `>= cursor.next_archive_sequence`;
- preserves archive order;
- bounds each returned page by selected-record count and aggregate selected payload bytes;
- treats `maximum_records` and `maximum_bytes` as pagination limits, not as an error merely because more selected records are already committed;
- when the next selected record would exceed a page limit after at least one selected record has been accepted, returns the current page and leaves the cursor at or before that unreturned selected record so a later call can return it;
- if the first pending selected record by itself exceeds `maximum_bytes`, returns `resource_exhausted` because no legal page can make progress;
- advances the cursor to one past the greatest committed archive sequence actually inspected before the page boundary, not merely one past the greatest selected source record;
- reports `finalized=true` only if a valid committed `final_index` is actually inspected in that successful page; a page that ends earlier reports false so a later call can observe finalization;
- returns no partial batch on corruption or an actual resource failure.

Advancing by inspected archive sequence prevents rescanning unrelated interleaved streams indefinitely, while stopping before an unreturned selected record prevents pagination from skipping source bytes.

A label-resolution helper is not required: CLI can resolve a label to `FeedInfo.stream` from `archive.feeds(verified_prefix)` and then use the stream helper. Duplicate matching labels in one verified prefix are treated as archive corruption/ambiguity rather than arbitrary selection.

## Concurrent recording design

### Preparation phase

All existing request validation and `PreparedCapture::prepare()` operations occur before archive creation, as today. This preserves the current failure property: URI/pre-open validation failure cannot leave a newly created partial archive.

After all sources are prepared:

1. Create the CODA writer.
2. Append every feed/stream descriptor before starting any producer thread.
3. Start one producer thread per prepared capture.
4. The calling thread becomes the only writer/consumer.

### Producer messages

Each producer owns exactly one `PreparedCapture` and never touches `CodaWriter`.

A producer emits messages into its bounded queue:

```text
chunk(bytes, observed_ns)
done
ailed(error)
```

The chunk owns its bytes. `PreparedCapture::run()` may reuse its input buffer after the callback returns, so the callback must copy the exact span into the queued message before returning.

### Backpressure

Each per-stream queue is bounded by `maximum_pending_chunks_per_stream`. Aggregate queued bytes are bounded by `maximum_pending_bytes`.

When a producer would exceed a bound, it waits on a condition variable until the writer drains capacity or cancellation occurs. This is bounded backpressure, not dropping.

No source chunk may be silently discarded to maintain throughput.

### Fair scheduling

The single writer drains at most one queued chunk from a stream before advancing round-robin to the next active stream. It may skip empty/done streams.

Fairness means a continuously busy producer cannot permanently starve another stream that already has a queued chunk. It is not a timing/latency guarantee.

The physical CODA archive sequence is therefore determined by writer drain order, while each source's original byte order is preserved exactly.

### Failure and cancellation

The first producer failure or writer failure becomes the recording call's error.

On failure:

- cancellation is signaled to every producer;
- waiting producers wake and stop queueing;
- all producer threads are joined before return;
- the writer is not finalized after an unsuccessful recording;
- already committed records remain a verifiable prefix according to existing CODA semantics;
- no success report is returned.

If all producers complete normally, the writer drains all queued chunks, joins all producers, then finalizes exactly once.

A producer that has already reached EOF remains done while other producers continue.

### Determinism

E.3 does not claim deterministic cross-stream physical interleaving because producer readiness is inherently scheduling/I/O dependent.

It does guarantee:

- exact byte order within each logical stream;
- descriptor records precede all source records;
- one total committed CODA order;
- no data race on the writer;
- no silent drop under backpressure.

## CLI recording behavior

The existing repeated syntax remains canonical:

```bash
codec record --archive FILE --feed LABEL=URI [--feed LABEL=URI ...]
```

E.3 does not add a plural `--feeds` alias because repeated `--feed` already expresses the desired input list and avoids two spellings for the same option.

The existing JSON completion report remains compatible.

## Follow extraction design

### CLI syntax

```bash
codec extract ARCHIVE --feed LABEL --fidelity source-exact --follow --output FILE
codec extract ARCHIVE --stream STREAM_ID --fidelity source-exact --follow --output FILE
```

Without `--follow`, current complete-archive behavior remains unchanged.

With `--follow`:

1. Open the archive path.
2. Resolve `--feed LABEL` against the verified prefix until the descriptor becomes available, or use the explicit `--stream` directly.
3. Open the output file exactly once in truncate/create mode.
4. Repeatedly call the incremental verified-prefix helper with the current cursor.
5. Append each returned source record payload exactly once.
6. Flush output after each nonempty batch.
7. If the batch reports `finalized`, exit successfully after writing the final selected batch.
8. If no new committed record is available and the archive is not finalized, sleep for a fixed bounded polling interval and retry.

The initial E.3 polling interval is 50 ms and is not exposed as a CLI option. This is a simple local-file follow mechanism, not an event-driven tailer or latency guarantee.

### Output failure

If opening, writing, or flushing the output fails, `extract --follow` exits with an error. It does not delete or roll back bytes already emitted to the caller-selected output path.

### Archive errors while following

A currently incomplete/torn physical tail after the last valid committed record is tolerated through `verified_prefix` policy.

A corruption that invalidates the committed prefix or header fails closed.

If the archive path disappears or becomes unreadable during follow, extraction fails.

### Feed descriptor timing

Because E.3 recording commits all descriptors before producer start, a reader started immediately after archive creation will find all labels as soon as the descriptor prefix is committed. Follow extraction remains robust to older/other writers where the requested descriptor is not yet present: it polls until either the descriptor appears or the archive finalizes without it.

### Exactly-once emission

The cursor is an archive-sequence cursor. Each poll queries records in `[cursor.next_archive_sequence, +inf)` over the current verified prefix. After a successful page, the cursor advances past every committed record actually inspected before that page's boundary, including unrelated streams, but never past an unreturned selected source record.

Therefore interleaving does not cause duplicate output or unbounded rescans, and bounded pagination cannot skip selected S0 bytes.

## Existing behavior preserved

- `codec extract ...` without `--follow` still requires a complete archive via default `ArchiveReadPolicy::complete_archive`.
- Existing `CodaArchive::extract_feed()` and `extract_stream()` default semantics do not change.
- Existing legacy feed label derivation and redacted URI descriptor behavior remain compatible.
- `record_streams()` gets the same concurrency machinery as `record()` so generic stream recording does not regress behind the legacy feed API.

## Tests

### RED/recording tests

Tests must prove the current serial engine cannot satisfy the new contract before production changes are added.

Use controlled local sources whose first source blocks while the second becomes readable. The RED assertion must show that, under the old implementation, the second source cannot contribute a committed record while the first remains open.

GREEN tests must prove:

1. descriptors for all streams are committed before source payloads;
2. two or more sources can contribute records before any long-lived source completes;
3. per-stream byte order is exact;
4. round-robin draining prevents starvation of an already queued stream;
5. per-stream and aggregate queue bounds use backpressure rather than drop;
6. one source EOF does not stop others;
7. producer failure cancels and joins peers and leaves no finalized archive;
8. writer failure cancels and joins peers;
9. single-feed recording remains compatible;
10. `record_streams()` shares the same concurrency semantics.

### Follow helper tests

Prove:

1. reads verified prefix from an unfinalized archive;
2. excludes an incomplete tail;
3. returns only exact selected-stream `source_bytes`;
4. cursor advances across unrelated interleaved records;
5. repeated calls emit every selected record exactly once;
6. record and aggregate-byte limits paginate without skipping the first unreturned selected record;
7. a single pending selected record larger than `maximum_bytes` returns `resource_exhausted` without pretending pagination can make progress;
8. finalization is reported only when a valid committed final index is actually inspected;
9. committed-prefix corruption fails closed.

### CLI integration tests

Prove:

1. repeated `--feed` recording remains accepted;
2. concurrent finite/streaming fixtures interleave into one archive;
3. `extract --feed LABEL --follow` can start before recording finalizes and emits exact bytes;
4. another feed's bytes are never emitted;
5. `--stream ... --follow` works equivalently;
6. follow exits after finalization;
7. non-follow extraction behavior is unchanged;
8. invalid combinations such as `--follow` with unsupported fidelity fail argument validation.

### Package and sanitizer gates

Installed-package consumer must compile and exercise the new C++ incremental helper.

Full GCC, Clang, package-consumer, C API, CLI integration, AI-contract, and sanitizer suites must remain green.

## Error mapping

- invalid zero queue/follow bounds: `invalid_argument`;
- recording pending-memory exhaustion where waiting cannot make progress, or a first pending selected follow record individually larger than `maximum_bytes`: `resource_exhausted`;
- ordinary follow `maximum_records` / aggregate-page-byte exhaustion after at least one selected record: successful paginated batch, not an error;
- capture provider failure: preserve existing provider error;
- archive mutation/read failures: preserve existing archive error classes;
- malformed committed archive semantics: `archive_corrupt`;
- output filesystem failures: existing CLI/file I/O error handling.

## Non-claims

E.3 does not add:

- a `--feeds` plural alias;
- CMX1-in-CODA local wrapping;
- socket/network transport providers;
- distributed capture workers;
- FEC/parity/reconstruction/retransmission;
- automatic `StreamGap` persistence from local scheduling;
- profile decoding or S1/D generation;
- deterministic cross-stream physical ordering;
- hard real-time scheduling;
- throughput/latency/scale guarantees;
- output rollback for a failed follow destination;
- CLI C-ABI parity for follow mode;
- Stage E completion.

## Dependency direction after E.3

After E.3, CODEC has usable local archive multiplexing plus live verified-prefix extraction. A later Stage E milestone may connect CMX1 to actual pipe/socket/network transports and then apply E.2 recovery semantics and a concrete repair/FEC mechanism. Those remain separate from E.3.