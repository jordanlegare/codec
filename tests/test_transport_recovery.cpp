#include "test.hpp"

#include <codec/recovery.hpp>
#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

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

codec::RecoveryGroupDescriptor group_for(
    std::string_view name, std::uint64_t group_sequence,
    std::uint64_t first_sequence, std::uint64_t source_count,
    std::uint64_t connection = 1, std::uint64_t format = 1) {
  codec::RecoveryGroupDescriptor descriptor;
  descriptor.key.stream = codec::derive_stream_id(name);
  descriptor.key.epoch = {.connection = connection, .format = format};
  descriptor.key.group_sequence = group_sequence;
  descriptor.first_sequence = first_sequence;
  descriptor.source_count = source_count;
  return descriptor;
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

TEST(transport_recovery_sequence_observer_handles_in_order_and_late) {
  codec::SequenceLossObserver observer;

  auto baseline = observer.observe(frame_for("A", 2));
  EXPECT_TRUE(baseline);
  EXPECT_EQ(baseline->kind, codec::SequenceObservationKind::baseline);

  auto in_order = observer.observe(frame_for("A", 3));
  EXPECT_TRUE(in_order);
  EXPECT_EQ(in_order->kind, codec::SequenceObservationKind::in_order);
  EXPECT_FALSE(in_order->affected_range.has_value());

  auto replay = observer.observe(frame_for("A", 3));
  EXPECT_TRUE(replay);
  EXPECT_EQ(replay->kind,
            codec::SequenceObservationKind::late_or_replayed);
  EXPECT_FALSE(replay->affected_range.has_value());
}

TEST(transport_recovery_sequence_observer_splits_missing_range) {
  codec::SequenceLossObserver observer;

  EXPECT_TRUE(observer.observe(frame_for("split", 0)));
  auto opened = observer.observe(frame_for("split", 5));
  EXPECT_TRUE(opened);
  EXPECT_EQ(opened->kind, codec::SequenceObservationKind::gap_opened);

  auto filled = observer.observe(frame_for("split", 3));
  EXPECT_TRUE(filled);
  EXPECT_EQ(filled->kind, codec::SequenceObservationKind::gap_filled);
  EXPECT_TRUE(filled->affected_range.has_value());
  EXPECT_EQ(filled->affected_range->begin, std::uint64_t{3});
  EXPECT_EQ(filled->affected_range->end, std::uint64_t{4});

  auto missing = observer.missing(frame_for("split", 0).stream,
                                  codec::StreamEpoch{1, 1});
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{2});
  EXPECT_EQ((*missing)[0].begin, std::uint64_t{1});
  EXPECT_EQ((*missing)[0].end, std::uint64_t{3});
  EXPECT_EQ((*missing)[1].begin, std::uint64_t{4});
  EXPECT_EQ((*missing)[1].end, std::uint64_t{5});
}

TEST(transport_recovery_sequence_observer_tracks_streams_and_epochs_independently) {
  codec::SequenceLossObserver observer;

  EXPECT_TRUE(observer.observe(frame_for("A", 10, 1, 1)));
  EXPECT_TRUE(observer.observe(frame_for("A", 13, 1, 1)));

  auto same_stream_new_epoch = observer.observe(frame_for("A", 50, 2, 1));
  EXPECT_TRUE(same_stream_new_epoch);
  EXPECT_EQ(same_stream_new_epoch->kind,
            codec::SequenceObservationKind::baseline);

  auto other_stream = observer.observe(frame_for("B", 99, 1, 1));
  EXPECT_TRUE(other_stream);
  EXPECT_EQ(other_stream->kind, codec::SequenceObservationKind::baseline);

  auto a_old = observer.missing(frame_for("A", 0).stream,
                                codec::StreamEpoch{1, 1});
  auto a_new = observer.missing(frame_for("A", 0).stream,
                                codec::StreamEpoch{2, 1});
  auto b = observer.missing(frame_for("B", 0).stream,
                            codec::StreamEpoch{1, 1});
  EXPECT_TRUE(a_old);
  EXPECT_TRUE(a_new);
  EXPECT_TRUE(b);
  EXPECT_EQ(a_old->size(), std::size_t{1});
  EXPECT_EQ(a_old->front().begin, std::uint64_t{11});
  EXPECT_EQ(a_old->front().end, std::uint64_t{13});
  EXPECT_EQ(a_new->size(), std::size_t{0});
  EXPECT_EQ(b->size(), std::size_t{0});
}

TEST(transport_recovery_sequence_observer_handles_uint64_max_without_overflow) {
  codec::SequenceLossObserver observer;
  const auto maximum = std::numeric_limits<std::uint64_t>::max();

  auto baseline = observer.observe(frame_for("max", maximum));
  EXPECT_TRUE(baseline);
  EXPECT_EQ(baseline->kind, codec::SequenceObservationKind::baseline);

  auto older = observer.observe(frame_for("max", maximum - 1));
  EXPECT_TRUE(older);
  EXPECT_EQ(older->kind,
            codec::SequenceObservationKind::late_or_replayed);

  auto missing = observer.missing(frame_for("max", 0).stream,
                                  codec::StreamEpoch{1, 1});
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{0});
}

TEST(transport_recovery_sequence_observer_enforces_track_limit_transactionally) {
  codec::SequenceLossLimits limits;
  limits.maximum_tracks = 1;
  limits.maximum_missing_ranges_per_track = 4;
  codec::SequenceLossObserver observer{limits};

  EXPECT_TRUE(observer.observe(frame_for("A", 0)));
  auto second = observer.observe(frame_for("B", 0));
  EXPECT_FALSE(second);
  EXPECT_EQ(second.error().code, codec::ErrorCode::resource_exhausted);

  auto first_missing = observer.missing(frame_for("A", 0).stream,
                                        codec::StreamEpoch{1, 1});
  auto second_missing = observer.missing(frame_for("B", 0).stream,
                                         codec::StreamEpoch{1, 1});
  EXPECT_TRUE(first_missing);
  EXPECT_TRUE(second_missing);
  EXPECT_EQ(first_missing->size(), std::size_t{0});
  EXPECT_EQ(second_missing->size(), std::size_t{0});
}

TEST(transport_recovery_sequence_observer_enforces_range_limit_transactionally) {
  codec::SequenceLossLimits limits;
  limits.maximum_tracks = 1;
  limits.maximum_missing_ranges_per_track = 1;
  codec::SequenceLossObserver observer{limits};

  EXPECT_TRUE(observer.observe(frame_for("split-bound", 0)));
  EXPECT_TRUE(observer.observe(frame_for("split-bound", 5)));

  auto split = observer.observe(frame_for("split-bound", 3));
  EXPECT_FALSE(split);
  EXPECT_EQ(split.error().code, codec::ErrorCode::resource_exhausted);

  auto missing = observer.missing(frame_for("split-bound", 0).stream,
                                  codec::StreamEpoch{1, 1});
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{1});
  EXPECT_EQ(missing->front().begin, std::uint64_t{1});
  EXPECT_EQ(missing->front().end, std::uint64_t{5});
}

TEST(transport_recovery_sequence_observer_rejects_zero_limits) {
  codec::SequenceLossLimits limits;
  limits.maximum_tracks = 0;
  codec::SequenceLossObserver observer{limits};

  auto result = observer.observe(frame_for("invalid", 0));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto missing = observer.missing(frame_for("invalid", 0).stream,
                                  codec::StreamEpoch{1, 1});
  EXPECT_FALSE(missing);
  EXPECT_EQ(missing.error().code, codec::ErrorCode::invalid_argument);
}

TEST(transport_recovery_group_tracks_out_of_order_members_and_duplicates) {
  codec::RecoveryGroupTracker tracker;
  const auto descriptor = group_for("group/A", 3, 100, 4);
  EXPECT_TRUE(tracker.begin(descriptor));

  auto initial = tracker.status(descriptor.key);
  EXPECT_TRUE(initial);
  EXPECT_EQ(initial->state, codec::RecoveryGroupState::collecting);
  EXPECT_EQ(initial->observed_source_count, std::uint64_t{0});
  EXPECT_EQ(initial->missing_ranges.size(), std::size_t{1});
  EXPECT_EQ(initial->missing_ranges[0].begin, std::uint64_t{100});
  EXPECT_EQ(initial->missing_ranges[0].end, std::uint64_t{104});

  auto one = tracker.observe(frame_for("group/A", 102));
  auto two = tracker.observe(frame_for("group/A", 100));
  auto three = tracker.observe(frame_for("group/A", 103));
  EXPECT_TRUE(one);
  EXPECT_TRUE(two);
  EXPECT_TRUE(three);
  EXPECT_EQ(one->kind, codec::RecoveryGroupFrameKind::first_observation);
  EXPECT_EQ(two->kind, codec::RecoveryGroupFrameKind::first_observation);
  EXPECT_EQ(three->kind, codec::RecoveryGroupFrameKind::first_observation);

  auto duplicate = tracker.observe(frame_for("group/A", 102));
  EXPECT_TRUE(duplicate);
  EXPECT_EQ(duplicate->kind, codec::RecoveryGroupFrameKind::duplicate);

  auto partial = tracker.status(descriptor.key);
  EXPECT_TRUE(partial);
  EXPECT_EQ(partial->observed_source_count, std::uint64_t{3});
  EXPECT_EQ(partial->missing_ranges.size(), std::size_t{1});
  EXPECT_EQ(partial->missing_ranges[0].begin, std::uint64_t{101});
  EXPECT_EQ(partial->missing_ranges[0].end, std::uint64_t{102});

  auto unrelated_stream = tracker.observe(frame_for("group/B", 101));
  auto unrelated_epoch = tracker.observe(frame_for("group/A", 101, 2, 1));
  auto unrelated_sequence = tracker.observe(frame_for("group/A", 999));
  EXPECT_TRUE(unrelated_stream);
  EXPECT_TRUE(unrelated_epoch);
  EXPECT_TRUE(unrelated_sequence);
  EXPECT_EQ(unrelated_stream->kind,
            codec::RecoveryGroupFrameKind::not_member);
  EXPECT_EQ(unrelated_epoch->kind,
            codec::RecoveryGroupFrameKind::not_member);
  EXPECT_EQ(unrelated_sequence->kind,
            codec::RecoveryGroupFrameKind::not_member);

  auto final_member = tracker.observe(frame_for("group/A", 101));
  EXPECT_TRUE(final_member);
  EXPECT_EQ(final_member->kind,
            codec::RecoveryGroupFrameKind::first_observation);

  auto complete = tracker.status(descriptor.key);
  EXPECT_TRUE(complete);
  EXPECT_EQ(complete->state, codec::RecoveryGroupState::observed_complete);
  EXPECT_EQ(complete->observed_source_count, std::uint64_t{4});
  EXPECT_EQ(complete->missing_ranges.size(), std::size_t{0});

  auto sealed = tracker.seal(descriptor.key);
  auto resealed = tracker.seal(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_TRUE(resealed);
  EXPECT_EQ(sealed->state, codec::RecoveryGroupState::sealed_complete);
  EXPECT_EQ(resealed->state, codec::RecoveryGroupState::sealed_complete);
  EXPECT_EQ(resealed->observed_source_count, sealed->observed_source_count);
  EXPECT_EQ(resealed->missing_ranges, sealed->missing_ranges);
}

TEST(transport_recovery_group_seals_exact_incomplete_ranges) {
  codec::RecoveryGroupTracker tracker;
  const auto descriptor = group_for("group/missing", 8, 7, 5);
  EXPECT_TRUE(tracker.begin(descriptor));
  EXPECT_TRUE(tracker.observe(frame_for("group/missing", 7)));
  EXPECT_TRUE(tracker.observe(frame_for("group/missing", 9)));
  EXPECT_TRUE(tracker.observe(frame_for("group/missing", 11)));

  auto sealed = tracker.seal(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_EQ(sealed->state, codec::RecoveryGroupState::sealed_incomplete);
  EXPECT_EQ(sealed->observed_source_count, std::uint64_t{3});
  EXPECT_EQ(sealed->missing_ranges.size(), std::size_t{2});
  EXPECT_EQ(sealed->missing_ranges[0].begin, std::uint64_t{8});
  EXPECT_EQ(sealed->missing_ranges[0].end, std::uint64_t{9});
  EXPECT_EQ(sealed->missing_ranges[1].begin, std::uint64_t{10});
  EXPECT_EQ(sealed->missing_ranges[1].end, std::uint64_t{11});

  auto late = tracker.observe(frame_for("group/missing", 8));
  EXPECT_FALSE(late);
  EXPECT_EQ(late.error().code, codec::ErrorCode::invalid_argument);

  auto frozen = tracker.status(descriptor.key);
  EXPECT_TRUE(frozen);
  EXPECT_EQ(frozen->state, codec::RecoveryGroupState::sealed_incomplete);
  EXPECT_EQ(frozen->observed_source_count, std::uint64_t{3});
  EXPECT_EQ(frozen->missing_ranges, sealed->missing_ranges);
}

TEST(transport_recovery_group_rejects_invalid_duplicate_and_overlap_descriptors) {
  codec::RecoveryGroupTracker tracker;

  auto zero = group_for("invalid", 1, 0, 0);
  auto zero_result = tracker.begin(zero);
  EXPECT_FALSE(zero_result);
  EXPECT_EQ(zero_result.error().code, codec::ErrorCode::invalid_argument);

  auto overflow = group_for(
      "invalid", 2, std::numeric_limits<std::uint64_t>::max(), 2);
  auto overflow_result = tracker.begin(overflow);
  EXPECT_FALSE(overflow_result);
  EXPECT_EQ(overflow_result.error().code, codec::ErrorCode::invalid_argument);

  const auto first = group_for("overlap", 10, 100, 4);
  EXPECT_TRUE(tracker.begin(first));

  auto duplicate = tracker.begin(first);
  EXPECT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code, codec::ErrorCode::invalid_argument);

  auto overlap = tracker.begin(group_for("overlap", 11, 103, 4));
  EXPECT_FALSE(overlap);
  EXPECT_EQ(overlap.error().code, codec::ErrorCode::invalid_argument);

  EXPECT_TRUE(tracker.begin(group_for("overlap", 12, 104, 2)));
  EXPECT_TRUE(tracker.begin(group_for("overlap", 13, 103, 4, 2, 1)));
  EXPECT_TRUE(tracker.begin(group_for("other", 1, 103, 4)));
}

TEST(transport_recovery_group_enforces_group_and_slot_limits) {
  codec::RecoveryGroupLimits count_limits;
  count_limits.maximum_groups = 1;
  count_limits.maximum_source_frames_per_group = 4;
  count_limits.maximum_tracked_source_slots = 4;
  codec::RecoveryGroupTracker count_tracker{count_limits};
  EXPECT_TRUE(count_tracker.begin(group_for("count", 1, 0, 2)));
  auto too_many_groups = count_tracker.begin(group_for("count", 2, 2, 1));
  EXPECT_FALSE(too_many_groups);
  EXPECT_EQ(too_many_groups.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::RecoveryGroupLimits source_limits;
  source_limits.maximum_groups = 4;
  source_limits.maximum_source_frames_per_group = 2;
  source_limits.maximum_tracked_source_slots = 8;
  codec::RecoveryGroupTracker source_tracker{source_limits};
  auto too_large = source_tracker.begin(group_for("source", 1, 0, 3));
  EXPECT_FALSE(too_large);
  EXPECT_EQ(too_large.error().code, codec::ErrorCode::resource_exhausted);

  codec::RecoveryGroupLimits slot_limits;
  slot_limits.maximum_groups = 4;
  slot_limits.maximum_source_frames_per_group = 4;
  slot_limits.maximum_tracked_source_slots = 3;
  codec::RecoveryGroupTracker slot_tracker{slot_limits};
  EXPECT_TRUE(slot_tracker.begin(group_for("slots", 1, 0, 2)));
  auto too_many_slots = slot_tracker.begin(group_for("slots", 2, 2, 2));
  EXPECT_FALSE(too_many_slots);
  EXPECT_EQ(too_many_slots.error().code,
            codec::ErrorCode::resource_exhausted);
}

TEST(transport_recovery_group_rejects_zero_limits_and_unknown_keys) {
  codec::RecoveryGroupLimits limits;
  limits.maximum_groups = 0;
  codec::RecoveryGroupTracker tracker{limits};
  const auto descriptor = group_for("zero", 1, 0, 1);

  auto begin = tracker.begin(descriptor);
  EXPECT_FALSE(begin);
  EXPECT_EQ(begin.error().code, codec::ErrorCode::invalid_argument);

  codec::RecoveryGroupTracker normal;
  auto status = normal.status(descriptor.key);
  auto seal = normal.seal(descriptor.key);
  EXPECT_FALSE(status);
  EXPECT_FALSE(seal);
  EXPECT_EQ(status.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_EQ(seal.error().code, codec::ErrorCode::invalid_argument);
}
