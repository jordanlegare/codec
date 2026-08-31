#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path review_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-h1-video-audio-review-" + std::string{name});
}

codec::Pcm16State review_pcm() {
  return codec::Pcm16State{
      .sample_rate = 1000,
      .channels = 1,
      .samples = std::vector<std::int16_t>(10U, std::int16_t{100}),
  };
}

codec::StreamDescriptor review_video_descriptor(const codec::StreamId& stream) {
  return codec::StreamDescriptor{
      .id = stream,
      .type = codec::StreamType::video,
      .label = "camera",
      .source_id = "fixture",
      .payload_type = "video/mp4",
  };
}

codec::ProvenanceProcess review_direct_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.pcm16.canonicalize",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type =
          "application/vnd.codec.video.audio-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

void expect_review_corrupt(
    const codec::Result<std::vector<video::VerifiedVideoPcm16Audio>>& result) {
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::archive_corrupt);
}

}  // namespace

TEST(video_audio_review_rejects_unprovenanced_h1_audio_state) {
  const auto path = review_path("unprovenanced.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-review-unprovenanced");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(review_video_descriptor(stream), 0));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 0,
                            10'000'000, std::array{std::byte{0x01}}));
  auto encoded = codec::encode_pcm16_state(review_pcm());
  EXPECT_TRUE(encoded);
  EXPECT_TRUE(writer.append_raw(video::video_pcm16_audio_state_record_type,
                                stream, 0, 10'000'000, *encoded));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_review_corrupt(video::query_verified_video_pcm16_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = std::nullopt,
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 4096,
                }));
  std::filesystem::remove(path);
}

TEST(video_audio_review_rejects_non_exact_h1_audio_provenance) {
  const auto path = review_path("wrong-truth.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-review-wrong-truth");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(review_video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              10'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(review_pcm());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::derived, inputs, review_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_review_corrupt(video::query_verified_video_pcm16_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = std::nullopt,
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 4096,
                }));
  std::filesystem::remove(path);
}

TEST(video_audio_review_enforces_single_state_even_with_time_filter) {
  const auto path = review_path("time-filter.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-review-time-filter");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(review_video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              20'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(review_pcm());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto first = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  auto second = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                  stream, 10'000'000, 20'000'000, *encoded);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *first, codec::TruthClass::state_exact, inputs, review_direct_process()));
  EXPECT_TRUE(writer.append_stream_provenance(
      *second, codec::TruthClass::state_exact, inputs, review_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_review_corrupt(video::query_verified_video_pcm16_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = codec::RecordTimeRange{.begin_ns = 0,
                                                   .end_ns = 10'000'000},
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 4096,
                }));
  std::filesystem::remove(path);
}
