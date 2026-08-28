# Stage D.3 Provenance-Verified PCM16 State Reader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded Audio Profile query that returns decoded APS1 PCM16 state only when finalized-archive verification and exact state-exact S0 lineage validate.

**Architecture:** Introduce one profile-only header and one audio implementation file. The implementation composes existing generic CODA verification, record/provenance queries, exact payload reads, and the D.1 APS1 decoder; it does not change generic archive semantics or add another normalization path.

**Tech Stack:** C++20, existing CODEC `Result`, CODA C++ APIs, CMake/CTest, GitHub Actions GCC/Clang/ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-d3-verified-pcm16-reader-design.md`

## Global Constraints

- Start from `main` SHA `9e00e3d19fd9f1487d410e8e20f4805154bb0ca0`.
- Work only on `automation/stage-d3-verified-pcm16-reader` until exact-head verification is complete.
- No production code before a failing D.3 test is observed in CI.
- Require finalized archives; no verified-prefix S1 classification.
- Preserve D.1 APS1 as the only PCM16 S1 state and D.2 as the only ingest path.
- Do not change CODA header/envelope/version/record types, generic archive API signatures, root audio ABI, CLI, or C ABI.
- Keep neural/GPU capability output unchanged and false.
- No FLAC, conversion, resampling, inference, identity fusion, scale, deployment, or Stage D completion claim.

---

### Task 1: Establish the RED trusted-state query contract

**Files:**
- Modify: `tests/test_audio_profile.cpp`

**Interfaces:**
- Consumes: existing D.1 `Pcm16State`, APS1 encode/decode, D.2 ingest helpers, `CodaArchive`, `RecordInfo`, `StreamProvenance`.
- Produces test expectations for:
  - `codec::profiles::audio::Pcm16StateQuery`
  - `codec::profiles::audio::VerifiedPcm16State`
  - `codec::profiles::audio::query_verified_pcm16_states(const CodaArchive&, const Pcm16StateQuery&)`

- [ ] **Step 1: Add compile/runtime tests before the new API exists**

Add focused tests to `tests/test_audio_profile.cpp` that call the profile facade and require:

```cpp
codec::profiles::audio::Pcm16StateQuery query{
    .stream = stream,
    .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
    .maximum_results = 8,
    .maximum_encoded_bytes = 1024 * 1024,
};
auto states = codec::profiles::audio::query_verified_pcm16_states(archive, query);
EXPECT_TRUE(states);
EXPECT_EQ(states->size(), std::size_t{1});
EXPECT_EQ(states->front().state.sample_rate, std::uint32_t{48000});
EXPECT_EQ(states->front().state.channels, std::uint16_t{2});
EXPECT_EQ(states->front().state_record.type, codec::RecordType::pcm16);
EXPECT_EQ(states->front().source_record.type, codec::RecordType::source_bytes);
EXPECT_EQ(states->front().provenance.subject_truth,
          codec::TruthClass::state_exact);
```

Also add tests for unprovenanced PCM16 filtering, wrong input type/stream/interval, malformed APS1, unfinalized archive, zero limits, result bound, and encoded-byte bound. Use real archives and public writer APIs rather than mocks.

- [ ] **Step 2: Publish the tests-only branch head and verify RED**

Open a draft PR from `automation/stage-d3-verified-pcm16-reader` to `main` and let repository CI run on the tests-only head.

Expected: GCC/Clang/sanitizer builds fail because `Pcm16StateQuery`, `VerifiedPcm16State`, and/or `query_verified_pcm16_states` do not exist. The failure must be attributable to the missing D.3 API, not syntax or unrelated regressions.

- [ ] **Step 3: Record RED evidence**

Record the exact tests-only head SHA and failing workflow run in roadmap issue #10 and the PR body.

---

### Task 2: Add the profile-only verified state reader

**Files:**
- Create: `include/codec/profiles/audio_state_reader.hpp`
- Create: `src/audio/pcm16_state_reader.cpp`
- Modify: `include/codec/profiles/audio.hpp`
- Modify: `CMakeLists.txt`
- Test: `tests/test_audio_profile.cpp`

**Interfaces:**
- Consumes:
  - `CodaArchive::verify()`
  - `CodaArchive::records()`
  - `CodaArchive::query_provenance()`
  - `CodaArchive::read_payload()`
  - `decode_pcm16_state()`
- Produces:

```cpp
namespace codec::profiles::audio {

struct Pcm16StateQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{64ULL * 1024ULL * 1024ULL};
};

struct VerifiedPcm16State {
  Pcm16State state;
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedPcm16State>> query_verified_pcm16_states(
    const CodaArchive& archive, const Pcm16StateQuery& query = {});

}
```

- [ ] **Step 1: Add the public profile header and facade include**

Create `audio_state_reader.hpp` with the exact types/signature above and include it from `include/codec/profiles/audio.hpp`. Keep all new names in `codec::profiles::audio`; add no root alias.

- [ ] **Step 2: Add minimal validation and finalized-archive gate**

In `src/audio/pcm16_state_reader.cpp`:

```cpp
if (query.maximum_results == 0 || query.maximum_encoded_bytes == 0) {
  return fail<std::vector<VerifiedPcm16State>>(
      ErrorCode::invalid_argument, "PCM16 state query limits must be non-zero");
}
const auto verification = archive.verify();
if (!verification.ok) {
  return fail<std::vector<VerifiedPcm16State>>(
      verification.error_code, verification.message);
}
if (!verification.finalized) {
  return fail<std::vector<VerifiedPcm16State>>(
      ErrorCode::archive_corrupt,
      "verified PCM16 state query requires a finalized archive");
}
```

- [ ] **Step 3: Select state-exact PCM16 provenance through generic query APIs**

Build:

```cpp
const RecordQuery subject{
    .stream = query.stream,
    .type = record_type_code(RecordType::pcm16),
    .sequence = std::nullopt,
    .time = query.time,
};
const ProvenanceQuery provenance_query{
    .subject_truth = TruthClass::state_exact,
    .subject = subject,
    .direct_input = std::nullopt,
};
auto selected = archive.query_provenance(provenance_query);
```

Propagate generic query errors unchanged. If `selected->size()` exceeds `maximum_results`, return `resource_exhausted` before decoding state payloads.

- [ ] **Step 4: Resolve exact subject/source records and enforce D.2 lineage**

Fetch `archive.records()` once. For each selected provenance object, resolve a link only when all four physical identity fields match:

```cpp
candidate.stream == link.stream &&
candidate.type_code() == link.type &&
candidate.sequence == link.sequence &&
candidate.hash == link.hash
```

Fail with `archive_corrupt` unless:

```cpp
provenance.inputs.size() == 1;
state_record.type == RecordType::pcm16;
source_record.type == RecordType::source_bytes;
state_record.stream == source_record.stream;
state_record.start_ns == source_record.start_ns;
state_record.end_ns == source_record.end_ns;
```

Do not require one hard-coded process implementation ID/version. Preserve the full process metadata in the returned `StreamProvenance`.

- [ ] **Step 5: Enforce encoded-byte bounds before payload decode**

Accumulate `state_record.payload_size` using overflow-safe subtraction:

```cpp
if (state_record.payload_size > query.maximum_encoded_bytes - total_bytes) {
  return fail<std::vector<VerifiedPcm16State>>(
      ErrorCode::resource_exhausted,
      "verified PCM16 state payloads exceed the configured limit");
}
total_bytes += state_record.payload_size;
```

- [ ] **Step 6: Read exact APS1 and decode**

For each validated candidate:

```cpp
auto payload = archive.read_payload(state_record);
if (!payload) return payload.error();
auto state = decode_pcm16_state(*payload);
if (!state) return state.error();
output.push_back(VerifiedPcm16State{
    .state = std::move(*state),
    .state_record = state_record,
    .source_record = source_record,
    .provenance = provenance,
});
```

Preserve selected provenance order.

- [ ] **Step 7: Register the implementation in CMake and run GREEN CI**

Add `src/audio/pcm16_state_reader.cpp` to the existing `codec` library source list. Push the implementation head.

Expected: GCC and Clang build/install/tests pass; sanitizer tests pass; the D.3 success/failure tests pass; pre-existing tests remain green.

---

### Task 3: Synchronize claims and perform final exact-head verification

**Files:**
- Modify: `README.md`
- Modify: `CHANGELOG.md`
- Update: `docs/superpowers/specs/2026-08-28-stage-d3-verified-pcm16-reader-design.md` only if implementation reality required a wording correction
- Update: `docs/superpowers/plans/2026-08-28-stage-d3-verified-pcm16-reader.md` only to correct execution facts, not broaden scope

**Interfaces:**
- Consumes: completed D.3 API and test evidence.
- Produces: truthful public implementation status and merge evidence.

- [ ] **Step 1: Update README implemented status narrowly**

Add that the Audio Stream Profile implements a bounded finalized-archive PCM16 S1 query that returns APS1 only when exact `state_exact` source lineage validates. Keep FLAC, production adapters, neural runtime/models, streaming/offline inference, identity fusion, recovery, scale/deployment, and Stage D completion in planned/not-implemented status.

- [ ] **Step 2: Update CHANGELOG Unreleased**

Record the profile-only verified PCM16 reader/query and explicitly state that CODA layout, generic archive APIs, CLI/C ABI, and neural capability flags are unchanged.

- [ ] **Step 3: Audit the final diff**

Confirm the final branch changes only intended D.3 files, no temporary markers, no credentials/generated build artifacts, and the README References/AI contract section remains intact.

- [ ] **Step 4: Run exact-head CI**

Push the final documentation head and require a complete successful workflow on that exact SHA: GCC build/test/install, Clang build/test/install, and sanitizers.

- [ ] **Step 5: Review PR threads and exact head**

Require no unresolved review threads. Re-check that PR head SHA still equals the exact green CI SHA and that `main` is still at the expected base or otherwise incorporate only non-conflicting verified drift before integration.

- [ ] **Step 6: Integrate and verify main**

Use the repository's authorized non-force integration path. Never force-update `main`. After integration, verify the published `main` tree matches the exact tested feature tree and inspect the push-triggered main CI result.

- [ ] **Step 7: Record Stage D.3 completion in issue #10**

Record base, tested head/tree, published main SHA, CI run(s), delivered API, truth semantics, RED/GREEN evidence, scope/non-claims, and the next Stage D dependency. Do not declare Stage D complete.