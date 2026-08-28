#include "test.hpp"

#include <codec/engine.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

TEST(engine_live_recording_rejects_invalid_concurrency_bounds) {
  codec::EngineConfig config;
  config.maximum_concurrent_streams = 0;
  auto zero_streams = codec::Engine::create(config);
  EXPECT_FALSE(zero_streams);
  if (!zero_streams) {
    EXPECT_EQ(zero_streams.error().code, codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_concurrent_streams = 4097;
  auto too_many_streams = codec::Engine::create(config);
  EXPECT_FALSE(too_many_streams);
  if (!too_many_streams) {
    EXPECT_EQ(too_many_streams.error().code,
              codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_queued_chunks_per_stream = 0;
  auto zero_queue = codec::Engine::create(config);
  EXPECT_FALSE(zero_queue);
  if (!zero_queue) {
    EXPECT_EQ(zero_queue.error().code, codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_queued_chunks_per_stream = 1025;
  auto too_large_queue = codec::Engine::create(config);
  EXPECT_FALSE(too_large_queue);
  if (!too_large_queue) {
    EXPECT_EQ(too_large_queue.error().code,
              codec::ErrorCode::invalid_argument);
  }
}

TEST(engine_live_recording_rejects_source_count_before_archive_creation) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto first_path = directory / "codec-e3-bound-first.bin";
  const auto second_path = directory / "codec-e3-bound-second.bin";
  const auto archive_path = directory / "codec-e3-bound.coda";
  {
    std::ofstream first(first_path, std::ios::binary | std::ios::trunc);
    first << "first";
    std::ofstream second(second_path, std::ios::binary | std::ios::trunc);
    second << "second";
  }
  std::filesystem::remove(archive_path);

  codec::EngineConfig config;
  config.maximum_concurrent_streams = 1;
  auto engine = codec::Engine::create(config);
  EXPECT_TRUE(engine);
  if (!engine) return;

  const std::vector<codec::FeedSpec> feeds{
      codec::FeedSpec{.uri = first_path.string(), .label = "first"},
      codec::FeedSpec{.uri = second_path.string(), .label = "second"},
  };
  auto result = engine->record(feeds, archive_path);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  EXPECT_FALSE(std::filesystem::exists(archive_path));

  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
  std::filesystem::remove(archive_path);
}
