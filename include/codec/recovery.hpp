#pragma once

#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace codec {

struct TransportSequenceRange {
  std::uint64_t begin{};
  std::uint64_t end{};

  bool operator==(const TransportSequenceRange&) const = default;
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

struct RecoveryGroupKey {
  StreamId stream{};
  StreamEpoch epoch{};
  std::uint64_t group_sequence{};
};

inline bool operator==(const RecoveryGroupKey& lhs,
                       const RecoveryGroupKey& rhs) noexcept {
  return lhs.stream == rhs.stream &&
         lhs.epoch.connection == rhs.epoch.connection &&
         lhs.epoch.format == rhs.epoch.format &&
         lhs.group_sequence == rhs.group_sequence;
}

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

}  // namespace codec
