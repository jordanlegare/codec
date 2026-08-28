#include "test.hpp"

#include <codec/engine.hpp>
#include <codec/inference.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

codec::StreamId stream_id(std::uint8_t seed) {
  codec::StreamId id{};
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}

codec::StreamSpec stream_spec(std::string uri, std::uint8_t seed = 80) {
  return codec::StreamSpec{
      .uri = std::move(uri),
      .descriptor = codec::StreamDescriptor{
          .id = stream_id(seed),
          .type = codec::StreamType::telemetry,
          .label = "temperature",
          .source_id = "sensor-42",
          .payload_type = "text/vnd.example.telemetry",
      },
      .preserve_source = true,
      .maximum_bytes = 1024,
  };
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void expect_invalid_stream_request(
    std::vector<codec::StreamSpec> streams,
    const std::filesystem::path& archive_path) {
  std::filesystem::remove(archive_path);
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  if (!engine) return;
  auto result = engine->record_streams(streams, archive_path);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  EXPECT_FALSE(std::filesystem::exists(archive_path));
}

}  // namespace

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
  EXPECT_EQ(report->source_records, std::uint64_t{1});

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto records = archive->records();
  EXPECT_TRUE(records);
  if (records) {
    EXPECT_EQ(records->front().type, codec::RecordType::feed_descriptor);
    EXPECT_TRUE(std::none_of(
        records->begin(), records->end(), [](const codec::RecordInfo& record) {
          return record.type == codec::RecordType::stream_descriptor;
        }));
  }
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

TEST(engine_records_a_typed_generic_stream_with_stable_identity) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto input = directory / "codec-telemetry-input.bin";
  const auto archive_path = directory / "codec-telemetry.coda";
  const auto payload = std::string{"temp_c=21.500\n"};
  {
    std::ofstream output(input, std::ios::binary);
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  std::filesystem::remove(archive_path);
  const codec::StreamDescriptor descriptor{
      .id = stream_id(70),
      .type = codec::StreamType::telemetry,
      .label = "reactor-temperature",
      .source_id = "plant-a/sensor-42",
      .payload_type = "text/vnd.example.telemetry",
  };

  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto report = engine->record_streams(
      {codec::StreamSpec{.uri = input.string(), .descriptor = descriptor}},
      archive_path);

  EXPECT_TRUE(report);
  if (report) {
    EXPECT_EQ(report->streams_recorded, std::size_t{1});
    EXPECT_EQ(report->source_bytes,
              static_cast<std::uint64_t>(payload.size()));
    EXPECT_EQ(report->source_records, std::uint64_t{1});
  }
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto records = archive->records();
  EXPECT_TRUE(records);
  if (records) {
    EXPECT_EQ(records->front().type, codec::RecordType::stream_descriptor);
    EXPECT_TRUE(std::none_of(
        records->begin(), records->end(), [](const codec::RecordInfo& record) {
          return record.type == codec::RecordType::feed_descriptor;
        }));
  }
  auto streams = archive->streams();
  EXPECT_TRUE(streams);
  if (streams) {
    EXPECT_EQ(streams->size(), std::size_t{1});
    EXPECT_EQ(streams->front().id, descriptor.id);
    EXPECT_EQ(streams->front().type, descriptor.type);
    EXPECT_EQ(streams->front().label, descriptor.label);
    EXPECT_EQ(streams->front().source_id, descriptor.source_id);
    EXPECT_EQ(streams->front().payload_type, descriptor.payload_type);
  }
  auto feeds = archive->feeds();
  EXPECT_TRUE(feeds);
  if (feeds) EXPECT_TRUE(feeds->empty());
  auto extracted = archive->extract_stream(descriptor.id);
  EXPECT_TRUE(extracted);
  if (extracted) {
    EXPECT_EQ(extracted->size(), payload.size());
    EXPECT_TRUE(std::equal(
        extracted->begin(), extracted->end(),
        reinterpret_cast<const std::byte*>(payload.data())));
  }

  std::filesystem::remove(input);
  std::filesystem::remove(archive_path);
}

TEST(generic_stream_recording_validates_before_archive_creation) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto first_input = directory / "codec-generic-validation-first.bin";
  const auto second_input = directory / "codec-generic-validation-second.bin";
  {
    std::ofstream first(first_input, std::ios::binary);
    first << "first";
    std::ofstream second(second_input, std::ios::binary);
    second << "second";
  }

  expect_invalid_stream_request({},
                                directory / "codec-stream-empty.coda");

  auto invalid = stream_spec(first_input.string());
  invalid.uri.clear();
  expect_invalid_stream_request({invalid},
                                directory / "codec-stream-uri.coda");

  invalid = stream_spec(first_input.string());
  invalid.preserve_source = false;
  expect_invalid_stream_request(
      {invalid}, directory / "codec-stream-preserve.coda");

  invalid = stream_spec(first_input.string());
  invalid.maximum_bytes = 0;
  expect_invalid_stream_request({invalid},
                                directory / "codec-stream-limit.coda");

  invalid = stream_spec(first_input.string());
  invalid.descriptor.source_id.clear();
  expect_invalid_stream_request({invalid},
                                directory / "codec-stream-source.coda");

  invalid = stream_spec(first_input.string());
  invalid.descriptor.payload_type.clear();
  expect_invalid_stream_request({invalid},
                                directory / "codec-stream-payload.coda");

  auto first = stream_spec(first_input.string());
  auto second = stream_spec(second_input.string(), 81);
  second.descriptor.id = first.descriptor.id;
  second.descriptor.label = "different-label";
  const std::vector<codec::StreamSpec> duplicate_ids{first, second};
  expect_invalid_stream_request(
      duplicate_ids, directory / "codec-stream-duplicate.coda");

  std::filesystem::remove(first_input);
  std::filesystem::remove(second_input);
}

TEST(generic_stream_recording_preserves_capture_security_boundaries) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto alias_path = directory / "codec-generic-record-alias.bin";
  const std::string sentinel = "generic source must survive";
  {
    std::ofstream output(alias_path, std::ios::binary | std::ios::trunc);
    output << sentinel;
  }
  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  auto alias = engine->record_streams(
      {stream_spec(alias_path.string())}, alias_path);
  EXPECT_FALSE(alias);
  EXPECT_EQ(read_text(alias_path), sentinel);

  const auto private_archive =
      directory / "codec-generic-private-http.coda";
  std::filesystem::remove(private_archive);
  auto denied = engine->record_streams(
      {stream_spec("http://2130706433:9/private")}, private_archive);
  EXPECT_FALSE(denied);
  if (!denied) {
    EXPECT_EQ(denied.error().code, codec::ErrorCode::unauthorized_source);
  }
  EXPECT_TRUE(std::filesystem::exists(private_archive));
  auto denied_archive = codec::CodaArchive::open(private_archive);
  EXPECT_TRUE(denied_archive);
  if (denied_archive) {
    const auto verification = denied_archive->verify();
    EXPECT_TRUE(verification.ok);
    EXPECT_FALSE(verification.finalized);
    auto prefix = denied_archive->records(
        codec::ArchiveReadPolicy::verified_prefix);
    EXPECT_TRUE(prefix);
    if (prefix) {
      EXPECT_EQ(prefix->size(), std::size_t{1});
      EXPECT_EQ(prefix->front().type,
                codec::RecordType::stream_descriptor);
    }
  }

  std::filesystem::remove(alias_path);
  std::filesystem::remove(private_archive);
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
