#include "test.hpp"

#include <codec/recovery.hpp>
#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>
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
