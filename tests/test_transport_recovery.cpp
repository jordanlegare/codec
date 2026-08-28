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
