# Stage B.5 Generic Provenance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Persist and validate declared S1/D provenance with exact backward-only record links in the provisional CODA development profile.

**Architecture:** Add a versioned `SPV1` sidecar record after each declared S1/D subject. The sidecar identifies its subject and one or more earlier inputs by stream, 16-bit type code, archive sequence, and SHA-256; generic process metadata is explicit while profile-specific assessment data remains typed opaque bytes.

**Tech Stack:** C++20, CODA append-only archive, CMake 3.20+, OpenSSL, repository test harness, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-27-generic-provenance-design.md`

## Global Constraints

- Keep policy/authorization tags, trust decisions, selective disclosure, interval queries, generic processing/query APIs, and C ABI additions out of scope.
- Do not change the 64-byte CODA header, 96-byte record envelope, 40-byte commit trailer, archive format version, or existing encodings.
- Subjects are explicitly declared S1 or D; do not infer truth classes or claim provenance completeness for arbitrary legacy/raw records.
- Preserve streaming append, unknown record types, structural verification, exact S0 extraction, and non-mutating repair.
- Malformed provenance may fail only the provenance view; it must not block committed S0 evidence.
- Bound inputs to 1–4096, metadata strings to 4096 bytes each, and opaque details to 1 MiB.
- Use exact record-level links only. Interval links remain deferred to Stage C query/extraction work.

## File Structure

- `include/codec/archive.hpp`: public provenance types, record code, writer API, and reader API.
- `src/core/stream_provenance.cpp`: `SPV1` encoding, decoding, resource validation, and link construction helpers.
- `src/core/internal.hpp`: internal provenance codec declarations and validation constants.
- `src/archive/archive.cpp`: writer exact-link checks, duplicate-subject state, and archive semantic validation.
- `tests/test_archive.cpp`: public round trips, writer rejection, raw corruption fixtures, isolation, legacy, and repair proof.
- `CMakeLists.txt`: compile the focused provenance codec source.
- `README.md` and `CHANGELOG.md`: capability statement limited to declared S1/D provenance.

---

### Task 1: Public model and happy-path persistence

**Files:**
- Modify: `include/codec/archive.hpp`
- Modify: `src/core/internal.hpp`
- Create: `src/core/stream_provenance.cpp`
- Modify: `src/archive/archive.cpp`
- Modify: `tests/test_archive.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `RecordInfo`, `TruthClass`, `Sha256`, `CodaWriter::append_raw()`, `CodaArchive::records()`, and `CodaArchive::read_payload()`.
- Produces: `ProvenanceRecordLink`, `ProvenanceProcess`, `StreamProvenance`, `RecordType::stream_provenance`, `CodaWriter::append_stream_provenance()`, `CodaArchive::provenance()`, and internal `encode_stream_provenance()` / `decode_stream_provenance()`.

- [ ] **Step 1: Write the failing public round-trip test**

Add `generic_stream_provenance_round_trips_s1_and_multi_input_d` to `tests/test_archive.cpp`. Its core setup is:

```cpp
const auto source_stream = stream_id(120);
const auto output_stream = stream_id(121);
auto writer = std::move(*codec::CodaWriter::create(path));
auto source = writer.append(codec::RecordType::source_bytes, source_stream,
                            10, 20, bytes("source evidence"));
auto normalized = writer.append_raw(0x7001, output_stream, 10, 20,
                                    bytes("canonical state"));
const codec::ProvenanceProcess normalizer{
    .operation = "profile.normalize",
    .implementation_id = "example.telemetry",
    .implementation_version = "1.0.0",
    .implementation_hash = codec::sha256(bytes("normalizer binary")),
    .configuration_hash = codec::sha256(bytes("canonical config")),
    .created_utc_ns = 1'725'000'000'000'000'000LL,
    .details_type = "application/cbor",
    .details = bytes("quality=exact"),
};
const std::array normalized_inputs{*source};
EXPECT_TRUE(writer.append_stream_provenance(
    *normalized, codec::TruthClass::state_exact, normalized_inputs,
    normalizer));

auto derived = writer.append_raw(0x7002, output_stream, 10, 20,
                                 bytes("derived assessment"));
const std::array derived_inputs{*source, *normalized};
EXPECT_TRUE(writer.append_stream_provenance(
    *derived, codec::TruthClass::derived, derived_inputs,
    codec::ProvenanceProcess{
        .operation = "model.infer",
        .implementation_id = "example.model-runtime",
        .implementation_version = "2.1.0",
        .created_utc_ns = 1'725'000'000'000'000'100LL,
    }));
```

Finalize, reopen, call `provenance()`, and compare both subject truth classes,
all subject/input stream/type/sequence/hash fields, every process string, both
optional hashes, timestamp, details type, and details bytes against literals
and the committed `RecordInfo` values.

- [ ] **Step 2: Run the test binary and verify RED**

Run:

```bash
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-baseline --parallel
```

Expected: compilation fails only because the provenance record code, public
types, and append/read methods do not exist.

- [ ] **Step 3: Add the public API and internal codec boundary**

In `include/codec/archive.hpp`, add `stream_provenance = 7`, the types from the
spec, and these exact declarations:

```cpp
Result<RecordInfo> append_stream_provenance(
    const RecordInfo& subject, TruthClass subject_truth,
    std::span<const RecordInfo> inputs, const ProvenanceProcess& process);

Result<std::vector<StreamProvenance>> provenance(
    ArchiveReadPolicy policy = ArchiveReadPolicy::complete_archive) const;
```

Include `<optional>`. In `src/core/internal.hpp`, declare:

```cpp
Result<std::vector<std::byte>> encode_stream_provenance(
    const StreamProvenance& provenance);
Result<StreamProvenance> decode_stream_provenance(
    std::span<const std::byte> payload);
```

Add `src/core/stream_provenance.cpp` to `codec_core` in `CMakeLists.txt`.

- [ ] **Step 4: Implement deterministic `SPV1` encoding/decoding**

In `src/core/stream_provenance.cpp`, use little-endian helpers and these fixed
constants:

```cpp
constexpr std::size_t link_size = 60;
constexpr std::size_t fixed_header_size = 56;
constexpr std::uint32_t has_implementation_hash = 1U << 0U;
constexpr std::uint32_t has_configuration_hash = 1U << 1U;
constexpr std::uint32_t known_flags =
    has_implementation_hash | has_configuration_hash;
constexpr std::uint32_t maximum_inputs = 4096;
constexpr std::uint32_t maximum_text_size = 4096;
constexpr std::uint32_t maximum_details_size = 1024 * 1024;
```

Lay out the fixed header as magic `[0,4)`, version `u16`, reserved `u16`, truth
class `u8`, three reserved bytes, flags `u32`, input count `u32`, five `u32`
variable lengths, creation time `i64`, and a final reserved `u64`; then encode
the subject link, optional hashes, input links, and variable fields in that
order. A link is stream ID (16), type code (2), reserved (2), sequence (8), and
hash (32). Decode only when exact calculated size equals payload size.

Validate required nonempty process fields, NUL-free text, paired details
type/bytes, input count, truth class, known flags, reserved zeros, and checked
size arithmetic. Encoding errors use `invalid_argument`; decoding errors use
`archive_corrupt`.

- [ ] **Step 5: Implement the minimum typed writer and reader path**

In `CodaWriter::append_stream_provenance()`, resolve the subject and each input
against `impl_->records` by all four exact link fields, require S1/D, require
inputs before the subject, build `StreamProvenance`, encode it, and append it on
the subject stream using the subject start/end times.

In `CodaArchive::provenance()`, decode every provenance payload and resolve the
subject/inputs against the scanned record list. Map semantic mismatches to
`archive_corrupt`. Keep duplicate-subject and prohibited-type hardening for the
next tasks; implement only what the happy path needs plus exact-link safety.

- [ ] **Step 6: Run the targeted unit binary and verify GREEN**

Run:

```bash
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-baseline --parallel
./build-baseline/codec_tests
```

Expected: the new round trip and every existing unit test pass.

- [ ] **Step 7: Commit the happy path**

```bash
git add CMakeLists.txt include/codec/archive.hpp src/core/internal.hpp \
  src/core/stream_provenance.cpp src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: persist generic stream provenance"
```

---

### Task 2: Writer rejection and duplicate-subject guarantees

**Files:**
- Modify: `src/archive/archive.cpp`
- Modify: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: Task 1 public append API and exact-link codec.
- Produces: deterministic pre-append rejection and per-writer provenance subject uniqueness.

- [ ] **Step 1: Add focused failing writer tests**

Add tests named:

```text
writer_rejects_uncommitted_or_forged_provenance_links
writer_rejects_invalid_provenance_truth_and_order
writer_rejects_duplicate_inputs_and_subject_provenance
writer_rejects_provenance_metadata_record_links
writer_rejects_invalid_provenance_process_fields
```

Build real records with `append()` / `append_raw()`, then mutate one
`RecordInfo` field at a time for absent sequence, wrong stream, wrong type code,
and wrong hash. Prove `source_exact`, empty inputs, an input appended after the
subject, repeated input sequence, a second sidecar for the same subject,
provenance/final-index links, empty required strings, embedded NUL, mismatched
details type/bytes, and over-limit text/details all return `invalid_argument`.

- [ ] **Step 2: Run the unit binary and verify RED**

Run `./build-baseline/codec_tests`.

Expected: the exact-link cases already pass; duplicate, prohibited-type, and
process-boundary cases fail because the extra writer validation is absent.

- [ ] **Step 3: Add writer state and complete validation**

Add a subject-sequence set to `CodaWriter::Impl`:

```cpp
std::set<std::uint64_t> provenance_subjects;
```

Reject `stream_provenance` and `final_index` as subjects or inputs, reject
duplicate input sequences, and reject a subject already present in the set.
Insert the subject only after `append()` succeeds. Reuse the codec's process
validation; do not duplicate string-size logic in `archive.cpp`.

- [ ] **Step 4: Rebuild and verify GREEN**

Run:

```bash
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-baseline --parallel
./build-baseline/codec_tests
```

Expected: every writer rejection test and all existing tests pass.

- [ ] **Step 5: Commit writer validation**

```bash
git add src/archive/archive.cpp tests/test_archive.cpp
git commit -m "feat: validate provenance writes"
```

---

### Task 3: Reader corruption isolation and semantic graph validation

**Files:**
- Modify: `src/archive/archive.cpp`
- Modify: `tests/test_archive.cpp`

**Interfaces:**
- Consumes: `SPV1` decoder and public provenance reader from Task 1.
- Produces: complete reader-side validation with isolated `archive_corrupt` failures.

- [ ] **Step 1: Add raw fixture helpers and malformed-payload tests**

Add a test-only `provenance_payload()` encoder mirroring the documented SPV1
layout, plus `expect_invalid_provenance_metadata()` modeled on the continuity
helper. The helper must append valid S0 first, inject a raw provenance record,
finalize, assert `verify().ok`, assert `provenance()` fails with
`archive_corrupt`, and assert exact source extraction still succeeds.

Exercise wrong magic, version, truth class, unknown flag, nonzero reserved
fields, zero/oversized input count, invalid variable lengths, truncated link,
trailing byte, embedded NUL, and mismatched details pairing.

- [ ] **Step 2: Add semantic-link and graph tests**

Use hash-valid raw fixtures to prove rejection of missing/later/wrong-stream/
wrong-type/wrong-hash subjects and inputs, input sequence not preceding the
subject, duplicate inputs, provenance/final-index links, sidecar envelope stream
not matching the subject, and two sidecars for one subject.

- [ ] **Step 3: Run the unit binary and verify RED**

Run `./build-baseline/codec_tests`.

Expected: codec-level malformed cases pass where already decoded defensively;
semantic duplicate/prohibited/envelope cases fail until the reader validator is
completed.

- [ ] **Step 4: Complete archive reader validation**

In `CodaArchive::provenance()`, maintain a set of subject sequences and a set of
input sequences per sidecar. Resolve every link against the exact scanned
record. Enforce source-before-subject-before-sidecar order, subject-envelope
stream equality, prohibited record types, uniqueness, S1/D truth class, and
codec field validity. Return `archive_corrupt` with a specific message at the
first failure.

Do not call `provenance()` from `verify()`, extraction, descriptors, feeds,
continuity, or repair.

- [ ] **Step 5: Rebuild and verify GREEN**

Run:

```bash
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-baseline --parallel
./build-baseline/codec_tests
```

Expected: all malformed/semantic isolation tests and the full unit binary pass.

- [ ] **Step 6: Commit reader proof**

```bash
git add src/archive/archive.cpp tests/test_archive.cpp
git commit -m "test: prove provenance graph validation"
```

---

### Task 4: Repair, compatibility, documentation, and package proof

**Files:**
- Modify: `tests/test_archive.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: the complete public API and validated SPV1 persistence.
- Produces: repair/legacy evidence and truthful public capability text.

- [ ] **Step 1: Add failing repair and legacy tests**

Add `repair_preserves_valid_stream_provenance_links` that writes source, S1,
S1 provenance, D, D provenance, and a torn tail. Repair the valid prefix and
compare every repaired provenance subject/input link and process field with the
original values. Assert S0 bytes remain exact.

Add `legacy_archive_has_empty_provenance_view` using an archive containing only
source bytes and a final index; assert `provenance()` succeeds with an empty
vector.

- [ ] **Step 2: Run the unit binary and verify RED or existing compatibility**

Run `./build-baseline/codec_tests`.

Expected: legacy passes immediately; repair fails if any recreated record hash
or provenance link differs, otherwise it confirms the existing raw repair path
already satisfies the new invariant.

- [ ] **Step 3: Make the smallest repair correction if the proof fails**

If the repair test exposes a difference, preserve record order, type code,
stream, envelope start/end, flags, and payload when recreating the valid prefix.
Do not rewrite SPV1 payloads or broaden recovery semantics. If the test already
passes, make no production repair change.

- [ ] **Step 4: Update truthful status text**

In README `implemented_v0_1.generic`, add one item stating that the provisional
profile persists versioned declared S1/D provenance with exact backward record
links and typed opaque profile details. Remove persisted generic provenance
from `planned_not_implemented`; leave policy tags and all later stages planned.

Add one `Unreleased` CHANGELOG bullet with the same bounded claim and explicit
no-envelope/no-C-ABI change.

- [ ] **Step 5: Run Release, sanitizer, capability, and install proof**

Run:

```bash
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake -S . -B build-stage-b5 -G Ninja -DCMAKE_MAKE_PROGRAM=/tmp/codec-build-tools.Bt4Ijl/bin/ninja -DCMAKE_BUILD_TYPE=Release -DCODEC_WARNINGS_AS_ERRORS=ON
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-stage-b5 --parallel
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/ctest --test-dir build-stage-b5 --output-on-failure
./build-stage-b5/codec capabilities
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake -S . -B build-stage-b5-san -G Ninja -DCMAKE_MAKE_PROGRAM=/tmp/codec-build-tools.Bt4Ijl/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DCODEC_WARNINGS_AS_ERRORS=ON -DCODEC_ENABLE_SANITIZERS=ON
/tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/cmake --build build-stage-b5-san --parallel
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 /tmp/codec-build-tools.Bt4Ijl/lib/python3.12/site-packages/cmake/data/bin/ctest --test-dir build-stage-b5-san --output-on-failure
```

Install to a `mktemp -d` prefix. Build a standalone CMake consumer that uses
`find_package(codec 0.1 CONFIG REQUIRED)`, appends S0 and a derived unknown-type
record, attaches provenance, reopens it, and validates the exact links through
the installed headers/library.

- [ ] **Step 6: Audit the diff and commit documentation/proof**

Run `git diff --check`, inspect `git diff --stat`, and verify only planned files
changed. Then:

```bash
git add README.md CHANGELOG.md tests/test_archive.cpp
git commit -m "docs: record generic stream provenance"
```

---

### Task 5: Exact-head publication evidence

**Files:**
- No repository file changes expected.

**Interfaces:**
- Consumes: the verified branch head and repository roadmap issue #10.
- Produces: exact-head CI and canonical completion evidence on `main`.

- [ ] **Step 1: Run fresh pre-publication verification**

Re-run Release CTest on the exact branch head, record `git rev-parse HEAD`, and
confirm `git status --porcelain` is empty.

- [ ] **Step 2: Publish without overwriting concurrent work**

Re-fetch `main`, require it still equals the recorded base or rebase/reverify,
publish the exact branch tree through an authenticated GitHub path, and update
`main` only as a non-force fast-forward or reviewed exact-head merge.

- [ ] **Step 3: Require exact-head GitHub CI**

Wait for GCC, Clang, and sanitizers on the published SHA. Every required job
must complete successfully; if the head moves, restart exact-head verification.

- [ ] **Step 4: Record roadmap completion**

Add a Stage B.5 completion comment to issue #10 with base/head SHAs, BEFORE /
AFTER, truth classes, exact test commands/results, install consumer, CI URLs,
risks, remaining Stage B evidence, and the next dependency. Do not claim policy
tags, query APIs, profile interpretation, model quality, scale, or frozen CODA
v1.
