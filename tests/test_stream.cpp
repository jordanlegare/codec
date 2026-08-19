#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/engine.hpp>
#include <codec/stream.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

static_assert(std::is_same_v<std::underlying_type_t<codec::StreamType>,
                             std::uint16_t>);
static_assert(std::is_same_v<std::underlying_type_t<codec::TruthClass>,
                             std::uint8_t>);

TEST(generic_stream_metadata_is_profile_neutral) {
  codec::StreamDescriptor descriptor;
  descriptor.id = codec::derive_stream_id("telemetry/device-42");
  descriptor.type = codec::StreamType::telemetry;
  descriptor.label = "device-42";
  descriptor.source_id = "sensor-bus-7";
  descriptor.payload_type = "application/vnd.example.telemetry";

  EXPECT_EQ(descriptor.type, codec::StreamType::telemetry);
  EXPECT_EQ(descriptor.label, std::string{"device-42"});
  EXPECT_EQ(descriptor.source_id, std::string{"sensor-bus-7"});
  EXPECT_EQ(descriptor.payload_type,
            std::string{"application/vnd.example.telemetry"});
  EXPECT_EQ(codec::to_string(descriptor.id).size(), std::size_t{36});
}

TEST(stream_clock_and_epochs_keep_generic_timing_dimensions_separate) {
  codec::StreamClock clock;
  clock.monotonic_ns = 100;
  clock.observed_utc_ns = 200;
  clock.observed_utc_uncertainty_ns = 5;
  clock.source_timestamp = 9000;
  clock.source_timebase_numerator = 1;
  clock.source_timebase_denominator = 1000;

  codec::StreamEpoch epoch;
  epoch.connection = 3;
  epoch.format = 2;

  EXPECT_EQ(clock.monotonic_ns, std::int64_t{100});
  EXPECT_EQ(clock.observed_utc_ns, std::int64_t{200});
  EXPECT_EQ(clock.observed_utc_uncertainty_ns, std::uint64_t{5});
  EXPECT_EQ(clock.source_timestamp, std::int64_t{9000});
  EXPECT_EQ(clock.source_timebase_numerator, std::int64_t{1});
  EXPECT_EQ(clock.source_timebase_denominator, std::int64_t{1000});
  EXPECT_EQ(epoch.connection, std::uint64_t{3});
  EXPECT_EQ(epoch.format, std::uint64_t{2});
}

TEST(truth_classes_remain_explicit_and_distinct) {
  EXPECT_TRUE(codec::TruthClass::source_exact != codec::TruthClass::state_exact);
  EXPECT_TRUE(codec::TruthClass::source_exact != codec::TruthClass::derived);
  EXPECT_TRUE(codec::TruthClass::state_exact != codec::TruthClass::derived);
}

TEST(existing_feed_api_remains_available_during_stream_migration) {
  codec::FeedSpec feed;
  feed.uri = "/tmp/source.bin";
  feed.label = "compat";
  feed.preserve_source = true;

  EXPECT_EQ(feed.label, std::string{"compat"});
  EXPECT_TRUE(feed.preserve_source);
}
