#include "test.hpp"

#include <codec/transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

codec::MultiplexFrame frame_for(std::string_view name,
                                std::uint64_t sequence,
                                std::byte value) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id(name);
  frame.sequence = sequence;
  frame.epoch = {.connection = 3, .format = 2};
  frame.clock = {
      .monotonic_ns = 1000 + static_cast<std::int64_t>(sequence),
      .observed_utc_ns = 2000 + static_cast<std::int64_t>(sequence),
      .observed_utc_uncertainty_ns = 7,
      .source_timestamp = static_cast<std::int64_t>(sequence * 480),
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  frame.start_ns = static_cast<std::int64_t>(sequence * 10);
  frame.end_ns = frame.start_ns + 10;
  frame.payload = {value, std::byte{0}, std::byte{0xff}};
  return frame;
}

std::vector<std::byte> encoded_frame(const codec::MultiplexFrame& frame,
                                     codec::MultiplexLimits limits = {}) {
  auto encoded = codec::encode_multiplex_frame(frame, limits);
  if (!encoded) throw std::runtime_error("test frame failed to encode");
  return *encoded;
}

void append_bytes(std::vector<std::byte>& output,
                  const std::vector<std::byte>& input) {
  output.insert(output.end(), input.begin(), input.end());
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

TEST(transport_multiplex_round_trips_one_generic_frame) {
  const auto original = frame_for("telemetry/A", 7, std::byte{0x42});
  auto encoded = codec::encode_multiplex_frame(original);
  EXPECT_TRUE(encoded);

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(*encoded);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{1});
  expect_same_frame(decoded->front(), original);
  EXPECT_TRUE(decoder.finish());
}

TEST(transport_multiplex_encoding_is_deterministic_and_versioned) {
  const auto frame = frame_for("sensor/A", 9, std::byte{0x11});
  const auto first = codec::encode_multiplex_frame(frame);
  const auto second = codec::encode_multiplex_frame(frame);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_TRUE(first->size() >= std::size_t{164});
  EXPECT_EQ((*first)[0], std::byte{'C'});
  EXPECT_EQ((*first)[1], std::byte{'M'});
  EXPECT_EQ((*first)[2], std::byte{'X'});
  EXPECT_EQ((*first)[3], std::byte{'1'});
}

TEST(transport_multiplex_round_trips_empty_payload) {
  auto original = frame_for("opaque/empty", 0, std::byte{0});
  original.payload.clear();
  auto encoded = codec::encode_multiplex_frame(original);
  EXPECT_TRUE(encoded);
  EXPECT_EQ(encoded->size(), std::size_t{164});

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(*encoded);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{1});
  expect_same_frame(decoded->front(), original);
  EXPECT_TRUE(decoder.finish());
}

TEST(transport_multiplex_encoder_rejects_invalid_metadata_and_bounds) {
  auto frame = frame_for("sensor/A", 0, std::byte{1});
  frame.end_ns = frame.start_ns - 1;
  auto inverted = codec::encode_multiplex_frame(frame);
  EXPECT_FALSE(inverted);
  EXPECT_EQ(inverted.error().code, codec::ErrorCode::invalid_argument);

  frame = frame_for("sensor/A", 0, std::byte{1});
  frame.clock.source_timebase_denominator = 0;
  auto bad_timebase = codec::encode_multiplex_frame(frame);
  EXPECT_FALSE(bad_timebase);
  EXPECT_EQ(bad_timebase.error().code, codec::ErrorCode::invalid_argument);

  codec::MultiplexLimits zero{};
  zero.maximum_payload_bytes = 0;
  auto zero_limit = codec::encode_multiplex_frame(frame_for(
      "sensor/A", 0, std::byte{1}), zero);
  EXPECT_FALSE(zero_limit);
  EXPECT_EQ(zero_limit.error().code, codec::ErrorCode::invalid_argument);

  codec::MultiplexLimits impossible{};
  impossible.maximum_payload_bytes = 1024;
  impossible.maximum_buffered_bytes = 200;
  auto impossible_limit = codec::encode_multiplex_frame(frame_for(
      "sensor/A", 0, std::byte{1}), impossible);
  EXPECT_FALSE(impossible_limit);
  EXPECT_EQ(impossible_limit.error().code, codec::ErrorCode::invalid_argument);

  codec::MultiplexLimits tiny{};
  tiny.maximum_payload_bytes = 2;
  tiny.maximum_buffered_bytes = 166;
  tiny.maximum_frames_per_push = 1;
  auto oversized = codec::encode_multiplex_frame(frame_for(
      "sensor/A", 0, std::byte{1}), tiny);
  EXPECT_FALSE(oversized);
  EXPECT_EQ(oversized.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(transport_multiplex_interleaves_independent_logical_streams) {
  const auto a7 = frame_for("A", 7, std::byte{0xa7});
  const auto b100 = frame_for("B", 100, std::byte{0xb1});
  const auto a9 = frame_for("A", 9, std::byte{0xa9});
  const auto c3 = frame_for("C", 3, std::byte{0xc3});

  std::vector<std::byte> physical;
  append_bytes(physical, encoded_frame(a7));
  append_bytes(physical, encoded_frame(b100));
  append_bytes(physical, encoded_frame(a9));
  append_bytes(physical, encoded_frame(c3));

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(physical);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{4});
  expect_same_frame((*decoded)[0], a7);
  expect_same_frame((*decoded)[1], b100);
  // A#8 is deliberately absent. E.1 preserves sequence values and does not
  // manufacture continuity or a gap event.
  expect_same_frame((*decoded)[2], a9);
  expect_same_frame((*decoded)[3], c3);
  EXPECT_EQ(decoder.buffered_bytes(), std::size_t{0});
  EXPECT_TRUE(decoder.finish());
}

TEST(transport_multiplex_accepts_one_byte_physical_chunks) {
  const auto a = frame_for("one-byte/A", 4, std::byte{0x31});
  const auto b = frame_for("one-byte/B", 88, std::byte{0x32});
  std::vector<std::byte> physical;
  append_bytes(physical, encoded_frame(a));
  append_bytes(physical, encoded_frame(b));

  codec::MultiplexDecoder decoder;
  std::vector<codec::MultiplexFrame> decoded;
  for (const auto byte : physical) {
    const std::array<std::byte, 1> one{byte};
    auto current = decoder.push(one);
    EXPECT_TRUE(current);
    decoded.insert(decoded.end(), current->begin(), current->end());
  }
  EXPECT_EQ(decoded.size(), std::size_t{2});
  expect_same_frame(decoded[0], a);
  expect_same_frame(decoded[1], b);
  EXPECT_TRUE(decoder.finish());
}

TEST(transport_multiplex_backpressure_drains_complete_frames_without_loss) {
  codec::MultiplexLimits limits{};
  limits.maximum_payload_bytes = 1024;
  limits.maximum_buffered_bytes = 4096;
  limits.maximum_frames_per_push = 1;

  const auto first = frame_for("backpressure/A", 1, std::byte{1});
  const auto second = frame_for("backpressure/B", 2, std::byte{2});
  const auto third = frame_for("backpressure/C", 3, std::byte{3});
  std::vector<std::byte> physical;
  append_bytes(physical, encoded_frame(first, limits));
  append_bytes(physical, encoded_frame(second, limits));
  append_bytes(physical, encoded_frame(third, limits));

  codec::MultiplexDecoder decoder{limits};
  auto one = decoder.push(physical);
  EXPECT_TRUE(one);
  EXPECT_EQ(one->size(), std::size_t{1});
  expect_same_frame(one->front(), first);
  EXPECT_TRUE(decoder.buffered_bytes() > 0);

  auto two = decoder.push({});
  EXPECT_TRUE(two);
  EXPECT_EQ(two->size(), std::size_t{1});
  expect_same_frame(two->front(), second);
  EXPECT_TRUE(decoder.buffered_bytes() > 0);

  auto three = decoder.push({});
  EXPECT_TRUE(three);
  EXPECT_EQ(three->size(), std::size_t{1});
  expect_same_frame(three->front(), third);
  EXPECT_EQ(decoder.buffered_bytes(), std::size_t{0});
  EXPECT_TRUE(decoder.finish());
}
