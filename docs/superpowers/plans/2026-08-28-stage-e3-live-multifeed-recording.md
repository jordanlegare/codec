# Stage E.3 Live Multi-Feed Recording Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record multiple live feeds concurrently into one preservation-first CODA archive and allow source-exact `--follow` extraction by feed label or stream ID from the verified committed prefix.

**Architecture:** Keep `CodaWriter` single-thread-owned. One worker per prepared source pushes owned chunks into bounded per-source queues; the recording thread drains one chunk per source per round-robin pass. Internal capture gains cooperative cancellation. CLI follow mode repeatedly reads `ArchiveReadPolicy::verified_prefix`, resolves one exact stream, emits only records after an archive-sequence cursor, and exits when the archive finalizes.

**Tech Stack:** C++20, POSIX file descriptors/FIFOs/poll, libcurl, CODA archive APIs, std::thread/mutex/condition_variable/atomic, Bash CLI integration tests, CMake/CTest.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-e3-live-multifeed-recording-design.md`

## Global Constraints

- Base is Stage E.2 `main` commit `bc1431839585cabb97a610da6f38d677a92564af`.
- No CODA binary-layout change.
- No CMX1 envelope is written inside CODA.
- Captured chunks remain S0 `RecordType::source_bytes` records.
- `maximum_concurrent_streams` defaults to 64, is nonzero, and may not exceed 4096.
- `maximum_queued_chunks_per_stream` defaults to 4, is nonzero, and may not exceed 1024.
- All source specs are validated/prepared before archive creation.
- All feed/stream descriptors are committed before any source chunk.
- `CodaWriter` remains owned by one thread.
- Follow mode reads only `ArchiveReadPolicy::verified_prefix` until finalized.
- Follow polling sleeps 100 ms only when no progress is made.
- No public cancellation token, graceful signal finalization, resume-to-existing-output, abandoned-writer timeout, FEC/ARQ, performance claim, C ABI change, or Stage E completion claim.

---

### Task 1: Add deterministic RED tests for concurrent live capture and bounds

**Files:**
- Modify: `tests/test_engine.cpp`
- Modify: `include/codec/engine.hpp` only after RED is demonstrated

**Interfaces:**
- Consumes: existing `Engine::record`, `CodaArchive::records(verified_prefix)`, repeated `FeedSpec`.
- Produces: test contract for `EngineConfig::maximum_concurrent_streams` and `EngineConfig::maximum_queued_chunks_per_stream`, plus descriptor-first/concurrent recording behavior.

- [ ] **Step 1: Add POSIX/concurrency test helpers**

Add includes for `atomic`, `chrono`, `fcntl.h`, `sys/stat.h`, `thread`, and `unistd.h`. Add a unique temporary FIFO helper and a bounded poll helper that repeatedly opens the growing archive and examines `records(ArchiveReadPolicy::verified_prefix)` for at most two seconds.

- [ ] **Step 2: Write the failing live-concurrency test**

Create two FIFOs, A and B. Start producer threads before `Engine::record()` so FIFO opens can pair with `PreparedCapture::prepare()`.

Producer A writes `"A-first"`, signals that it has written, then waits on `release_a` while keeping the FIFO open. Producer B writes `"B-only"` and closes.

Run `Engine::record()` in a third thread/future. Before setting `release_a`, poll the growing archive and require:

```cpp
bool saw_b_before_a_closed = false;
// Find B's feed descriptor, then a B source_bytes record in verified_prefix.
EXPECT_TRUE(saw_b_before_a_closed);
```

Also verify every `feed_descriptor` sequence precedes the first `source_bytes` sequence.

The existing sequential implementation must fail because B cannot be captured until A reaches EOF.

- [ ] **Step 3: Write failing EngineConfig bound tests**

Add tests requiring:

```cpp
codec::EngineConfig config;
config.maximum_concurrent_streams = 0;
EXPECT_FALSE(codec::Engine::create(config));

config = {};
config.maximum_concurrent_streams = 4097;
EXPECT_FALSE(codec::Engine::create(config));

config = {};
config.maximum_queued_chunks_per_stream = 0;
EXPECT_FALSE(codec::Engine::create(config));

config = {};
config.maximum_queued_chunks_per_stream = 1025;
EXPECT_FALSE(codec::Engine::create(config));
```

Add a request-size test with `maximum_concurrent_streams = 1` and two valid regular-file feeds; require `resource_exhausted` and no archive creation.

- [ ] **Step 4: Commit the tests-only RED**

Commit message:

```text
Stage E.3 RED: require concurrent live feed recording
```

Open/update a draft PR so GitHub CI proves the failure is limited to the absent E.3 config fields/concurrent behavior rather than unrelated build failures.

---

### Task 2: Add internal cooperative capture cancellation

**Files:**
- Modify: `src/capture/capture.hpp`
- Modify: `src/capture/capture.cpp`
- Test: `tests/test_engine.cpp` through the concurrent recorder failure/cancellation tests in Task 3

**Interfaces:**
- Consumes: existing `PreparedCapture`, `ByteSink`, libcurl capture path.
- Produces:

```cpp
Result<CaptureReport> PreparedCapture::run(
    const ByteSink& sink,
    const std::atomic_bool* cancelled = nullptr);
```

Internal only; no installed public API change.

- [ ] **Step 1: Change the internal signature and add cancellation utility**

Include `<atomic>` in `capture.hpp`. In `capture.cpp`, add:

```cpp
bool cancellation_requested(const std::atomic_bool* cancelled) noexcept {
  return cancelled != nullptr && cancelled->load(std::memory_order_relaxed);
}
```

Return:

```cpp
fail<CaptureReport>(ErrorCode::cancelled, "capture cancelled")
```

when cancellation is observed.

- [ ] **Step 2: Make descriptor capture interruptible**

Use `poll()` on the prepared descriptor with a 100 ms timeout before `read()`. On timeout, check cancellation and poll again. On readable/hangup state, perform the existing bounded `read()` and sink flow. Preserve existing `EINTR`, byte-limit, and read-error behavior.

Regular files are immediately readable under `poll()`.

- [ ] **Step 3: Make HTTP capture interruptible**

Add the cancellation pointer to `CurlSink`. Enable progress callbacks:

```cpp
curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, &curl_progress);
curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &context);
```

`curl_progress` returns nonzero when cancellation is requested and records a `cancelled` error in the context. Keep the existing write callback/resource limits and private-network policy unchanged.

- [ ] **Step 4: Preserve caller compatibility**

All existing `PreparedCapture::run(sink)` calls continue compiling via the default `nullptr` parameter.

- [ ] **Step 5: Run the existing unit suite**

Run `codec-unit`/direct `codec_tests` on the branch. Existing capture tests must remain green; the Task 1 E.3 concurrency test remains RED until Task 3.

- [ ] **Step 6: Commit**

Commit message:

```text
Stage E.3: add cooperative capture cancellation
```

---

### Task 3: Implement bounded concurrent recording with descriptor-first commits

**Files:**
- Modify: `include/codec/engine.hpp`
- Modify: `src/core/engine.cpp`
- Modify: `tests/test_engine.cpp`
- Modify: `tests/package_consumer/main.cpp`

**Interfaces:**
- Produces public additive config fields:

```cpp
struct EngineConfig {
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_feed_bytes{16ULL * 1024ULL * 1024ULL * 1024ULL};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
  std::size_t maximum_concurrent_streams{64};
  std::size_t maximum_queued_chunks_per_stream{4};
};
```

- Existing `Engine::record()` and `record_streams()` signatures remain unchanged.

- [ ] **Step 1: Add config validation**

`Engine::create()` rejects zero or excessive new values exactly as the spec requires.

Before calling `prepare_sources()`, both `record()` and `record_streams()` reject a source count above `maximum_concurrent_streams` with `resource_exhausted`, before archive creation.

- [ ] **Step 2: Replace sequential `record_prepared_sources()` with a bounded scheduler**

Define internal state in `engine.cpp` similar to:

```cpp
struct QueuedSource {
  StreamId stream{};
  std::deque<std::vector<std::byte>> chunks;
  bool finished{false};
};

struct ConcurrentCaptureState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<QueuedSource> sources;
  std::optional<Error> first_error;
  std::atomic_bool cancelled{false};
};
```

Do not place `CodaWriter` in this shared state.

- [ ] **Step 3: Commit every descriptor before starting workers**

After `CodaWriter::create()`, loop over all sources and call `append_descriptor(writer, index, now_ns())`. If any descriptor append fails, return immediately; no capture thread has started and no source bytes exist.

- [ ] **Step 4: Start one capture worker per source**

Each worker calls:

```cpp
prepared[index].capture.run(
    [&](std::span<const std::byte> bytes) -> Result<void> {
      std::unique_lock lock(state.mutex);
      state.changed.wait(lock, [&] {
        return state.cancelled.load(std::memory_order_relaxed) ||
               state.sources[index].chunks.size() <
                   config.maximum_queued_chunks_per_stream;
      });
      if (state.cancelled.load(std::memory_order_relaxed)) {
        return fail(ErrorCode::cancelled, "concurrent capture cancelled");
      }
      state.sources[index].chunks.emplace_back(bytes.begin(), bytes.end());
      lock.unlock();
      state.changed.notify_all();
      return {};
    },
    &state.cancelled);
```

Workers catch `std::bad_alloc` as `resource_exhausted` and all other exceptions as `internal`; no exception may escape a thread.

The first non-cancelled worker error is stored under the mutex and sets `cancelled = true`. Mark the worker `finished` and notify on every exit path.

- [ ] **Step 5: Drain queues fairly on the recording thread**

Maintain `next_source`. Under the mutex, scan at most N queues starting at that index and pop at most one chunk from the first nonempty queue. Release the lock before `writer.append()`.

After every pop, notify producers that capacity is available. After every successful append, add exact committed bytes/record counts to the report.

When no queue is nonempty, wait until a queue gains data, all workers finish, or an error appears.

Continue draining already queued chunks after a capture error as long as writer commits succeed. On writer failure, store that writer error as the operation error, set cancellation, stop appending further chunks, and join all workers.

- [ ] **Step 6: Finalize only after clean completion**

When all workers are finished, all queues are empty, and no operation error exists, join all workers and call `writer.finalize()`. Set `streams_recorded` to the number of successfully completed source workers only on clean completion; preserve existing successful report behavior.

If an operation error exists, join workers and return it without finalization.

- [ ] **Step 7: Extend tests for exact finite-feed extraction and first-error preservation**

Add a two-regular-file test with small `capture_chunk_bytes` so each feed produces several chunks. After successful recording, `extract_feed()` for each label must byte-match its source, descriptors must precede source records, and both streams must have source records.

Add a FIFO/source failure test where one source exceeds a deliberately tiny byte limit while a peer remains live. Require the returned error to be `resource_exhausted` rather than `cancelled`, and require the archive verified prefix to remain readable and unfinalized.

- [ ] **Step 8: Exercise config fields in installed-package consumer**

Construct `EngineConfig`, set both new fields to valid values, call `Engine::create(config)`, and require successful construction. This proves installed headers expose the additive fields.

- [ ] **Step 9: Run GREEN verification for Task 1/3 tests**

Run the direct unit test binary with the relevant engine prefix or full `codec-unit`. The deterministic FIFO concurrency test and all bound/error tests must pass.

- [ ] **Step 10: Commit**

Commit message:

```text
Stage E.3: record prepared sources concurrently
```

---

### Task 4: Add verified-prefix `extract --follow` and `--feeds` alias

**Files:**
- Modify: `src/cli/main.cpp`
- Modify: `tests/cli_integration.sh`

**Interfaces:**
- Existing snapshot extract commands unchanged.
- Add:

```text
codec extract ARCHIVE --feed LABEL --fidelity source-exact --follow --output FILE
codec extract ARCHIVE --stream STREAM_ID --fidelity source-exact --follow --output FILE
```

- Record parser accepts both `--feed LABEL=URI` and `--feeds LABEL=URI`.

- [ ] **Step 1: Write CLI integration RED before implementation**

Extend `tests/cli_integration.sh` with two FIFOs and producer processes. Start:

```bash
"$codec_bin" record --archive "$test_dir/live.coda" \
  --feeds "alpha=$test_dir/alpha.pipe" \
  --feed "beta=$test_dir/beta.pipe" &
record_pid=$!
```

Wait until the CODA header exists. Start:

```bash
"$codec_bin" extract "$test_dir/live.coda" --feed alpha \
  --fidelity source-exact --follow --output "$test_dir/alpha-follow.bin" &
follow_pid=$!
```

Arrange the alpha producer to write a first chunk, remain open, then write a second chunk only after the test has observed the first chunk in `alpha-follow.bin`. Beta writes independently.

Require the first alpha bytes to appear while `kill -0 "$record_pid"` still reports the recorder alive. Then release the producers, wait for record/follow success, and byte-compare final alpha and beta outputs.

The pre-E.3 CLI must fail on `--feeds` and/or `--follow`.

- [ ] **Step 2: Accept `--feeds` as an alias**

In `usage()`, keep `--feed` canonical but mention the alias. In `record_command`, accept either token:

```cpp
if (arguments[index] != "--feed" && arguments[index] != "--feeds") continue;
```

Keep the same `LABEL=URI` validation and engine duplicate-label rules.

- [ ] **Step 3: Add a secure follow-output writer**

In CLI-private code, add an RAII file descriptor wrapper or tightly scoped helper using:

```text
O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC
```

and a retrying `write()` loop. Follow mode must refuse existing output paths and close the descriptor on every exit path.

- [ ] **Step 4: Resolve feed labels from the verified prefix**

For a label selector, repeatedly call:

```cpp
archive->feeds(codec::ArchiveReadPolicy::verified_prefix)
```

Detect duplicate matching labels mapping to different stream IDs as `archive_corrupt`. If no label exists yet and the archive is not finalized, sleep 100 ms and retry. If finalized, report `feed label not found`.

For `--stream`, parse the ID exactly as snapshot mode already does.

- [ ] **Step 5: Add the archive-sequence follow cursor**

Track `std::uint64_t next_archive_sequence = 0`. On each scan, call `query_records()` with:

```cpp
codec::RecordQuery{
  .stream = selected_stream,
  .type = codec::record_type_code(codec::RecordType::source_bytes),
  .sequence = std::nullopt,
  .time = std::nullopt,
}
```

and `ArchiveReadPolicy::verified_prefix`, then skip records whose `sequence < next_archive_sequence`.

For each new record, call `read_payload()`, write all bytes to the follow output, then set `next_archive_sequence = record.sequence + 1` (guarding the uint64 maximum edge).

- [ ] **Step 6: Detect finalization only after draining**

After each scan, call `archive->verify()`. If `ok && finalized`, perform one final verified-prefix scan/drain and exit. If no progress and not finalized, sleep 100 ms.

A transient incomplete tail does not fail follow mode because record selection uses `verified_prefix`.

- [ ] **Step 7: Preserve snapshot behavior**

Only use the new loop when `--follow` is present. Existing snapshot `extract_feed()` / `extract_stream()` behavior and output JSON remain unchanged.

Follow mode prints one completion JSON object after it exits, including selector, `fidelity:"source_exact"`, `follow:true`, and total bytes written.

- [ ] **Step 8: Run CLI integration GREEN**

Run `codec-cli-integration` and repeat it enough to rule out obvious FIFO race flakiness. Then run full CTest.

- [ ] **Step 9: Commit**

Commit message:

```text
Stage E.3: follow live feed extraction
```

---

### Task 5: Update public truth surface and perform exact-head verification

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Update: PR description
- Update: roadmap issue #10 after merge

**Interfaces:**
- No new code interfaces.

- [ ] **Step 1: Update README capability manifest**

State precisely that Stage E.3 supports bounded concurrent multi-source recording into one CODA writer and verified-prefix source-exact follow extraction. Keep CMX1 external-transport framing distinct from CODA record interleaving.

Do not claim network multiplexing, recovery, graceful signal finalization, follow resume, latency/throughput, or Stage E completion.

- [ ] **Step 2: Add one CHANGELOG bullet**

Add one E.3 Unreleased entry only. Diff-check against `main` so no unrelated historical text changes ride with the milestone.

- [ ] **Step 3: Run final exact-head CI**

Require a fresh workflow on the exact final head. The gate is:

- GCC build/test/install/package-consumer: success
- Clang build/test/install/package-consumer: success
- sanitizer build/test: success
- CLI integration: success inside both compiler jobs/CTest
- no unexpected review threads
- branch base still equals the current expected `main`, or rebase/reverify if it moved

- [ ] **Step 4: Review the final diff**

Confirm only intended E.3 files changed. Specifically inspect `README.md`, `CHANGELOG.md`, `engine.cpp`, `capture.cpp`, CLI, and FIFO tests for accidental drift or unbounded waits.

- [ ] **Step 5: Mark PR ready and merge with expected-head guard**

Use the repository's accepted merge method with the exact final head SHA guard. The standing authorization permits integration only after every gate is green.

- [ ] **Step 6: Verify published tree and post-merge CI**

Require the published `main` tree to equal the exact tested PR tree. Require push-triggered post-merge CI to pass the same GCC, Clang, installed-package, CLI, and sanitizer gates.

- [ ] **Step 7: Record roadmap evidence**

Add issue #10 completion evidence containing RED run, final exact head/tree, final PR CI, merge SHA/tree equality, post-merge CI, and explicit E.3 non-claims.
