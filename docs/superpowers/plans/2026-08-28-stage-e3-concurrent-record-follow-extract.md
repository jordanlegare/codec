# Stage E.3 Concurrent Record / Follow Extract Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record multiple feeds/streams concurrently into one CODA archive and allow source-exact extraction by feed label or stream id while the archive is still growing.

**Architecture:** Keep `CodaWriter` single-threaded. Each prepared capture runs in its own producer thread and copies callback bytes into a bounded per-stream queue; the calling thread drains queues fairly and serializes source records into CODA. Follow extraction is a bounded archive helper over `ArchiveReadPolicy::verified_prefix` plus a CLI polling loop using an archive-sequence cursor.

**Tech Stack:** C++20, std::thread/std::mutex/std::condition_variable, existing `PreparedCapture`, CODA archive APIs, bash CLI integration tests, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-e3-concurrent-record-follow-extract-design.md`

## Global Constraints

- Preserve source bytes exactly; no chunk transformation, dropping, resampling, decoding, or truth reassignment.
- `CodaWriter` remains single-owner/single-threaded.
- All descriptors commit before producer threads begin.
- Queue bounds are nonzero and backpressure blocks rather than drops.
- Follow mode reads only `ArchiveReadPolicy::verified_prefix`.
- Existing non-follow extraction keeps `complete_archive` default semantics.
- Local recording uses CODA stream multiplexing directly; do not wrap local writes in CMX1.
- No FEC/retransmission/network/provider/scale claims.

---

### Task 1: Bounded verified-prefix source-exact cursor

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/archive/archive.cpp`
- Create: `tests/test_archive_follow.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `SourceExactCursor { uint64_t next_archive_sequence; }`
  - `SourceExactFollowBatch { vector<ExtractedRecord> records; SourceExactCursor cursor; bool finalized; }`
  - `extract_stream_source_exact_prefix(const CodaArchive&, const StreamId&, SourceExactCursor, size_t maximum_records, uint64_t maximum_bytes)`

- [ ] **Step 1: Write RED tests**

Add tests that construct an unfinalized CODA archive with interleaved source records for A/B and verify the new helper is callable, returns only A, advances its cursor past unrelated committed records, excludes an incomplete tail, enforces nonzero count/byte limits, and reports finalization only after a committed final index exists.

- [ ] **Step 2: Run CI/targeted tests and confirm RED**

Expected failure: missing public cursor/batch/helper symbols only.

- [ ] **Step 3: Implement minimal helper**

Validation:

```cpp
if (maximum_records == 0 || maximum_bytes == 0) {
  return fail<SourceExactFollowBatch>(ErrorCode::invalid_argument,
                                      "follow extraction limits must be nonzero");
}
```

Read `archive.records(verified_prefix)` once. Iterate committed records from `cursor.next_archive_sequence`, tracking the greatest inspected archive sequence. Collect only exact selected-stream `source_bytes`; preflight record/aggregate-byte bounds before reading payload. Set `finalized=true` only if a committed `final_index` appears in the inspected verified prefix. Return no partial batch on failure.

- [ ] **Step 4: Verify GREEN**

Run full unit suite and sanitizer subset.

- [ ] **Step 5: Commit**

Commit message: `Stage E.3: add verified-prefix source cursor`

---

### Task 2: Concurrent bounded recorder scheduler

**Files:**
- Modify: `include/codec/engine.hpp`
- Modify: `src/core/engine.cpp`
- Modify/Create tests in: `tests/test_engine.cpp`

**Interfaces:**
- Extend `EngineConfig` with:

```cpp
std::size_t maximum_pending_chunks_per_stream{8};
std::uint64_t maximum_pending_bytes{64ULL * 1024ULL * 1024ULL};
```

- Existing `Engine::record()` / `record_streams()` signatures remain unchanged.

- [ ] **Step 1: Write RED concurrency tests**

Use controlled FIFO/local streaming fixtures or test-only capture hooks already available in repository patterns. Prove a first source may remain open while a second source contributes committed bytes, something the current sequential `record_prepared_sources()` cannot do. Also test zero queue limits.

- [ ] **Step 2: Confirm RED is isolated**

Expected failures: new config members/behavior absent; existing tests green.

- [ ] **Step 3: Implement producer queue state**

Add private implementation types in `engine.cpp` only:

```cpp
struct PendingChunk {
  std::vector<std::byte> bytes;
  std::int64_t observed_ns{};
};

struct ProducerState {
  std::deque<PendingChunk> chunks;
  bool done{false};
  std::optional<Error> error;
};
```

Shared scheduler state owns mutex, condition variable, cancellation flag, aggregate pending-byte count, and vector of producer states.

- [ ] **Step 4: Commit all descriptors before thread start**

Preserve existing feed/stream descriptor encoders. Create writer only after all captures were successfully prepared. Append all descriptors in input order before constructing producer threads.

- [ ] **Step 5: Start one producer per prepared capture**

Each producer runs its owned `PreparedCapture::run()`. Callback copies exact bytes into an owned vector, waits until both per-stream chunk capacity and aggregate byte capacity permit enqueue, then pushes the chunk and wakes the writer. No producer calls writer APIs.

- [ ] **Step 6: Implement fair single-writer drain**

Writer loop rotates stream index. At most one queued chunk is consumed from an active stream before moving to the next. Append as `RecordType::source_bytes` using the producer-observed timestamp and exact stream id. After dequeue, reduce aggregate pending bytes and wake blocked producers.

- [ ] **Step 7: Implement failure/cancellation/join rules**

First producer or writer error sets cancellation. Wake all waiters; producers stop queueing; join every thread before return. Never finalize on failure. On normal completion, drain queues, join, then finalize once.

- [ ] **Step 8: Add GREEN coverage**

Prove:
- multiple live sources contribute before first EOF;
- descriptors precede every source record;
- exact per-stream byte order;
- already queued stream cannot starve;
- queue bounds block/no-drop;
- one EOF does not stop peers;
- capture failure leaves non-finalized verified prefix;
- single-feed compatibility;
- `record_streams()` gets same scheduler.

- [ ] **Step 9: Full verification and commit**

Commit message: `Stage E.3: record sources concurrently`

---

### Task 3: CLI `extract --follow`

**Files:**
- Modify: `src/cli/main.cpp`
- Modify: `tests/cli_integration.sh`

**Interfaces:**
- New CLI flag: `--follow` valid only with `extract`, exactly one of `--feed`/`--stream`, and `source-exact` fidelity.

- [ ] **Step 1: Write RED CLI tests**

Add argument-validation RED tests and an integration case that starts a writer process producing a growing archive while follow extraction runs concurrently. Verify selected feed bytes appear before writer finalization and final output is exact after writer exits.

- [ ] **Step 2: Confirm RED**

Expected: CLI rejects/ignores `--follow` because it is not implemented.

- [ ] **Step 3: Refactor stream selection into helper**

Keep non-follow path semantically unchanged. For follow by label, query `archive.feeds(verified_prefix)` until exactly one matching label appears; duplicate matches fail closed; finalized-without-label returns not found.

- [ ] **Step 4: Implement follow loop**

Open output once in truncate/binary mode. Keep `SourceExactCursor`. Call the Task-1 helper with bounded defaults, append returned payloads in archive order, flush after nonempty batches, and sleep `50ms` only when no new records are available and finalization is false. Exit success when finalization is true after writing the last batch.

- [ ] **Step 5: Preserve one-shot extraction**

Without `--follow`, continue using existing `extract_feed()` / `extract_stream()` complete-archive behavior.

- [ ] **Step 6: GREEN CLI integration**

Verify feed and stream follow, no cross-feed bytes, exact output, finalization exit, invalid combinations, and unchanged one-shot behavior.

- [ ] **Step 7: Commit**

Commit message: `Stage E.3: add follow extraction CLI`

---

### Task 4: Package surface, documentation, exact-head verification

**Files:**
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Modify spec/plan only if implementation evidence requires qualification.

- [ ] **Step 1: Package consumer**

Compile and exercise `SourceExactCursor` and `extract_stream_source_exact_prefix()` from installed headers/library.

- [ ] **Step 2: Documentation**

Document repeated `--feed` as concurrent, `--follow` semantics, verified-prefix safety, and explicit non-claims. Avoid unrelated historical edits.

- [ ] **Step 3: Final diff review**

Compare branch to E.2 base. Check for accidental README/CHANGELOG drift and ensure only intended files changed.

- [ ] **Step 4: Exact-head CI**

Require GCC build/test/install/package-consumer success, Clang equivalent success, sanitizer success, CLI integration success, C API and AI-contract success.

- [ ] **Step 5: PR metadata/review**

Open or update PR with RED→GREEN evidence, exact head/tree, nonclaims. Require no unresolved review threads and no base drift.

- [ ] **Step 6: Merge with expected-head guard**

Squash merge only the exact green PR head into `main`.

- [ ] **Step 7: Post-merge verification**

Confirm published tree equals tested tree and push CI on `main` is fully green. Record completion evidence on roadmap issue #10.
