#include <codec/recovery.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace codec {
namespace {

bool same_epoch(const StreamEpoch& lhs, const StreamEpoch& rhs) noexcept {
  return lhs.connection == rhs.connection && lhs.format == rhs.format;
}

Result<void> validate_sequence_limits(const SequenceLossLimits& limits) {
  if (limits.maximum_tracks == 0 ||
      limits.maximum_missing_ranges_per_track == 0) {
    return fail(ErrorCode::invalid_argument,
                "sequence-loss limits must be non-zero");
  }
  return {};
}

struct LossTrack {
  StreamId stream{};
  StreamEpoch epoch{};
  std::uint64_t frontier{};
  bool frontier_exhausted{false};
  std::vector<TransportSequenceRange> missing;
};

LossTrack* find_track(std::vector<LossTrack>& tracks, const StreamId& stream,
                      const StreamEpoch& epoch) noexcept {
  for (auto& track : tracks) {
    if (track.stream == stream && same_epoch(track.epoch, epoch)) {
      return &track;
    }
  }
  return nullptr;
}

const LossTrack* find_track(const std::vector<LossTrack>& tracks,
                            const StreamId& stream,
                            const StreamEpoch& epoch) noexcept {
  for (const auto& track : tracks) {
    if (track.stream == stream && same_epoch(track.epoch, epoch)) {
      return &track;
    }
  }
  return nullptr;
}

SequenceObservation make_observation(SequenceObservationKind kind,
                                     const MultiplexFrame& frame) {
  SequenceObservation observation;
  observation.kind = kind;
  observation.stream = frame.stream;
  observation.epoch = frame.epoch;
  observation.sequence = frame.sequence;
  return observation;
}

}  // namespace

struct SequenceLossObserver::Impl {
  explicit Impl(SequenceLossLimits configured) : limits(configured) {}

  SequenceLossLimits limits{};
  std::vector<LossTrack> tracks;
};

SequenceLossObserver::SequenceLossObserver(SequenceLossLimits limits)
    : impl_(new (std::nothrow) Impl{limits}) {}

SequenceLossObserver::SequenceLossObserver(SequenceLossObserver&&) noexcept =
    default;
SequenceLossObserver& SequenceLossObserver::operator=(
    SequenceLossObserver&&) noexcept = default;
SequenceLossObserver::~SequenceLossObserver() = default;

Result<SequenceObservation> SequenceLossObserver::observe(
    const MultiplexFrame& frame) {
  if (!impl_) {
    return fail<SequenceObservation>(ErrorCode::resource_exhausted,
                                     "sequence-loss observer allocation failed");
  }
  auto valid_limits = validate_sequence_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  auto* track = find_track(impl_->tracks, frame.stream, frame.epoch);
  if (!track) {
    if (impl_->tracks.size() >= impl_->limits.maximum_tracks) {
      return fail<SequenceObservation>(ErrorCode::resource_exhausted,
                                       "sequence-loss track limit exceeded");
    }
    try {
      LossTrack created;
      created.stream = frame.stream;
      created.epoch = frame.epoch;
      if (frame.sequence == std::numeric_limits<std::uint64_t>::max()) {
        created.frontier_exhausted = true;
      } else {
        created.frontier = frame.sequence + 1;
      }
      impl_->tracks.push_back(std::move(created));
    } catch (const std::bad_alloc&) {
      return fail<SequenceObservation>(
          ErrorCode::resource_exhausted,
          "sequence-loss track allocation failed");
    }
    return make_observation(SequenceObservationKind::baseline, frame);
  }

  if (!track->frontier_exhausted && frame.sequence == track->frontier) {
    auto observation = make_observation(SequenceObservationKind::in_order,
                                        frame);
    if (frame.sequence == std::numeric_limits<std::uint64_t>::max()) {
      track->frontier_exhausted = true;
    } else {
      track->frontier = frame.sequence + 1;
    }
    return observation;
  }

  if (!track->frontier_exhausted && frame.sequence > track->frontier) {
    if (track->missing.size() >=
        impl_->limits.maximum_missing_ranges_per_track) {
      return fail<SequenceObservation>(
          ErrorCode::resource_exhausted,
          "sequence-loss missing-range limit exceeded");
    }
    try {
      auto staged = track->missing;
      const TransportSequenceRange opened{track->frontier, frame.sequence};
      staged.push_back(opened);

      auto observation = make_observation(
          SequenceObservationKind::gap_opened, frame);
      observation.affected_range = opened;

      track->missing.swap(staged);
      if (frame.sequence == std::numeric_limits<std::uint64_t>::max()) {
        track->frontier_exhausted = true;
      } else {
        track->frontier = frame.sequence + 1;
      }
      return observation;
    } catch (const std::bad_alloc&) {
      return fail<SequenceObservation>(
          ErrorCode::resource_exhausted,
          "sequence-loss missing-range allocation failed");
    }
  }

  std::size_t range_index = track->missing.size();
  for (std::size_t index = 0; index < track->missing.size(); ++index) {
    const auto& range = track->missing[index];
    if (frame.sequence >= range.begin && frame.sequence < range.end) {
      range_index = index;
      break;
    }
  }
  if (range_index == track->missing.size()) {
    return make_observation(SequenceObservationKind::late_or_replayed,
                            frame);
  }

  const auto selected = track->missing[range_index];
  const bool split = frame.sequence > selected.begin &&
                     frame.sequence + 1 < selected.end;
  if (split && track->missing.size() >=
                   impl_->limits.maximum_missing_ranges_per_track) {
    return fail<SequenceObservation>(
        ErrorCode::resource_exhausted,
        "sequence-loss missing-range split exceeds configured limit");
  }

  try {
    auto staged = track->missing;
    auto& range = staged[range_index];
    if (range.begin == frame.sequence && range.end == frame.sequence + 1) {
      staged.erase(staged.begin() + static_cast<std::ptrdiff_t>(range_index));
    } else if (range.begin == frame.sequence) {
      range.begin = frame.sequence + 1;
    } else if (range.end == frame.sequence + 1) {
      range.end = frame.sequence;
    } else {
      const TransportSequenceRange right{frame.sequence + 1, range.end};
      range.end = frame.sequence;
      staged.insert(staged.begin() +
                        static_cast<std::ptrdiff_t>(range_index + 1),
                    right);
    }

    auto observation = make_observation(SequenceObservationKind::gap_filled,
                                        frame);
    observation.affected_range =
        TransportSequenceRange{frame.sequence, frame.sequence + 1};
    track->missing.swap(staged);
    return observation;
  } catch (const std::bad_alloc&) {
    return fail<SequenceObservation>(
        ErrorCode::resource_exhausted,
        "sequence-loss missing-range allocation failed");
  }
}

Result<std::vector<TransportSequenceRange>> SequenceLossObserver::missing(
    const StreamId& stream, const StreamEpoch& epoch) const {
  if (!impl_) {
    return fail<std::vector<TransportSequenceRange>>(
        ErrorCode::resource_exhausted,
        "sequence-loss observer allocation failed");
  }
  auto valid_limits = validate_sequence_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  const auto* track = find_track(impl_->tracks, stream, epoch);
  if (!track) return std::vector<TransportSequenceRange>{};
  try {
    return track->missing;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<TransportSequenceRange>>(
        ErrorCode::resource_exhausted,
        "sequence-loss missing-range copy failed");
  }
}

struct RecoveryGroupTracker::Impl {};

RecoveryGroupTracker::RecoveryGroupTracker(RecoveryGroupLimits)
    : impl_(new (std::nothrow) Impl{}) {}
RecoveryGroupTracker::RecoveryGroupTracker(RecoveryGroupTracker&&) noexcept =
    default;
RecoveryGroupTracker& RecoveryGroupTracker::operator=(
    RecoveryGroupTracker&&) noexcept = default;
RecoveryGroupTracker::~RecoveryGroupTracker() = default;

Result<void> RecoveryGroupTracker::begin(const RecoveryGroupDescriptor&) {
  return fail(ErrorCode::internal,
              "recovery-group tracking is not implemented yet");
}

Result<RecoveryGroupFrameObservation> RecoveryGroupTracker::observe(
    const MultiplexFrame&) {
  return fail<RecoveryGroupFrameObservation>(
      ErrorCode::internal,
      "recovery-group tracking is not implemented yet");
}

Result<RecoveryGroupReport> RecoveryGroupTracker::status(
    const RecoveryGroupKey&) const {
  return fail<RecoveryGroupReport>(
      ErrorCode::internal,
      "recovery-group tracking is not implemented yet");
}

Result<RecoveryGroupReport> RecoveryGroupTracker::seal(
    const RecoveryGroupKey&) {
  return fail<RecoveryGroupReport>(
      ErrorCode::internal,
      "recovery-group tracking is not implemented yet");
}

}  // namespace codec
