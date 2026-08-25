#include "test.hpp"

#include <codec/engine.hpp>
#include <codec/inference.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

TEST(engine_records_a_file_feed_and_preserves_its_bytes) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto input = directory / "codec-engine-input.bin";
  const auto archive_path = directory / "codec-engine.coda";
  const auto payload = std::string{"preserve-first\nwith another line\n"};
  {
    std::ofstream output(input, std::ios::binary);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  std::filesystem::remove(archive_path);

  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record({codec::FeedSpec{.uri = input.string(),
                                                .label = "local-test"}},
                               archive_path);
  EXPECT_TRUE(report);
  EXPECT_EQ(report->feeds_recorded, std::size_t{1});
  EXPECT_EQ(report->source_bytes, static_cast<std::uint64_t>(payload.size()));

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto feeds = archive->feeds();
  EXPECT_TRUE(feeds);
  EXPECT_EQ(feeds->size(), std::size_t{1});
  EXPECT_EQ(feeds->front().label, std::string{"local-test"});
  auto streams = archive->streams();
  EXPECT_TRUE(streams);
  EXPECT_EQ(streams->size(), std::size_t{1});
  EXPECT_EQ(streams->front().id, feeds->front().stream);
  EXPECT_EQ(streams->front().type, codec::StreamType::opaque);
  EXPECT_EQ(streams->front().label, std::string{"local-test"});
  EXPECT_EQ(streams->front().source_id, input.string());
  EXPECT_TRUE(streams->front().payload_type.empty());
  auto extracted = archive->extract_feed("local-test");
  EXPECT_TRUE(extracted);
  EXPECT_EQ(extracted->size(), payload.size());
  EXPECT_TRUE(std::equal(extracted->begin(), extracted->end(),
                         reinterpret_cast<const std::byte*>(payload.data())));

  std::filesystem::remove(input);
  std::filesystem::remove(archive_path);
}

TEST(capabilities_never_claim_an_unloaded_neural_backend) {
  const auto capabilities = codec::Engine::capabilities();
  EXPECT_TRUE(capabilities.coda_archive);
  EXPECT_TRUE(capabilities.w0_ed25519);
  EXPECT_TRUE(capabilities.w1_reference);
  EXPECT_FALSE(capabilities.neural_separation);
  EXPECT_FALSE(capabilities.gpu_inference);
}

TEST(default_separation_backend_fails_explicitly_without_model_weights) {
  auto backend = codec::default_separation_backend();
  EXPECT_FALSE(backend->available());
  EXPECT_EQ(backend->name(), std::string{"unavailable"});
  codec::SeparationRequest request;
  request.mixture.sample_rate = 48000;
  request.mixture.channels = 1;
  request.mixture.samples.assign(4800, 0);
  request.maximum_sources = 4;
  auto result = backend->separate(request);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::model_incompatible);
}

TEST(http_capture_rejects_numeric_loopback_before_connecting) {
  const auto archive_path =
      std::filesystem::temp_directory_path() / "codec-private-http.coda";
  std::filesystem::remove(archive_path);
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record(
      {codec::FeedSpec{.uri = "http://2130706433:9/private",
                       .label = "private"}},
      archive_path);
  EXPECT_FALSE(report);
  EXPECT_EQ(report.error().code, codec::ErrorCode::unauthorized_source);
  std::filesystem::remove(archive_path);
}

TEST(recording_refuses_when_archive_output_is_the_feed_input) {
  const auto path =
      std::filesystem::temp_directory_path() / "codec-record-alias.bin";
  const std::string sentinel = "source must survive";
  {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << sentinel;
  }
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record(
      {codec::FeedSpec{.uri = path.string(), .label = "alias"}}, path);
  EXPECT_FALSE(report);
  std::ifstream input(path, std::ios::binary);
  std::string actual((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  EXPECT_EQ(actual, sentinel);
  std::filesystem::remove(path);
}

TEST(recording_refuses_a_dangling_feed_symlink_to_the_future_archive) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto archive_path = directory / "codec-future-archive.coda";
  const auto source_link = directory / "codec-future-source-link";
  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_link);
  std::filesystem::create_symlink(archive_path, source_link);
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record(
      {codec::FeedSpec{.uri = source_link.string(),
                       .label = "future-alias",
                       .maximum_bytes = 1024}},
      archive_path);
  EXPECT_FALSE(report);
  EXPECT_FALSE(std::filesystem::exists(archive_path));
  std::filesystem::remove(source_link);
  std::filesystem::remove(archive_path);
}
