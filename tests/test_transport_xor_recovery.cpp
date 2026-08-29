#include "test.hpp"

#include <codec/integrity.hpp>
#include <codec/xor_recovery.hpp>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

codec::MultiplexFrame source_frame(std::uint64_t sequence,
                                   std::size_t payload_size) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id("recovery/xor/e4");
  frame.sequence = sequence;
  frame.epoch = {.connection = 7, .format = 3};
  frame.clock = {
      .monotonic_ns = 1000 + static_cast<std::int64_t>(sequence),
      .observed_utc_ns = 2000 + static_cast<std::int64_t>(sequence),
      .observed_utc_uncertainty_ns = 9,
      .source_timestamp = static_cast<std::int64_t>(sequence * 480),
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  frame.start_ns = static_cast<std::int64_t>(sequence * 10);
  frame.end_ns = frame.start_ns + 10;
  frame.payload.resize(payload_size);
  for (std::size_t index = 0; index < payload_size; ++index) {
    frame.payload[index] = static_cast<std::byte>(
        static_cast<unsigned char>((sequence + index * 17U) & 0xffU));
  }
  return frame;
}

std::vector<codec::MultiplexFrame> source_group() {
  return {
      source_frame(40, 0),
      source_frame(41, 1),
      source_frame(42, 17),
      source_frame(43, 257),
  };
}

codec::RecoveryGroupDescriptor descriptor_for(
    const std::vector<codec::MultiplexFrame>& frames) {
  codec::RecoveryGroupDescriptor descriptor;
  descriptor.key.stream = frames.front().stream;
  descriptor.key.epoch = frames.front().epoch;
  descriptor.key.group_sequence = 12;
  descriptor.first_sequence = frames.front().sequence;
  descriptor.source_count = frames.size();
  return descriptor;
}

std::vector<std::byte> exact_encoding(const codec::MultiplexFrame& frame) {
  auto encoded = codec::encode_multiplex_frame(frame);
  if (!encoded) throw std::runtime_error("test CMX1 frame failed to encode");
  return *encoded;
}

void expect_same_frame(const codec::MultiplexFrame& actual,
                       const codec::MultiplexFrame& expected) {
  EXPECT_EQ(actual.stream, expected.stream);
  EXPECT_EQ(actual.sequence, expected.sequence);
  EXPECT_EQ(actual.epoch.connection, expected.epoch.connection);
  EXPECT_EQ(actual.epoch.format, expected.epoch.format);
  EXPECT_EQ(actual.clock.monotonic_ns, expected.clock.monotonic_ns);
  EXPECT_EQ(actual.clock.observed_utc_ns, expected.clock.observed_utc_ns);
  EXPECT_EQ(actual.clock.observed_utc_uncertainty_ns,
            expected.clock.observed_utc_uncertainty_ns);
  EXPECT_EQ(actual.clock.source_timestamp, expected.clock.source_timestamp);
  EXPECT_EQ(actual.clock.source_timebase_numerator,
            expected.clock.source_timebase_numerator);
  EXPECT_EQ(actual.clock.source_timebase_denominator,
            expected.clock.source_timebase_denominator);
  EXPECT_EQ(actual.start_ns, expected.start_ns);
  EXPECT_EQ(actual.end_ns, expected.end_ns);
  EXPECT_EQ(actual.payload, expected.payload);
}

}  // namespace

TEST(transport_xor_recovery_round_trips_symbol_and_one_erasure) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);

  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  auto encoded = codec::encode_xor_repair_symbol(*symbol);
  EXPECT_TRUE(encoded);
  auto decoded = codec::decode_xor_repair_symbol(*encoded);
  EXPECT_TRUE(decoded);

  const std::vector<codec::MultiplexFrame> observed{
      frames[0], frames[2], frames[3]};
  auto recovered = codec::recover_xor_single_erasure(*decoded, observed);
  EXPECT_TRUE(recovered);
  expect_same_frame(recovered->frame, frames[1]);
  EXPECT_EQ(recovered->encoded_frame, exact_encoding(frames[1]));
  EXPECT_EQ(recovered->encoded_frame_hash,
            codec::sha256(recovered->encoded_frame));
}
