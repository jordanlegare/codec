# Stage E.2 Loss and Recovery-Group Semantics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded generic sequence-loss observation and explicit recovery-group tracking above the existing E.1 CMX1 frame boundary without selecting or implementing a concrete repair algorithm.

**Architecture:** `SequenceLossObserver` tracks provisional sequence gaps independently per exact `(StreamId, StreamEpoch)` and can shrink/split them when late frames arrive. `RecoveryGroupTracker` tracks caller-declared same-stream/same-epoch source ranges, accepts out-of-order/duplicate observations, and freezes exact unresolved ranges on explicit seal. Neither component changes CMX1, CODA, truth classes, or transport I/O.

**Tech Stack:** C++20, existing `codec::MultiplexFrame` / `StreamId` / `StreamEpoch` / `Result`, STL containers, CMake/Ninja, custom CODEC test harness, GCC/Clang, ASan/UBSan.

**Spec:** `docs/superpowers/specs/2026-08-28-stage-e2-loss-recovery-semantics-design.md`

## Global Constraints

- No CMX1 wire-format/version change.
- No CODA layout/version change.
- No S0/S1/D classification change.
- Provisional sequence jumps are not permanent-loss claims.
- Sequence observation state is exact per `(StreamId, connection epoch, format epoch)`.
- Recovery groups never span an epoch and overlapping source ranges in one stream/epoch namespace are rejected.
- Sealing is the only final E.2 group-closure boundary.
- No FEC, parity, repair-symbol encoding, reconstruction, ARQ/retransmission, network I/O, persistence, CLI/C ABI, or scale claim.
- All memory/state growth is caller bounded.
- Existing global leak detection and the isolated ONNX Runtime suppression remain unchanged.

---

### Task 1: Establish the public E.2 contract in RED

**Files:**
- Create: `tests/test_transport_recovery.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: existing `<codec/transport.hpp>` and stream types.
- Produces: compile-time expectations for `<codec/recovery.hpp>`, `TransportSequenceRange`, `SequenceLossObserver`, `RecoveryGroupTracker`, and their supporting types.

- [ ] **Step 1: Create the tests-first source against the absent API**

Create `tests/test_transport_recovery.cpp`:

```cpp
#include "test.hpp"

#include <codec/recovery.hpp>
#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

codec::MultiplexFrame frame_for(std::string_view name,
                                std::uint64_t sequence,
                                std::uint64_t connection = 1,
                                std::uint64_t format = 1) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id(name);
  frame.sequence = sequence;
  frame.epoch = {.connection = connection, .format = format};
  frame.clock.source_timebase_numerator = 1;
  frame.clock.source_timebase_denominator = 1;
  frame.payload = {std::byte{0x42}};
  return frame;
}

}  // namespace

TEST(transport_recovery_sequence_observer_opens_and_fills_gap) {
  codec::SequenceLossObserver observer;

  auto baseline = observer.observe(frame_for("A", 7));
  EXPECT_TRUE(baseline);
  EXPECT_EQ(baseline->kind, codec::SequenceObservationKind::baseline);

  auto gap = observer.observe(frame_for("A", 10));
  EXPECT_TRUE(gap);
  EXPECT_EQ(gap->kind, codec::SequenceObservationKind::gap_opened);
  EXPECT_TRUE(gap->affected_range.has_value());
  EXPECT_EQ(gap->affected_range->begin, std::uint64_t{8});
  EXPECT_EQ(gap->affected_range->end, std::uint64_t{10});

  auto fill = observer.observe(frame_for("A", 8));
  EXPECT_TRUE(fill);
  EXPECT_EQ(fill->kind, codec::SequenceObservationKind::gap_filled);

  auto missing = observer.missing(frame_for("A", 0).stream,
                                  codec::StreamEpoch{1, 1});
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{1});
  EXPECT_EQ(missing->front().begin, std::uint64_t{9});
  EXPECT_EQ(missing->front().end, std::uint64_t{10});
}
```

Include `<string_view>` explicitly.

- [ ] **Step 2: Register only the new test source**

Add `tests/test_transport_recovery.cpp` to `codec_tests` in `CMakeLists.txt`. Do not add any E.2 production header/source yet.

- [ ] **Step 3: Open a draft PR so GitHub CI records RED**

Use the branch `automation/stage-e2-loss-recovery-semantics`, base `main`, title `Stage E.2: add loss observation and recovery-group semantics`, draft=true. The body must state that the current branch is intentionally RED and no production recovery API exists yet.

- [ ] **Step 4: Require clean RED**

Expected CI failure: `tests/test_transport_recovery.cpp` fails compilation solely because `<codec/recovery.hpp>` is absent. Existing production/test sources must configure and compile normally up to that point.

- [ ] **Step 5: Commit the RED contract**

The GitHub contents API write registering the test is itself the commit. Record the exact RED commit/run in the PR/plan execution notes.

---

### Task 2: Implement bounded provisional sequence-loss observation

**Files:**
- Create: `include/codec/recovery.hpp`
- Create: `src/transport/recovery.cpp`
- Modify: `tests/test_transport_recovery.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `MultiplexFrame`, `StreamId`, `StreamEpoch`, `Result`.
- Produces:

```cpp
struct TransportSequenceRange {
  std::uint64_t begin{};
  std::uint64_t end{};
};

enum class SequenceObservationKind : std::uint8_t {
  baseline = 1,
  in_order = 2,
  gap_opened = 3,
  gap_filled = 4,
  late_or_replayed = 5,
};

struct SequenceObservation {
  SequenceObservationKind kind{SequenceObservationKind::baseline};
  StreamId stream{};
  StreamEpoch epoch{};
  std::uint64_t sequence{};
  std::optional<TransportSequenceRange> affected_range;
};

struct SequenceLossLimits {
  std::size_t maximum_tracks{1024};
  std::size_t maximum_missing_ranges_per_track{1024};
};
```

and the move-only `SequenceLossObserver` API exactly as specified.

- [ ] **Step 1: Expand RED tests for the complete observer semantics**

Add tests that prove:

```cpp
TEST(transport_recovery_sequence_observer_handles_in_order_and_late) {
  codec::SequenceLossObserver observer;
  EXPECT_EQ(observer.observe(frame_for("A", 2))->kind,
            codec::SequenceObservationKind::baseline);
  EXPECT_EQ(observer.observe(frame_for("A", 3))->kind,
            codec::SequenceObservationKind::in_order);
  EXPECT_EQ(observer.observe(frame_for("A", 3))->kind,
            codec::SequenceObservationKind::late_or_replayed);
}
```

Add a split-range case: baseline 0, observe 5 to open `[1,5)`, then observe 3; require `missing()` returns `[1,3)` and `[4,5)`.

Add independence cases for different `StreamId`s and exact epochs: `(A, epoch 1/1)` may have a gap while `(A, epoch 2/1)` and `(B, epoch 1/1)` remain independent baselines.

Add `UINT64_MAX` coverage: baseline at `UINT64_MAX` succeeds; a later lower value is `late_or_replayed`; no overflow occurs and no forward gap can be opened.

- [ ] **Step 2: Add transactional limit RED tests**

Use:

```cpp
codec::SequenceLossLimits limits;
limits.maximum_tracks = 1;
limits.maximum_missing_ranges_per_track = 1;
```

Prove a second exact stream/epoch track returns `resource_exhausted` without changing the first track.

For range fragmentation, create `[1,5)` then attempt to fill `3`. Because this would split one range into two while the limit is one, require `resource_exhausted` and verify `missing()` still returns exactly `[1,5)`.

Zero `maximum_tracks` or zero `maximum_missing_ranges_per_track` must return `invalid_argument` on first use without allocating track state.

- [ ] **Step 3: Implement `include/codec/recovery.hpp` public observer types**

Include `<codec/transport.hpp>` plus `<cstddef>`, `<cstdint>`, `<memory>`, `<optional>`, and `<vector>`. Declare all E.2 types, including the recovery-group types from Task 3 so the public file is added once and remains type-consistent.

- [ ] **Step 4: Implement observer state in `src/transport/recovery.cpp`**

Use a private track key containing `StreamId`, `epoch.connection`, and `epoch.format`. A simple vector of tracks is acceptable because `maximum_tracks` is bounded and defaults to 1024; avoid adding hash specializations to the public API.

Each track stores:

```cpp
struct LossTrack {
  StreamId stream{};
  StreamEpoch epoch{};
  std::uint64_t frontier{};
  bool frontier_exhausted{false};
  std::vector<TransportSequenceRange> missing;
};
```

`frontier` is the next expected sequence when `frontier_exhausted == false`.

Baseline logic:
- first frame creates the track;
- if sequence is `UINT64_MAX`, set `frontier_exhausted=true`;
- otherwise set `frontier=sequence+1`.

Existing-track logic:
- if not exhausted and `sequence == frontier`, emit `in_order` and checked-advance frontier;
- if not exhausted and `sequence > frontier`, preflight one additional range against the range limit, append `[frontier, sequence)`, emit `gap_opened`, and set frontier to `sequence+1` or exhausted at max;
- otherwise search the sorted missing ranges. If the sequence is absent, emit `late_or_replayed` with no mutation;
- if present, precompute whether removing one sequence deletes, shrinks, or splits the range. A split requires one additional range slot; enforce the configured limit before mutation. Emit `gap_filled` with affected range `[sequence, sequence+1)` (the max sequence can never be inside a forward-created half-open range).

Catch `std::bad_alloc` and map to `resource_exhausted` without exposing partial mutation. For operations that may allocate, construct a staged copy of the missing-range vector first, then swap on success.

`missing()` validates limits, returns an empty vector for an unknown track, and copies the current sorted ranges.

- [ ] **Step 5: Register production source and run targeted GREEN**

Add `src/transport/recovery.cpp` to `codec_core` in CMake.

Run in CI via PR and require all `transport_recovery_sequence_*` tests plus the existing suite to pass on GCC/Clang/sanitizers before proceeding.

- [ ] **Step 6: Commit observer GREEN**

Record the exact commit/run that first makes the observer suite green.

---

### Task 3: Implement explicit recovery-group tracking and sealing

**Files:**
- Modify: `include/codec/recovery.hpp`
- Modify: `src/transport/recovery.cpp`
- Modify: `tests/test_transport_recovery.cpp`

**Interfaces:**
- Consumes: Task 2 `TransportSequenceRange`, `MultiplexFrame`.
- Produces the exact spec types:

```cpp
struct RecoveryGroupKey {
  StreamId stream{};
  StreamEpoch epoch{};
  std::uint64_t group_sequence{};
};

struct RecoveryGroupDescriptor {
  RecoveryGroupKey key{};
  std::uint64_t first_sequence{};
  std::uint64_t source_count{};
};

enum class RecoveryGroupState : std::uint8_t {
  collecting = 1,
  observed_complete = 2,
  sealed_complete = 3,
  sealed_incomplete = 4,
};

enum class RecoveryGroupFrameKind : std::uint8_t {
  not_member = 1,
  first_observation = 2,
  duplicate = 3,
};

struct RecoveryGroupFrameObservation {
  RecoveryGroupFrameKind kind{RecoveryGroupFrameKind::not_member};
  std::optional<RecoveryGroupKey> group;
  std::uint64_t sequence{};
};

struct RecoveryGroupReport {
  RecoveryGroupDescriptor descriptor{};
  RecoveryGroupState state{RecoveryGroupState::collecting};
  std::uint64_t observed_source_count{};
  std::vector<TransportSequenceRange> missing_ranges;
};

struct RecoveryGroupLimits {
  std::size_t maximum_groups{64};
  std::uint64_t maximum_source_frames_per_group{65536};
  std::uint64_t maximum_tracked_source_slots{262144};
};
```

and the move-only `RecoveryGroupTracker` methods `begin`, `observe`, `status`, `seal`.

- [ ] **Step 1: Add recovery-group RED tests**

Create helper:

```cpp
codec::RecoveryGroupDescriptor group_for(std::string_view name,
                                         std::uint64_t group_sequence,
                                         std::uint64_t first,
                                         std::uint64_t count,
                                         std::uint64_t connection = 1,
                                         std::uint64_t format = 1) {
  return {
      .key = {
          .stream = codec::derive_stream_id(name),
          .epoch = {.connection = connection, .format = format},
          .group_sequence = group_sequence,
      },
      .first_sequence = first,
      .source_count = count,
  };
}
```

Prove:
- begin group `[100,104)`; status initially has missing `[100,104)` and state `collecting`;
- observe 102 then 100 then 103 -> three `first_observation`s, out of order accepted;
- observe 102 again -> `duplicate`, count unchanged;
- status reports one missing range `[101,102)`;
- unrelated stream, epoch, or sequence returns `not_member`;
- observe 101 -> `observed_complete`, no missing ranges;
- seal -> `sealed_complete` and repeated seal is byte/field-equivalent.

Add an incomplete group `[7,12)` observed at 7, 9, 11; seal must return missing `[8,9)` and `[10,11)` with `sealed_incomplete`.

- [ ] **Step 2: Add validation/bounds/sealed-state RED tests**

Prove `begin()` rejects:
- `source_count == 0` -> `invalid_argument`;
- range overflow (`first_sequence = UINT64_MAX`, `source_count = 2`) -> `invalid_argument`;
- source_count above per-group limit -> `resource_exhausted`;
- maximum group count exceeded -> `resource_exhausted`;
- aggregate tracked slots exceeded -> `resource_exhausted`;
- duplicate key -> `invalid_argument`;
- overlapping ranges in same exact stream/epoch -> `invalid_argument`;
- identical numeric range in another epoch or stream is allowed.

After sealing, observing a frame that falls inside that group must return `invalid_argument` and the frozen status must remain unchanged.

Unknown `status()` or `seal()` key returns `invalid_argument`.

Zero-valued local limits are `invalid_argument` before mutation.

- [ ] **Step 3: Implement group storage**

Private group state:

```cpp
struct GroupState {
  RecoveryGroupDescriptor descriptor{};
  std::vector<std::uint8_t> present;
  std::uint64_t observed_count{};
  bool sealed{false};
};
```

The tracker pimpl stores `RecoveryGroupLimits limits`, `std::vector<GroupState> groups`, and `std::uint64_t tracked_slots`.

`begin()` must preflight all descriptor arithmetic, limits, duplicate keys, and overlapping range checks before allocating/pushing the staged group. Use checked addition for `tracked_slots + source_count`.

Group-key equality compares all 16 StreamId bytes, both epoch values, and `group_sequence`.

Namespace overlap compares exact stream and both epoch values, then checks half-open range overlap without unchecked end arithmetic (ends are validated during descriptor preflight).

- [ ] **Step 4: Implement observe/status/seal**

`observe()` finds at most one matching source range because overlap is forbidden. If none, return `not_member`. If the group is sealed, return `invalid_argument`. Otherwise mark `present[index]` exactly once and update count. Return `duplicate` for an already-present slot.

`status()` computes missing ranges by scanning the bounded bitmap and grouping consecutive absent indices. Convert indices back to logical sequence values with checked arithmetic already guaranteed by the descriptor. State is:
- unsealed + count < source_count -> `collecting`;
- unsealed + count == source_count -> `observed_complete`;
- sealed + count == source_count -> `sealed_complete`;
- sealed + count < source_count -> `sealed_incomplete`.

`seal()` finds the group, sets `sealed=true` only once, and returns `status()`. Repeated calls return the same frozen report.

Any allocation while building a report maps to `resource_exhausted`; sealing must not depend on report allocation to become irreversible. Therefore build the report first against a staged `sealed=true` state, then commit the `sealed` bit only after report construction succeeds. Repeated successful seals are deterministic.

- [ ] **Step 5: Run targeted and full GREEN**

Require all `transport_recovery_*` tests and the complete CTest matrix to pass remotely under GCC, Clang, and sanitizers. Do not alter E.1 tests or sanitizer configuration.

- [ ] **Step 6: Commit group semantics GREEN**

Record the exact commit/run that establishes the complete E.2 functional contract.

---

### Task 4: Prove E.1 integration and installed-package usability; synchronize docs

**Files:**
- Modify: `tests/test_transport_recovery.cpp`
- Modify: `tests/package_consumer/main.cpp`
- Modify: `README.md`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: E.1 `encode_multiplex_frame`/`MultiplexDecoder` and E.2 public API.
- Produces: transport-stack integration proof and truthful implementation status.

- [ ] **Step 1: Add E.1 -> E.2 integration test**

Encode physical CMX1 frames for one stream with sequences `40, 42, 41`, concatenate them, decode through `MultiplexDecoder`, and feed the resulting frames in physical order to `SequenceLossObserver` and an active recovery group `[40,43)`.

Require observer kinds `baseline`, `gap_opened` with `[41,42)`, then `gap_filled`; require the group becomes `observed_complete` despite the out-of-order arrival. Verify every decoded stream/epoch/sequence matches the original frame metadata.

- [ ] **Step 2: Extend installed-package consumer**

Add `#include <codec/recovery.hpp>`.

After the existing E.1 package-consumer decode, construct a `SequenceLossObserver`, observe the decoded frame, and require `baseline`.

Create one `RecoveryGroupTracker`, begin a one-source group matching the decoded frame, observe it, seal it, and require `sealed_complete` with no missing ranges. No new link dependency is permitted.

- [ ] **Step 3: Update README**

Under the Transport / Recovery Profile section, state only:
- E.2 adds bounded exact-stream/exact-epoch provisional sequence-gap observation;
- late frames can fill provisional gaps;
- explicit bounded recovery groups can be sealed into deterministic complete/incomplete reports;
- incomplete does not imply recoverable or unrecoverable.

Keep explicit non-claims for FEC/reconstruction/ARQ, repair-symbol wire encoding, persistence, network I/O, auth, CLI/C ABI, benchmarks, and Stage E completion.

Update `implemented_v0_1.generic` and `planned_not_implemented` so they no longer say loss/group semantics are absent but still say concrete recovery/FEC is absent.

- [ ] **Step 4: Update CHANGELOG**

Add one Unreleased E.2 entry before E.1 summarizing the additive recovery API and non-claims.

- [ ] **Step 5: Require package-consumer and full CI GREEN**

The exact candidate must pass GCC/Clang build, tests, install, external package consumer, and sanitizer CTest with existing ONNX isolation.

- [ ] **Step 6: Commit docs/package proof**

Record the exact final candidate SHA and tree.

---

### Task 5: Final review, exact-head integration, and roadmap evidence

**Files:**
- No production changes unless a concrete review/verification defect is found.
- Update PR metadata and roadmap issue #10 after verification.

**Interfaces:**
- Consumes: complete E.2 branch.
- Produces: reviewed exact-head CI evidence, merge, post-merge proof, and next-dependency record.

- [ ] **Step 1: Review final diff against base `e06d7f0616ae821bb95c5656798b6d8ea8825665`**

Require:
- no CMX1 format/source behavior change except CMake registration of E.2 source/tests;
- no CODA/archive implementation change;
- no S0/S1/D truth assignment;
- exact per-stream/per-epoch observer keys;
- no cross-epoch loss inference;
- transactional range-split behavior on resource exhaustion;
- no overlapping group ranges in one stream/epoch namespace;
- all sequence/end/slot arithmetic checked;
- sealing immutable and idempotent;
- no repair/reconstruction claim or implementation;
- package consumer uses installed E.2 symbols.

- [ ] **Step 2: Require exact-head remote CI**

The exact branch head must pass all GCC, Clang, package-consumer, and sanitizer jobs. Read the sanitizer job result rather than relying only on the workflow badge.

- [ ] **Step 3: Correct PR metadata**

Replace the initial intentional-RED description with final head/tree/run evidence and explicit non-claims. Mark ready for review if still draft.

- [ ] **Step 4: Apply merge guards**

Require no incompatible `main` drift, mergeable PR, no unresolved review thread, and head SHA equal to the exact tested SHA.

- [ ] **Step 5: Merge the exact tested head**

Use the repository's authorized integration method with an expected-head guard. After merge, verify the published commit is GitHub-verified when GitHub creates it and that the published tree equals the exact tested PR tree.

- [ ] **Step 6: Require post-merge `main` CI**

Require the push-triggered run on the published `main` commit to pass GCC, Clang, installed-package consumer, and sanitizers.

- [ ] **Step 7: Record completion on roadmap issue #10**

Include base/head/tree/PR/RED run/final exact-head run/merge SHA/post-merge run, the exact E.2 capability claim, all non-claims, and the next dependency: a repair-symbol contract plus one concrete bounded recovery scheme measured against sealed recovery groups.