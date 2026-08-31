#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path encoded_reader_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-reader-" + std::string{name});
}

codec::ProvenanceProcess encoded_direct_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.encoded-audio.preserve",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.encoded-audio.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess encoded_hls_process() {
  auto process = encoded_direct_process();
  process.operation = "codec.video.encoded-audio.preserve.hls";
  process.details_type = "application/vnd.codec.video.hls-encoded-audio.v1";
  return process;
}

video::EncodedAudioState encoded_reader_state() {
  return video::EncodedAudioState{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 8'000,
      .channels = 1,
      .decoded_frames = 1'024,
      .trim_start_frames = 0,
      .presentation_frames = 1'024,
      .decoder_config = {std::byte{0x15}, std::byte{0x88}},
      .packets = {video::EncodedAudioPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 128'000'000,
          .flags = 1,
          .payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},
      }},
  };
}

void expect_encoded_reader_corrupt(
    const codec::Result<std::vector<video::VerifiedVideoEncodedAudio>>& result) {
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::archive_corrupt);
}

}  // namespace

TEST(video_encoded_audio_reader_accepts_exact_direct_lineage) {
  const auto path = encoded_reader_path("direct.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-reader-direct");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "camera",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 4096,
      });
  EXPECT_TRUE(verified);
  if (verified) {
    EXPECT_EQ(verified->size(), std::size_t{1});
    if (!verified->empty()) {
      EXPECT_EQ(verified->front().state, encoded_reader_state());
      EXPECT_EQ(verified->front().state_record.hash, state->hash);
      EXPECT_EQ(verified->front().source_records.size(), std::size_t{1});
      EXPECT_EQ(verified->front().source_records.front().hash, source->hash);
      EXPECT_EQ(verified->front().provenance.process.operation,
                std::string{"codec.video.encoded-audio.preserve"});
    }
  }
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_accepts_ordered_hls_frontier) {
  const auto path = encoded_reader_path("hls.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-reader-hls");
  const auto child = codec::derive_stream_id("encoded-audio-reader-hls-child");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "application/vnd.apple.mpegurl"},
      0));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = child,
                              .type = codec::StreamType::opaque,
                              .label = "resource",
                              .source_id = "codec.video.hls-resource",
                              .payload_type = "video/mp2t"},
      0));
  auto primary = writer.append(codec::RecordType::source_bytes, stream, 0,
                               128'000'000,
                               std::array{std::byte{0xaa}});
  auto secondary = writer.append(codec::RecordType::source_bytes, child, 0,
                                 128'000'000,
                                 std::array{std::byte{0xbb}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(primary);
  EXPECT_TRUE(secondary);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*primary, *secondary};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_hls_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096});
  EXPECT_TRUE(verified);
  if (verified && !verified->empty()) {
    EXPECT_EQ(verified->front().source_records.size(), std::size_t{2});
    EXPECT_EQ(verified->front().source_records[0].hash, primary->hash);
    EXPECT_EQ(verified->front().source_records[1].hash, secondary->hash);
    EXPECT_EQ(verified->front().provenance.process.operation,
              std::string{"codec.video.encoded-audio.preserve.hls"});
  }
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_wrong_process_identity) {
  const auto path = encoded_reader_path("wrong-process.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-wrong-process");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  auto process = encoded_direct_process();
  process.operation = "codec.video.pcm16.canonicalize";
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, process));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096}));
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_interval_mismatch) {
  const auto path = encoded_reader_path("interval.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-interval");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 127'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096}));
  std::filesystem::remove(path);
}
