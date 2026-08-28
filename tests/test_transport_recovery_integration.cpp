#include "test.hpp"

#include <codec/recovery.hpp>
#include <codec/transport.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

codec::MultiplexFrame integration_frame(std::uint64_t sequence) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id("recovery/integration");
  frame.sequence = sequence;
  frame.epoch = {.connection = 7, .format = 3};
  frame.clock.source_timebase_numerator = 1;
  frame.clock.source_timebase_denominator = 48000;
  frame.payload = {std::byte{static_cast<unsigned char>(sequence & 0xffU)}};
  return frame;
}

}  // namespace

TEST(transport_recovery_consumes_actual_multiplex_decode_without_metadata_mutation) {
  const auto frame40 = integration_frame(40);
  const auto frame42 = integration_frame(42);
  const auto frame41 = integration_frame(41);

  auto encoded40 = codec::encode_multiplex_frame(frame40);
  auto encoded42 = codec::encode_multiplex_frame(frame42);
  auto encoded41 = codec::encode_multiplex_frame(frame41);
  EXPECT_TRUE(encoded40);
  EXPECT_TRUE(encoded42);
  EXPECT_TRUE(encoded41);

  std::vector<std::byte> wire;
  wire.insert(wire.end(), encoded40->begin(), encoded40->end());
  wire.insert(wire.end(), encoded42->begin(), encoded42->end());
  wire.insert(wire.end(), encoded41->begin(), encoded41->end());

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(wire);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{3});
  EXPECT_TRUE(decoder.finish());

  codec::SequenceLossObserver observer;
  codec::RecoveryGroupTracker groups;
  codec::RecoveryGroupDescriptor descriptor;
  descriptor.key.stream = frame40.stream;
  descriptor.key.epoch = frame40.epoch;
  descriptor.key.group_sequence = 55;
  descriptor.first_sequence = 40;
  descriptor.source_count = 3;
  EXPECT_TRUE(groups.begin(descriptor));

  const codec::MultiplexFrame originals[] = {frame40, frame42, frame41};
  const codec::SequenceObservationKind expected[] = {
      codec::SequenceObservationKind::baseline,
      codec::SequenceObservationKind::gap_opened,
      codec::SequenceObservationKind::gap_filled,
  };

  for (std::size_t index = 0; index < decoded->size(); ++index) {
    const auto& actual = (*decoded)[index];
    EXPECT_EQ(actual.stream, originals[index].stream);
    EXPECT_EQ(actual.sequence, originals[index].sequence);
    EXPECT_EQ(actual.epoch.connection, originals[index].epoch.connection);
    EXPECT_EQ(actual.epoch.format, originals[index].epoch.format);

    auto observed = observer.observe(actual);
    EXPECT_TRUE(observed);
    EXPECT_EQ(observed->kind, expected[index]);

    auto grouped = groups.observe(actual);
    EXPECT_TRUE(grouped);
    EXPECT_EQ(grouped->kind,
              codec::RecoveryGroupFrameKind::first_observation);
  }

  auto missing = observer.missing(frame40.stream, frame40.epoch);
  EXPECT_TRUE(missing);
  EXPECT_EQ(missing->size(), std::size_t{0});

  auto report = groups.status(descriptor.key);
  EXPECT_TRUE(report);
  EXPECT_EQ(report->state, codec::RecoveryGroupState::observed_complete);
  EXPECT_EQ(report->observed_source_count, std::uint64_t{3});
  EXPECT_EQ(report->missing_ranges.size(), std::size_t{0});
}
