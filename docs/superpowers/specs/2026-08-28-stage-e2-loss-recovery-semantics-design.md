# Stage E.2 Explicit Loss/Gap Observation and Recovery-Group Semantics

## Work record

```yaml
task: Add bounded generic sequence-loss observation and recovery-group semantics above Stage E.1 CMX1 without choosing or implementing a concrete FEC/retransmission algorithm.
base_ref: main
base_head_sha: e06d7f0616ae821bb95c5656798b6d8ea8825665
work_branch: automation/stage-e2-loss-recovery-semantics
current_version: 0.1.0
active_roadmap_stage: E — transport/recovery profile; E.1 CMX1 multiplexing is integrated and all-green
scope: generic transport loss observation and recovery-group state
truth_classes: unchanged
coda_layout_delta: none
cmx1_wire_delta: none
new_capability_claim: CODEC can deterministically observe provisional per-stream/per-epoch sequence gaps and late fills, and can explicitly track/seal bounded same-stream/same-epoch recovery groups to report unresolved source-sequence ranges without claiming that those ranges are recoverable.
change_class: transport_profile
```

## Decision

Stage E.2 adds semantics **above** the E.1 `CMX1` frame codec. It does not modify the CMX1 wire format and does not infer a particular repair algorithm.

The milestone has two independent public components:

1. `SequenceLossObserver` consumes decoded `MultiplexFrame` metadata and maintains bounded sequence-observation state independently for each `(StreamId, StreamEpoch)` track. It distinguishes a first baseline frame, in-order progress, a provisional forward gap, a later frame that fills a known provisional gap, and an older frame that cannot be proven to be a gap fill and is therefore only classified as late-or-replayed.
2. `RecoveryGroupTracker` consumes caller-declared, bounded source-sequence groups. A group is scoped to exactly one logical stream and one exact connection/format epoch. Frames may arrive in any order. Before sealing, missing members are observations only. Sealing freezes the group and deterministically reports the unresolved source-sequence ranges. E.2 never labels an incomplete group recoverable or unrecoverable because no repair scheme exists yet.

This separation is deliberate. A raw sequence jump is not sufficient evidence of permanent loss because physical transports may reorder. A sealed recovery group supplies an explicit closure boundary without pretending that E.2 can repair the missing members.

## Alternatives considered

1. **Additive observation + explicit group state above CMX1 — selected.** Preserves E.1 bytes, separates transient reordering from group closure, and gives later FEC/retransmission milestones a stable semantic target.
2. **Treat every sequence jump as confirmed loss — rejected.** This would conflate reordering with permanent loss and make later recovery semantics unsound.
3. **Add recovery fields directly to CMX1 — rejected for E.2.** This would prematurely freeze repair-algorithm metadata into the transport wire before a recovery scheme has been selected or qualified.

## Public API

Add `include/codec/recovery.hpp`.

### Sequence ranges

```cpp
struct TransportSequenceRange {
  // Half-open logical-stream sequence range [begin, end).
  std::uint64_t begin{};
  std::uint64_t end{};
};
```

Ranges are always non-empty when returned. Sequence arithmetic is checked for overflow.

### Provisional loss observation

```cpp
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

class SequenceLossObserver {
 public:
  explicit SequenceLossObserver(SequenceLossLimits limits = {});
  SequenceLossObserver(SequenceLossObserver&&) noexcept;
  SequenceLossObserver& operator=(SequenceLossObserver&&) noexcept;
  ~SequenceLossObserver();

  SequenceLossObserver(const SequenceLossObserver&) = delete;
  SequenceLossObserver& operator=(const SequenceLossObserver&) = delete;

  Result<SequenceObservation> observe(const MultiplexFrame& frame);
  Result<std::vector<TransportSequenceRange>> missing(
      const StreamId& stream, const StreamEpoch& epoch) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

A track is identified by the exact `(StreamId, connection epoch, format epoch)` tuple. E.2 deliberately does not infer missing sequence numbers across an epoch boundary. The first frame observed for each track establishes the baseline, regardless of its numeric sequence value.

Within one track:

- first frame -> `baseline`; no earlier loss is inferred;
- sequence equal to the next expected sequence -> `in_order`;
- sequence greater than the next expected sequence -> `gap_opened` with one half-open affected range `[next_expected, sequence)`; the received frame itself advances the frontier;
- sequence below the frontier and inside a currently missing range -> `gap_filled`; the missing-range set is reduced or split transactionally;
- sequence below the frontier but not inside a currently missing range -> `late_or_replayed`; E.2 does not claim whether this is an exact duplicate, stale frame, or another replay condition.

A sequence value of `UINT64_MAX` is valid. A track that has observed it has no representable next sequence; later observations for that track can only be late/gap-fill classifications and cannot open a forward range beyond the integer domain.

`missing()` returns the current provisional missing ranges for exactly one track in ascending non-overlapping order. An unknown track returns an empty vector.

If filling one missing sequence would split a range and exceed `maximum_missing_ranges_per_track`, `observe()` returns `resource_exhausted` and leaves the track unchanged.

### Recovery-group identity and descriptor

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
```

`group_sequence` is caller-defined and is unique within one `(StreamId, StreamEpoch)` namespace. It is not a CMX1 sequence number and is not a physical connection identifier.

A group covers exactly `[first_sequence, first_sequence + source_count)`. `source_count` must be nonzero and the range must not overflow. A group cannot span an epoch because both epoch values are part of the key.

### Recovery-group tracking

```cpp
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

class RecoveryGroupTracker {
 public:
  explicit RecoveryGroupTracker(RecoveryGroupLimits limits = {});
  RecoveryGroupTracker(RecoveryGroupTracker&&) noexcept;
  RecoveryGroupTracker& operator=(RecoveryGroupTracker&&) noexcept;
  ~RecoveryGroupTracker();

  RecoveryGroupTracker(const RecoveryGroupTracker&) = delete;
  RecoveryGroupTracker& operator=(const RecoveryGroupTracker&) = delete;

  Result<void> begin(const RecoveryGroupDescriptor& descriptor);
  Result<RecoveryGroupFrameObservation> observe(const MultiplexFrame& frame);
  Result<RecoveryGroupReport> status(const RecoveryGroupKey& key) const;
  Result<RecoveryGroupReport> seal(const RecoveryGroupKey& key);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
```

`begin()` validates local limits before mutating state. Groups with the same key are rejected. Source sequence ranges for groups in the same `(StreamId, StreamEpoch)` namespace may not overlap, even when `group_sequence` differs. Non-overlapping groups may coexist.

The tracker stores one bounded observation slot per source sequence. Aggregate source slots across all retained groups may not exceed `maximum_tracked_source_slots`. This makes worst-case tracker memory caller-bounded independently of payload size.

`observe()` matches by exact stream, exact epoch, and source sequence range:

- no matching group -> `not_member`;
- first observation of a source sequence -> `first_observation` and one slot becomes present;
- a repeated observation of an already-present member -> `duplicate` with no state change;
- a frame that belongs to a sealed group -> `invalid_argument`; sealing is a final semantic boundary for that tracker.

Because overlapping source ranges are forbidden within a stream/epoch namespace, one frame can match at most one group.

`status()` reports deterministic missing ranges by compressing absent source slots into ascending half-open ranges. Before seal the state is `collecting` or `observed_complete`. It makes no loss-finality claim.

`seal()` is idempotent. On first seal it freezes the group and returns `sealed_complete` when every source member was observed, otherwise `sealed_incomplete` plus exact unresolved ranges. Repeated `seal()` and `status()` return the same frozen result. No later observation may alter a sealed group.

## Relationship between observer and groups

The two components intentionally do not share hidden state.

A caller may use `SequenceLossObserver` for live diagnostics and `RecoveryGroupTracker` for explicit repair-unit accounting. A provisional observer gap can later disappear when a late frame arrives. A recovery-group report becomes final only after explicit `seal()`.

E.2 does not automatically turn a `SequenceLossObserver` gap into a `StreamGap` archive record. Doing so would couple transport observation to archive truth/persistence policy. A later integration layer may persist selected observations explicitly.

## Error mapping

- zero or internally inconsistent local limits: `invalid_argument`;
- duplicate/overlapping/overflowing recovery descriptors: `invalid_argument`;
- unknown recovery-group key for `status()` or `seal()`: `invalid_argument`;
- observation/group count or slot/range bounds exhausted: `resource_exhausted`;
- memory allocation failure caught at this boundary: `resource_exhausted`;
- observing a member of a sealed group: `invalid_argument`;
- unexpected implementation failure: `internal`.

E.2 introduces no network, archive, authentication, or decode error mapping.

## Truth, archive, wire, and compatibility effects

- **S0/S1/D:** unchanged. Observation and recovery-group state are operational metadata, not truth classification.
- **CODA layout/version:** unchanged.
- **CMX1 format/version:** unchanged.
- **Existing `StreamGap`:** unchanged. E.2 does not auto-persist transport observations.
- **E.1 encoder/decoder:** unchanged.
- **Audio Profile/inference:** unchanged.
- **C ABI/CLI:** unchanged.

## Implementation structure

- `include/codec/recovery.hpp` — public generic loss/range/group API.
- `src/transport/recovery.cpp` — bounded observer/group implementations.
- `tests/test_transport_recovery.cpp` — E.2 TDD proof.
- `CMakeLists.txt` — source/test registration.
- `tests/package_consumer/main.cpp` — installed public API proof.
- `README.md`, `CHANGELOG.md` — current-status synchronization and non-claims.

No new external dependency is added.

## Proof contract

The E.2 test suite must first establish RED against the absent `<codec/recovery.hpp>` API, then prove GREEN for:

1. first observation establishes a baseline without inventing prior loss;
2. in-order observations advance the frontier;
3. a forward jump opens exactly one provisional half-open missing range;
4. late fills shrink, remove, and split missing ranges deterministically;
5. old non-missing observations are reported only as `late_or_replayed`;
6. stream and exact epoch tracks are independent;
7. `UINT64_MAX` is handled without overflow;
8. track/range limits are enforced transactionally;
9. valid recovery groups accept out-of-order members and deterministic duplicates;
10. same-key and same-stream/epoch overlapping groups are rejected;
11. group descriptor range/slot/count limits and overflow are rejected before mutation;
12. `status()` reports `collecting` / `observed_complete` and exact missing ranges;
13. sealing incomplete and complete groups produces deterministic frozen reports;
14. sealing is idempotent and later member observation cannot mutate a sealed group;
15. unrelated stream/epoch/sequence frames return `not_member`;
16. actual E.1 encode/decode output can feed both E.2 components without metadata mutation;
17. installed-package consumer can include and use `<codec/recovery.hpp>` without a new link dependency;
18. all existing GCC, Clang, sanitizer, package-consumer, C ABI, CLI, Audio Profile, capability, and AI-contract gates remain green.

## Non-claims

E.2 does not implement or select Reed-Solomon, fountain/Raptor, XOR parity, convolutional coding, retransmission/ARQ, repair-symbol wire encoding, parity generation, reconstruction, erasure decoding, corruption resynchronization, congestion control, QoS, packet scheduling, network I/O, automatic archive persistence, authentication, encryption, distributed workers, benchmarked loss tolerance, throughput/latency/scale evidence, CLI/C ABI recovery commands, a frozen normative recovery wire standard, or Stage E completion.

The next Stage E dependency after E.2 should be a **repair-symbol contract and concrete bounded recovery scheme** whose claims can be measured against these sealed recovery-group semantics.