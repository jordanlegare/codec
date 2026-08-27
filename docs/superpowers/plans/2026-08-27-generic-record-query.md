# Generic Record Query and Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic payload-agnostic C++ record query and boundary-preserving extraction APIs over authenticated CODA envelope fields.

**Architecture:** Add small aggregate query/result types to the existing archive header. Implement one private predicate in `archive.cpp`; `query_records()` filters `records(policy)` in archive order, `extract_records()` verifies each selected payload, and legacy concatenating extraction delegates to the new boundary.

**Tech Stack:** C++20, existing CODA archive scanner/hash verification, CMake, custom unit harness, CTest, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-27-generic-record-query-design.md`

## Global Constraints

- Keep the CODA development-profile bytes, record registry, C ABI, and CLI unchanged.
- Combine only authenticated envelope fields: stream, raw type, archive sequence, and envelope time.
- Use half-open ranges and AND semantics; preserve archive order.
- Preserve exact per-record boundaries and hash verification during extraction.
- Do not parse provenance, continuity, descriptor, or profile payloads.
- Make invalid or empty ranges return `ErrorCode::invalid_argument` before scanning.
- Make no truth-class, semantic-clock, index, performance, scale, or frozen-v1 claim.

---

### Task 1: Document and freeze the Stage C.1 contract

**Files:**
- Create: `docs/superpowers/specs/2026-08-27-generic-record-query-design.md`
- Create: `docs/superpowers/plans/2026-08-27-generic-record-query.md`

**Interfaces:**
- Consumes: Stage B.5 `RecordInfo`, `RecordTypeCode`, `ArchiveReadPolicy`, and exact payload verification.
- Produces: approved signatures and half-open matching semantics for Tasks 2–4.

- [ ] **Step 1: Cross-check the spec against current public types**

Run:

```bash
rg -n "RecordInfo|RecordTypeCode|ArchiveReadPolicy|records\(|read_payload|extract_stream_raw" include/codec/archive.hpp src/archive/archive.cpp tests/test_archive.cpp
```

Expected: all consumed APIs exist on base `e541a703` and there is no generic query type/method.

- [ ] **Step 2: Scan the plan for placeholders and inconsistent names**

Run:

```bash
rg -n "T[B]D|T[O]DO|implement l[a]ter|appropriate e[r]ror|similar t[o]" docs/superpowers/specs/2026-08-27-generic-record-query-design.md docs/superpowers/plans/2026-08-27-generic-record-query.md
rg -n "RecordSequenceRange|RecordTimeRange|RecordQuery|ExtractedRecord|query_records|extract_records" docs/superpowers/specs/2026-08-27-generic-record-query-design.md docs/superpowers/plans/2026-08-27-generic-record-query.md
```

Expected: the placeholder scan is empty; every planned public name is consistent.

- [ ] **Step 3: Commit the approved design and plan**

```bash
git add docs/superpowers/specs/2026-08-27-generic-record-query-design.md docs/superpowers/plans/2026-08-27-generic-record-query.md
git commit -m "docs: design generic record queries"
```

### Task 2: Add deterministic physical-record selection

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/archive/archive.cpp`
- Modify: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: `CodaArchive::records(ArchiveReadPolicy)` and authenticated `RecordInfo` fields.
- Produces: `RecordSequenceRange`, `RecordTimeRange`, `RecordQuery`, and `CodaArchive::query_records()`.

- [ ] **Step 1: Write the failing combined-filter test**

Add a test that appends records for two streams and two raw types, including
intervals `[0,10)`, `[10,20)`, `[12,18)`, plus point records at 10 and 20.
Query with:

```cpp
const codec::RecordQuery query{
    .stream = selected_stream,
    .type = codec::RecordTypeCode{0x7400},
    .sequence = codec::RecordSequenceRange{.begin = 1, .end = 6},
    .time = codec::RecordTimeRange{.begin_ns = 10, .end_ns = 20},
};
```

Assert only records satisfying every filter appear, in sequence order. Add
focused time queries proving an interval ending at 10 is excluded, a point at
10 is included, and a point at 20 is excluded.

- [ ] **Step 2: Run the unit target to verify RED**

```bash
cmake -S . -B build-stage-c1 -G Ninja -DCMAKE_MAKE_PROGRAM=/tmp/codec-stage-tools/bin/ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-stage-c1 --target codec_tests --parallel
```

Expected: compilation fails only because `RecordQuery`, range types, and
`query_records()` do not exist.

- [ ] **Step 3: Add the public query types and method**

Add the exact aggregates from the spec after `RecordInfo` and declare:

```cpp
Result<std::vector<RecordInfo>> query_records(
    const RecordQuery& query,
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
```

Implement helpers in the anonymous namespace:

```cpp
Result<void> validate_record_query(const RecordQuery& query) {
  if (query.sequence && query.sequence->begin >= query.sequence->end) {
    return fail(ErrorCode::invalid_argument,
                "record query sequence range must be non-empty");
  }
  if (query.time && query.time->begin_ns >= query.time->end_ns) {
    return fail(ErrorCode::invalid_argument,
                "record query time range must be non-empty");
  }
  return {};
}

bool matches_record_query(const RecordInfo& record, const RecordQuery& query) {
  if (query.stream && record.stream != *query.stream) return false;
  if (query.type && record.type_code() != *query.type) return false;
  if (query.sequence &&
      (record.sequence < query.sequence->begin ||
       record.sequence >= query.sequence->end)) return false;
  if (query.time) {
    const auto point = record.start_ns == record.end_ns;
    const auto matches = point
        ? query.time->begin_ns <= record.start_ns &&
              record.start_ns < query.time->end_ns
        : record.start_ns < query.time->end_ns &&
              query.time->begin_ns < record.end_ns;
    if (!matches) return false;
  }
  return true;
}
```

`query_records()` validates first, calls `records(policy)`, and copies matching
records in existing order.

- [ ] **Step 4: Build and run the unit binary to verify GREEN**

```bash
cmake --build build-stage-c1 --target codec_tests --parallel
./build-stage-c1/codec_tests
```

Expected: every unit case passes.

- [ ] **Step 5: Commit physical record selection**

```bash
git add include/codec/archive.hpp src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: query generic archive records"
```

### Task 3: Prove invalid ranges and verified-prefix behavior

**Files:**
- Modify: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: `CodaArchive::query_records(const RecordQuery&, ArchiveReadPolicy)`.
- Produces: failure and torn-tail compatibility proof for the query boundary.

- [ ] **Step 1: Write invalid-range assertions**

Assert these four queries return `ErrorCode::invalid_argument`:

```cpp
RecordSequenceRange{.begin = 2, .end = 2};
RecordSequenceRange{.begin = 3, .end = 2};
RecordTimeRange{.begin_ns = 20, .end_ns = 20};
RecordTimeRange{.begin_ns = 21, .end_ns = 20};
```

- [ ] **Step 2: Prove verified-prefix selection**

Create two committed records, truncate the second payload, then assert:

```cpp
EXPECT_FALSE(archive.query_records(query));
auto prefix = archive.query_records(
    query, codec::ArchiveReadPolicy::verified_prefix);
EXPECT_TRUE(prefix);
EXPECT_EQ(prefix->size(), std::size_t{1});
EXPECT_EQ(prefix->front().hash, first->hash);
```

- [ ] **Step 3: Temporarily bypass validation and verify RED**

Temporarily make `validate_record_query()` return success. Rebuild and run the
invalid-range test; it must fail because the empty/inverted ranges are
accepted. Restore validation immediately.

- [ ] **Step 4: Run the unit binary to verify GREEN**

```bash
cmake --build build-stage-c1 --target codec_tests --parallel
./build-stage-c1/codec_tests
```

Expected: every unit case passes, including invalid ranges and verified prefix.

- [ ] **Step 5: Commit failure/prefix proof**

```bash
git add tests/test_archive.cpp
git commit -m "test: prove record query boundaries"
```

### Task 4: Add boundary-preserving exact extraction

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/archive/archive.cpp`
- Modify: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: `query_records()`, `read_payload()`, `RecordInfo`, and `RecordQuery`.
- Produces: `ExtractedRecord`, `extract_records()`, and legacy raw extraction delegation.

- [ ] **Step 1: Write the failing extraction test**

Append two matching unknown-type records with distinct payloads. Query them and
assert `extract_records()` returns two elements—not one concatenated buffer—and
that each element retains the exact record stream, raw type, sequence, hash,
and payload bytes. Add the torn-tail `verified_prefix` extraction assertion.

- [ ] **Step 2: Build to verify RED**

```bash
cmake --build build-stage-c1 --target codec_tests --parallel
```

Expected: compilation fails only because `ExtractedRecord` and
`extract_records()` do not exist.

- [ ] **Step 3: Implement exact extraction**

Add the public aggregate and method from the spec. Implement:

```cpp
Result<std::vector<ExtractedRecord>> CodaArchive::extract_records(
    const RecordQuery& query, ArchiveReadPolicy policy) const {
  auto selected = query_records(query, policy);
  if (!selected) return selected.error();
  std::vector<ExtractedRecord> output;
  output.reserve(selected->size());
  for (const auto& record : *selected) {
    auto payload = read_payload(record);
    if (!payload) return payload.error();
    output.push_back(ExtractedRecord{record, std::move(*payload)});
  }
  return output;
}
```

Refactor `extract_stream_raw()` to call `extract_records()` with exact stream
and raw type filters, then concatenate the returned payloads with the existing
overflow check.

- [ ] **Step 4: Build and run the unit binary to verify GREEN**

```bash
cmake --build build-stage-c1 --target codec_tests --parallel
./build-stage-c1/codec_tests
```

Expected: every unit case passes and legacy extraction behavior is unchanged.

- [ ] **Step 5: Commit exact extraction**

```bash
git add include/codec/archive.hpp src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: extract generic archive records"
```

### Task 5: Publish truthful status and verify the package boundary

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Create ignored verification consumer under: `build-stage-c1-consumer-src/`

**Interfaces:**
- Consumes: the completed public C++ query/extraction API.
- Produces: truthful status/compatibility evidence; no shipped consumer source.

- [ ] **Step 1: Update implementation status**

Add an implemented README bullet for AND-combined physical record query and
boundary-preserving exact extraction. Keep truth/provenance traversal,
adapters, processors, profile exporters, and generic engine migration planned.
Add one Unreleased changelog entry with the same bounded claim.

- [ ] **Step 2: Run complete Release verification**

```bash
cmake -S . -B build-stage-c1 -G Ninja -DCMAKE_MAKE_PROGRAM=/tmp/codec-stage-tools/bin/ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
cmake --build build-stage-c1 --parallel
ctest --test-dir build-stage-c1 --output-on-failure
./build-stage-c1/codec capabilities
```

Expected: 4/4 CTest targets pass; capabilities remain truthful and unchanged.

- [ ] **Step 3: Run complete sanitizer verification**

```bash
cmake -S . -B build-stage-c1-san -G Ninja -DCMAKE_MAKE_PROGRAM=/tmp/codec-stage-tools/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
cmake --build build-stage-c1-san --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-stage-c1-san --output-on-failure
```

Expected: 4/4 sanitizer CTest targets pass without ASan/UBSan findings.

- [ ] **Step 4: Verify the installed package**

Install to an ignored prefix and configure a standalone CMake consumer against
`find_package(codec 0.1 CONFIG REQUIRED)`. The consumer must append an unknown
non-audio record, query it by stream/type/time/sequence, extract it, and assert
the exact payload and record hash. Build and run it successfully.

- [ ] **Step 5: Audit and commit status**

```bash
git diff --check
git status --short
git diff --stat main...HEAD
git add README.md CHANGELOG.md
git commit -m "docs: record generic query boundary"
```

- [ ] **Step 6: Re-run exact-HEAD verification and publish**

Re-run Release CTest, sanitizer CTest, and the installed consumer on the exact
commit. Confirm remote `main` is still `e541a703`, create a non-forced
fast-forward commit whose tree equals local `HEAD^{tree}`, wait for GCC, Clang,
and sanitizer GitHub checks on that exact commit, and record completion on
issue #10. If the remote ref moves, stop publication and rebase/reverify.
