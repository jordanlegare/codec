#include "test.hpp"

#include <codec/transport.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
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

}  // namespace

TEST(transport_multiplex_round_trips_one_generic_frame) {
  const auto original = frame_for("telemetry/A", 7, std::byte{0x42});
  auto encoded = codec::encode_multiplex_frame(original);
  EXPECT_TRUE(encoded);

  codec::MultiplexDecoder decoder;
  auto decoded = decoder.push(*encoded);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->size(), std::size_t{1});
  EXPECT_EQ(decoded->front().stream, original.stream);
  EXPECT_EQ(decoded->front().sequence, original.sequence);
  EXPECT_EQ(decoded->front().epoch.connection, original.epoch.connection);
  EXPECT_EQ(decoded->front().epoch.format, original.epoch.format);
  EXPECT_EQ(decoded->front().clock.monotonic_ns, original.clock.monotonic_ns);
  EXPECT_EQ(decoded->front().clock.observed_utc_ns,
            original.clock.observed_utc_ns);
  EXPECT_EQ(decoded->front().clock.observed_utc_uncertainty_ns,
            original.clock.observed_utc_uncertainty_ns);
  EXPECT_EQ(decoded->front().clock.source_timestamp,
            original.clock.source_timestamp);
  EXPECT_EQ(decoded->front().clock.source_timebase_numerator,
            original.clock.source_timebase_numerator);
  EXPECT_EQ(decoded->front().clock.source_timebase_denominator,
            original.clock.source_timebase_denominator);
  EXPECT_EQ(decoded->front().start_ns, original.start_ns);
  EXPECT_EQ(decoded->front().end_ns, original.end_ns);
  EXPECT_EQ(decoded->front().payload, original.payload);
  EXPECT_TRUE(decoder.finish());
}
