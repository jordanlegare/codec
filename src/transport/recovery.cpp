#include <codec/recovery.hpp>

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

SequenceObservation make_observation(SequenceObservationKind kind,
                                     const MultiplexFrame& frame) {
  SequenceObservation observation;
  observation.kind = kind;
  observation.stream = frame.stream;
  observation.epoch = frame.epoch;
  observation.sequence = frame.sequence;
  return observation;
}

Result<void> validate_group_limits(const RecoveryGroupLimits& limits) {
  if (limits.maximum_groups == 0 ||
      limits.maximum_source_frames_per_group == 0 ||
      limits.maximum_tracked_source_slots == 0) {
    return fail(ErrorCode::invalid_argument,
                "recovery-group limits must be non-zero");
  }
  return {};
}

Result<std::uint64_t> recovery_group_end(
    const RecoveryGroupDescriptor& descriptor) {
  if (descriptor.source_count == 0) {
    return fail<std::uint64_t>(ErrorCode::invalid_argument,
                               "recovery group must contain source frames");
  }
  if (descriptor.source_count >
      std::numeric_limits<std::uint64_t>::max() -
          descriptor.first_sequence) {
    return fail<std::uint64_t>(ErrorCode::invalid_argument,
                               "recovery-group sequence range overflows");
  }
  return descriptor.first_sequence + descriptor.source_count;
}

bool same_group_namespace(const RecoveryGroupDescriptor& lhs,
                          const RecoveryGroupDescriptor& rhs) noexcept {
  return lhs.key.stream == rhs.key.stream &&
         same_epoch(lhs.key.epoch, rhs.key.epoch);
}

struct GroupState {
  RecoveryGroupDescriptor descriptor{};
  std::vector<std::uint8_t> present;
  std::uint64_t observed_count{};
  bool sealed{false};
};

GroupState* find_group(std::vector<GroupState>& groups,
                       const RecoveryGroupKey& key) noexcept {
  for (auto& group : groups) {
    if (group.descriptor.key == key) return &group;
  }
  return nullptr;
}

const GroupState* find_group(const std::vector<GroupState>& groups,
                             const RecoveryGroupKey& key) noexcept {
  for (const auto& group : groups) {
    if (group.descriptor.key == key) return &group;
  }
  return nullptr;
}

Result<RecoveryGroupReport> make_group_report(const GroupState& group,
                                               bool sealed_view) {
  RecoveryGroupReport report;
  report.descriptor = group.descriptor;
  report.observed_source_count = group.observed_count;
  const bool complete = group.observed_count == group.descriptor.source_count;
  if (sealed_view) {
    report.state = complete ? RecoveryGroupState::sealed_complete
                            : RecoveryGroupState::sealed_incomplete;
  } else {
    report.state = complete ? RecoveryGroupState::observed_complete
                            : RecoveryGroupState::collecting;
  }

  try {
    std::size_t index = 0;
    while (index < group.present.size()) {
      while (index < group.present.size() && group.present[index] != 0) {
        ++index;
      }
      if (index == group.present.size()) break;
      const auto missing_begin = index;
      while (index < group.present.size() && group.present[index] == 0) {
        ++index;
      }
      const auto missing_end = index;
      report.missing_ranges.push_back(TransportSequenceRange{
          group.descriptor.first_sequence +
              static_cast<std::uint64_t>(missing_begin),
          group.descriptor.first_sequence +
              static_cast<std::uint64_t>(missing_end),
      });
    }
    return report;
  } catch (const std::bad_alloc&) {
    return fail<RecoveryGroupReport>(
        ErrorCode::resource_exhausted,
        "recovery-group report allocation failed");
  }
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

struct RecoveryGroupTracker::Impl {
  explicit Impl(RecoveryGroupLimits configured) : limits(configured) {}

  RecoveryGroupLimits limits{};
  std::vector<GroupState> groups;
  std::uint64_t tracked_slots{};
};

RecoveryGroupTracker::RecoveryGroupTracker(RecoveryGroupLimits limits)
    : impl_(new (std::nothrow) Impl{limits}) {}
RecoveryGroupTracker::RecoveryGroupTracker(RecoveryGroupTracker&&) noexcept =
    default;
RecoveryGroupTracker& RecoveryGroupTracker::operator=(
    RecoveryGroupTracker&&) noexcept = default;
RecoveryGroupTracker::~RecoveryGroupTracker() = default;

Result<void> RecoveryGroupTracker::begin(
    const RecoveryGroupDescriptor& descriptor) {
  if (!impl_) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group tracker allocation failed");
  }
  auto valid_limits = validate_group_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  auto end = recovery_group_end(descriptor);
  if (!end) return end.error();
  if (descriptor.source_count >
      impl_->limits.maximum_source_frames_per_group) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group source-frame limit exceeded");
  }
  if (descriptor.source_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group source range cannot be represented");
  }
  if (impl_->groups.size() >= impl_->limits.maximum_groups) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group count limit exceeded");
  }
  if (descriptor.source_count >
          std::numeric_limits<std::uint64_t>::max() - impl_->tracked_slots ||
      impl_->tracked_slots + descriptor.source_count >
          impl_->limits.maximum_tracked_source_slots) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group tracked-slot limit exceeded");
  }

  for (const auto& existing : impl_->groups) {
    if (existing.descriptor.key == descriptor.key) {
      return fail(ErrorCode::invalid_argument,
                  "recovery-group key already exists");
    }
    if (!same_group_namespace(existing.descriptor, descriptor)) continue;
    auto existing_end = recovery_group_end(existing.descriptor);
    if (!existing_end) {
      return fail(ErrorCode::internal,
                  "stored recovery-group descriptor is invalid");
    }
    if (descriptor.first_sequence < *existing_end &&
        existing.descriptor.first_sequence < *end) {
      return fail(ErrorCode::invalid_argument,
                  "recovery-group source ranges overlap");
    }
  }

  try {
    GroupState staged;
    staged.descriptor = descriptor;
    staged.present.assign(static_cast<std::size_t>(descriptor.source_count), 0);
    impl_->groups.push_back(std::move(staged));
    impl_->tracked_slots += descriptor.source_count;
    return {};
  } catch (const std::bad_alloc&) {
    return fail(ErrorCode::resource_exhausted,
                "recovery-group allocation failed");
  }
}

Result<RecoveryGroupFrameObservation> RecoveryGroupTracker::observe(
    const MultiplexFrame& frame) {
  if (!impl_) {
    return fail<RecoveryGroupFrameObservation>(
        ErrorCode::resource_exhausted,
        "recovery-group tracker allocation failed");
  }
  auto valid_limits = validate_group_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  RecoveryGroupFrameObservation observation;
  observation.sequence = frame.sequence;

  GroupState* matched = nullptr;
  for (auto& group : impl_->groups) {
    if (group.descriptor.key.stream != frame.stream ||
        !same_epoch(group.descriptor.key.epoch, frame.epoch)) {
      continue;
    }
    auto end = recovery_group_end(group.descriptor);
    if (!end) {
      return fail<RecoveryGroupFrameObservation>(
          ErrorCode::internal,
          "stored recovery-group descriptor is invalid");
    }
    if (frame.sequence >= group.descriptor.first_sequence &&
        frame.sequence < *end) {
      matched = &group;
      break;
    }
  }

  if (!matched) {
    observation.kind = RecoveryGroupFrameKind::not_member;
    return observation;
  }
  if (matched->sealed) {
    return fail<RecoveryGroupFrameObservation>(
        ErrorCode::invalid_argument,
        "sealed recovery group cannot accept more observations");
  }

  observation.group = matched->descriptor.key;
  const auto index = static_cast<std::size_t>(
      frame.sequence - matched->descriptor.first_sequence);
  if (matched->present[index] != 0) {
    observation.kind = RecoveryGroupFrameKind::duplicate;
    return observation;
  }

  matched->present[index] = 1;
  ++matched->observed_count;
  observation.kind = RecoveryGroupFrameKind::first_observation;
  return observation;
}

Result<RecoveryGroupReport> RecoveryGroupTracker::status(
    const RecoveryGroupKey& key) const {
  if (!impl_) {
    return fail<RecoveryGroupReport>(ErrorCode::resource_exhausted,
                                     "recovery-group tracker allocation failed");
  }
  auto valid_limits = validate_group_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  const auto* group = find_group(impl_->groups, key);
  if (!group) {
    return fail<RecoveryGroupReport>(ErrorCode::invalid_argument,
                                     "recovery-group key was not found");
  }
  return make_group_report(*group, group->sealed);
}

Result<RecoveryGroupReport> RecoveryGroupTracker::seal(
    const RecoveryGroupKey& key) {
  if (!impl_) {
    return fail<RecoveryGroupReport>(ErrorCode::resource_exhausted,
                                     "recovery-group tracker allocation failed");
  }
  auto valid_limits = validate_group_limits(impl_->limits);
  if (!valid_limits) return valid_limits.error();

  auto* group = find_group(impl_->groups, key);
  if (!group) {
    return fail<RecoveryGroupReport>(ErrorCode::invalid_argument,
                                     "recovery-group key was not found");
  }
  if (group->sealed) return make_group_report(*group, true);

  auto report = make_group_report(*group, true);
  if (!report) return report.error();
  group->sealed = true;
  return report;
}

}  // namespace codec
