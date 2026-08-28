#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path validation_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d6-validation-" + std::string{name});
}

codec::profiles::audio::Pcm16FlacIngestRequest valid_shape(
    const std::filesystem::path& archive_path) {
  return codec::profiles::audio::Pcm16FlacIngestRequest{
      .source_uri = "/codec-d6-source-must-not-be-opened-before-validation.flac",
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = codec::derive_stream_id("d6-validation-complete"),
          .type = codec::StreamType::audio,
          .label = "D.6 validation",
          .source_id = "fixture",
          .payload_type = "audio/flac",
      },
      .start_ns = 100,
      .end_ns = 200,
      .capture_chunk_bytes = 256U * 1024U,
      .maximum_source_bytes = 1024ULL * 1024ULL,
      .maximum_decoded_pcm_bytes = 1024ULL * 1024ULL,
      .maximum_redirects = 5,
      .deny_private_network = true,
  };
}

void expect_invalid_without_output(
    const codec::profiles::audio::Pcm16FlacIngestRequest& request) {
  std::filesystem::remove(request.archive_path);
  auto result = codec::profiles::audio::ingest_pcm16_flac(request);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(request.archive_path));
}

}  // namespace

TEST(audio_flac_ingest_rejects_non_audio_descriptor_before_capture) {
  const auto archive = validation_path("non-audio.coda");
  auto request = valid_shape(archive);
  request.descriptor.type = codec::StreamType::opaque;
  expect_invalid_without_output(request);
}

TEST(audio_flac_ingest_rejects_inverted_interval_before_capture) {
  const auto archive = validation_path("interval.coda");
  auto request = valid_shape(archive);
  request.start_ns = 201;
  request.end_ns = 200;
  expect_invalid_without_output(request);
}

TEST(audio_flac_ingest_rejects_invalid_capture_chunk_before_capture) {
  const auto archive = validation_path("chunk.coda");
  auto request = valid_shape(archive);
  request.capture_chunk_bytes = 1;
  expect_invalid_without_output(request);
}

TEST(audio_flac_ingest_rejects_zero_source_bound_before_capture) {
  const auto archive = validation_path("source-bound.coda");
  auto request = valid_shape(archive);
  request.maximum_source_bytes = 0;
  expect_invalid_without_output(request);
}

TEST(audio_flac_ingest_rejects_excessive_redirects_before_capture) {
  const auto archive = validation_path("redirects.coda");
  auto request = valid_shape(archive);
  request.maximum_redirects = 21;
  expect_invalid_without_output(request);
}
