# Stage B.4 Generic Stream Continuity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist and deterministically validate generic per-stream S0 timing, logical sequence, connection/format epoch, explicit gap, and exact source-record integrity-link semantics in the provisional CODA development profile.

**Architecture:** Keep the 64-byte archive header and 96-byte record envelope unchanged. Add versioned `STM1` timing and `SGP1` gap sidecar payloads plus type-safe C++ append/read APIs; both writer and reader advance the same per-stream ledger rules, while timing records bind an already committed same-stream `source_bytes` record by archive sequence and SHA-256.

**Tech Stack:** C++20, CMake 3.20+, OpenSSL-backed existing build, the repository test harness, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md`, Stage B and testing requirements; canonical approved work record is GitHub issue #10, `Planned work — Stage B.4 generic stream continuity`.

## Global Constraints

- Preserve S0 before optional interpretation; do not add S1 or D behavior.
- Do not change the 64-byte CODA header, 96-byte record envelope, C ABI v1, CLI, audio-profile behavior, or archive development-profile version.
- Logical sequence starts at zero per stream and advances by one timing event or by an explicitly nonzero missing-count gap.
- Connection and format epochs never decrease; monotonic time never decreases within one connection epoch.
- Source timebase numerator and denominator must both be positive.
- A timing event must reference an earlier, exact, same-stream `source_bytes` record by archive sequence and SHA-256.
- Legacy archives remain valid and expose an empty continuity view.
- Malformed or semantically invalid continuity metadata must not corrupt or block verification/extraction of committed S0 bytes.
- No model, identity, fidelity, scale, deployment, frozen CODA v1, provenance-envelope, or release-readiness claim.

---

### Task 1: Add the public continuity model and happy-path persistence

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/core/internal.hpp`
- Create: `src/core/stream_continuity.cpp`
- Modify: `src/archive/archive.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: existing `StreamId`, `StreamClock`, `StreamEpoch`, `RecordInfo`, `Sha256`, `CodaWriter::append`, and `CodaArchive::records/read_payload`.
- Produces:

```cpp
enum class StreamContinuityKind : std::uint8_t { timing = 1, gap = 2 };

struct StreamTiming {
  StreamId stream{};
  std::uint64_t sequence{};
  StreamClock clock{};
  StreamEpoch epoch{};
  std::uint64_t source_record_sequence{};
  Sha256 source_record_hash{};
};

struct StreamGap {
  StreamId stream{};
  std::uint64_t first_sequence{};
  std::uint64_t missing_count{};
  StreamEpoch epoch{};
};

struct StreamContinuityEvent {
  StreamContinuityKind kind{StreamContinuityKind::timing};
  StreamTiming timing{};
  StreamGap gap{};
};

Result<RecordInfo> CodaWriter::append_stream_timing(
    const RecordInfo& source, std::uint64_t stream_sequence,
    const StreamClock& clock, const StreamEpoch& epoch);
Result<RecordInfo> CodaWriter::append_stream_gap(
    const StreamId& stream, std::uint64_t first_sequence,
    std::uint64_t missing_count, const StreamEpoch& epoch);
Result<std::vector<StreamContinuityEvent>> CodaArchive::continuity(
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
```

- Adds registered record type `stream_timing = 6`; the existing `gap = 4` code remains unchanged.
- `STM1` is a fixed 120-byte little-endian payload: magic/version/reserved, logical sequence, connection epoch, format epoch, all six `StreamClock` dimensions, source archive sequence, and 32-byte source hash.
- `SGP1` is a fixed 40-byte little-endian payload: magic/version/reserved, first missing sequence, missing count, connection epoch, and format epoch.

- [ ] **Step 1: Write the failing happy-path test**

Add `generic_stream_continuity_round_trips_exact_timing_and_gaps` to `tests/test_archive.cpp`. It writes two telemetry `source_bytes` records, attaches timing sequence 0 to the first, inserts gap `[1, 3)`, attaches timing sequence 3 to the second after a reconnect/format transition, finalizes, reads `continuity()`, and compares every clock, epoch, logical-sequence, source archive-sequence, and source-hash field to hand-authored literals and the committed `RecordInfo` values.

- [ ] **Step 2: Run the unit build to verify RED**

Run:

```bash
cmake --build build --parallel
```

Expected: compilation fails only because the continuity types, record code, and public methods do not yet exist.

- [ ] **Step 3: Implement the fixed payload codecs and public happy path**

Add the public structs/signatures above. Add `detail::encode/decode_stream_timing` and `detail::encode/decode_stream_gap` in `stream_continuity.cpp`; reject the wrong magic, version, reserved field, or exact payload size as `archive_corrupt`. Add writer methods that encode and append the registered sidecar record and a reader method that decodes timing/gap records in archive order.

- [ ] **Step 4: Run the focused unit suite to verify GREEN**

Run:

```bash
cmake --build build --parallel
./build/codec_tests
```

Expected: the new happy-path test and all existing unit tests pass.

- [ ] **Step 5: Commit the happy path**

```bash
git add CMakeLists.txt include/codec/archive.hpp src/core/internal.hpp \
  src/core/stream_continuity.cpp src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: persist generic stream continuity"
```

### Task 2: Enforce the writer-side logical stream ledger

**Files:**
- Modify: `src/core/internal.hpp`
- Modify: `src/core/stream_continuity.cpp`
- Modify: `src/archive/archive.cpp`
- Test: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: Task 1 public event types and codecs.
- Produces `detail::StreamContinuityState` plus overloads of `detail::validate_and_advance` shared by structured writer and archive reader paths.

```cpp
struct StreamContinuityState {
  std::uint64_t next_sequence{};
  StreamEpoch epoch{};
  std::int64_t last_monotonic_ns{};
  bool initialized{false};
  bool has_monotonic{false};
};

Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamTiming& timing);
Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamGap& gap);
```

- [ ] **Step 1: Write failing writer-validation tests**

Add tests proving structured appends reject, without consuming ledger state or appending a record:

```text
source link not present in this writer
source link hash/stream/type mismatch
zero or negative source timebase component
first timing sequence other than zero
unannounced sequence jump
zero-length gap or gap not starting at next sequence
connection or format epoch regression
monotonic clock regression within one connection
logical sequence overflow
```

The reconnect case must show that monotonic time may restart when the connection epoch increases while logical sequence remains continuous.

- [ ] **Step 2: Run the unit suite to verify RED**

Run:

```bash
cmake --build build --parallel
./build/codec_tests
```

Expected: the new validation assertions fail because Task 1 accepts semantically invalid events.

- [ ] **Step 3: Implement state validation with atomic advancement**

Store `std::map<StreamId, detail::StreamContinuityState>` in `CodaWriter::Impl`. Validate against a copied state, append the payload, then publish the copied state only after the record commit succeeds. Resolve a timing source only from `Impl::records`; require an earlier exact `RecordType::source_bytes` record whose stream, sequence, and hash match the supplied `RecordInfo`.

- [ ] **Step 4: Run the unit suite to verify GREEN**

Run:

```bash
cmake --build build --parallel
./build/codec_tests
```

Expected: all writer validation and existing unit tests pass.

- [ ] **Step 5: Commit writer validation**

```bash
git add src/core/internal.hpp src/core/stream_continuity.cpp \
  src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: validate stream continuity writes"
```

### Task 3: Validate archive reads, corruption isolation, and repair

**Files:**
- Modify: `src/archive/archive.cpp`
- Test: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: Task 2 `validate_and_advance` functions and the record list returned by `CodaArchive::records`.
- Produces: deterministic semantic validation inside `CodaArchive::continuity()` without changing structural `verify()` or byte extraction.

- [ ] **Step 1: Write failing hash-valid invalid-metadata tests**

Use `append_raw` with hand-built `STM1`/`SGP1` byte fixtures so the archive chain remains structurally valid. Prove `verify().ok` and exact source extraction remain true while `continuity()` returns `archive_corrupt` for:

```text
malformed payload/magic/version/reserved/size
missing, later, wrong-stream, non-source, or wrong-hash source reference
invalid timebase
unannounced sequence jump
epoch regression
within-connection monotonic regression
zero or overflowing gap
```

- [ ] **Step 2: Write the failing repair round-trip test**

Repair an archive containing valid timing and gap metadata plus a torn tail. Assert the repaired archive preserves exact source bytes, timing/gap fields, source record sequence/hash bindings, and a valid continuity ledger.

- [ ] **Step 3: Run the unit suite to verify RED**

Run:

```bash
cmake --build build --parallel
./build/codec_tests
```

Expected: invalid semantic metadata is currently returned as data instead of `archive_corrupt`, or repaired continuity is not yet proven.

- [ ] **Step 4: Implement reader validation**

In archive order, decode each continuity payload, resolve timing links only to earlier records, validate the exact source record sequence/hash/stream/type, then advance a per-stream ledger using Task 2 validators. Map invalid persisted metadata to `ErrorCode::archive_corrupt`; do not alter `verify()`, `records()`, `read_payload()`, or extraction behavior.

- [ ] **Step 5: Run the unit and C ABI suites to verify GREEN**

Run:

```bash
cmake --build build --parallel
./build/codec_tests
./build/codec_c_api_test
```

Expected: all new failure-path/repair tests and all compatibility tests pass.

- [ ] **Step 6: Commit reader validation**

```bash
git add src/archive/archive.cpp tests/test_archive.cpp
git commit -m "test: prove stream continuity validation"
```

### Task 4: Synchronize capability claims and verify the exact head

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: the proven Task 1–3 behavior.
- Produces: truthful current-status text limited to generic S0 continuity in the provisional development profile.

- [ ] **Step 1: Update documentation after behavior is green**

Move persisted generic clock/epoch, sequence/gap, and exact source-record integrity-link semantics from `planned_not_implemented` to `implemented_v0_1.generic`. Leave provenance envelopes, generic engine/API migration, S1/D behavior, profiles, models, scale, deployment, and frozen CODA v1 explicitly planned/unimplemented. Add the same bounded statement to `CHANGELOG.md`.

- [ ] **Step 2: Run Release CI-equivalent verification**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/codec capabilities
```

Expected: configure/build pass with warnings as errors; 4/4 CTest targets pass; neural separation and GPU inference remain explicitly unavailable.

- [ ] **Step 3: Run sanitizer verification**

```bash
cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug \
  -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

Expected: configure/build and 4/4 tests pass. If this runner blocks LeakSanitizer `/proc` discovery, retain ASan/UBSan and rerun tests with `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`, recording the limitation exactly; GitHub CI must retain its normal leak-enabled job.

- [ ] **Step 4: Verify package compatibility**

Install to a temporary prefix and compile a standalone CMake consumer that includes `<codec/archive.hpp>`, writes a telemetry source record plus timing/gap metadata, reopens it, and validates `continuity()` through `find_package(codec 0.1 CONFIG REQUIRED)`.

- [ ] **Step 5: Audit and commit documentation**

```bash
git diff --check
git status --short
git diff --stat
git add README.md CHANGELOG.md
git commit -m "docs: record generic stream continuity"
```

Expected: only the intended public headers, implementation, tests, CMake source registration, README, changelog, and this plan changed; no generated build artifacts are tracked.

- [ ] **Step 6: Record the exact tested SHA**

Run `git rev-parse HEAD`, rerun `git status --short`, and use that immutable SHA for the PR, CI, roadmap issue completion record, and merge gate. If the head moves, repeat all applicable verification.
