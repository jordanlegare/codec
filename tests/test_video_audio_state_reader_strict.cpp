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

std::filesystem::path strict_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-h1-video-audio-strict-" + std::string{name});
}

codec::Pcm16State pcm(std::uint32_t rate = 1000, std::uint16_t channels = 1,
                      std::size_t frames = 10) {
  codec::Pcm16State state{
      .sample_rate = rate,
      .channels = channels,
      .samples = {},
  };
  state.samples.reserve(frames * channels);
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::uint16_t channel = 0; channel < channels; ++channel) {
      state.samples.push_back(static_cast<std::int16_t>(frame * 10 + channel));
    }
  }
  return state;
}

codec::ProvenanceProcess direct_process(std::string operation =
                                            "codec.video.pcm16.canonicalize") {
  return codec::ProvenanceProcess{
      .operation = std::move(operation),
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 10'000'000,
      .details_type =
          "application/vnd.codec.video.audio-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess hls_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.pcm16.canonicalize.hls",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 10'000'000,
      .details_type =
          "application/vnd.codec.video.hls-audio-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::StreamDescriptor video_descriptor(const codec::StreamId& stream) {
  return codec::StreamDescriptor{
      .id = stream,
      .type = codec::StreamType::video,
      .label = "camera",
      .source_id = "fixture",
      .payload_type = "video/mp4",
  };
}

codec::StreamDescriptor child_descriptor(const codec::StreamId& stream,
                                         std::string source_id =
                                             "codec.video.hls-resource") {
  return codec::StreamDescriptor{
      .id = stream,
      .type = codec::StreamType::opaque,
      .label = "camera:hls-resource-0000",
      .source_id = std::move(source_id),
      .payload_type = "application/octet-stream",
  };
}

void expect_corrupt(
    const codec::Result<std::vector<video::VerifiedVideoPcm16Audio>>& result) {
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::archive_corrupt);
}

}  // namespace

TEST(video_audio_state_reader_accepts_hls_pcm16_frontier) {
  const auto path = strict_path("hls.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-hls-parent");
  const auto child = codec::derive_stream_id("video-audio-hls-child");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  EXPECT_TRUE(writer.append_stream_descriptor(child_descriptor(child), 0));
  auto primary = writer.append(codec::RecordType::source_bytes, stream, 0,
                               10'000'000, std::array{std::byte{0x01}});
  auto secondary = writer.append(codec::RecordType::source_bytes, child, 0,
                                 10'000'000, std::array{std::byte{0x02}});
  auto encoded = codec::encode_pcm16_state(pcm());
  EXPECT_TRUE(primary);
  EXPECT_TRUE(secondary);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*primary, *secondary};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, hls_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto result = video::query_verified_video_pcm16_audio(*archive);
  EXPECT_TRUE(result);
  if (result) {
    EXPECT_EQ(result->size(), std::size_t{1});
    EXPECT_EQ(result->front().source_records.size(), std::size_t{2});
    EXPECT_EQ(result->front().source_records[0].sequence, primary->sequence);
    EXPECT_EQ(result->front().source_records[1].sequence, secondary->sequence);
  }
  std::filesystem::remove(path);
}

TEST(video_audio_state_reader_rejects_wrong_process_identity) {
  const auto path = strict_path("wrong-process.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-wrong-process");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              10'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(pcm());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      direct_process("codec.video.pcm16.resize")));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_corrupt(video::query_verified_video_pcm16_audio(*archive));
  std::filesystem::remove(path);
}

TEST(video_audio_state_writer_rejects_duplicate_subject_provenance) {
  const auto path = strict_path("duplicate-provenance.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-duplicate-provenance");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              10'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(pcm());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, direct_process()));
  auto duplicate = writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, direct_process());
  EXPECT_FALSE(duplicate);
  EXPECT_TRUE(writer.finalize());
  std::filesystem::remove(path);
}

TEST(video_audio_state_reader_rejects_invalid_hls_child_descriptor) {
  const auto path = strict_path("bad-child.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-bad-child-parent");
  const auto child = codec::derive_stream_id("video-audio-bad-child");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  EXPECT_TRUE(
      writer.append_stream_descriptor(child_descriptor(child, "wrong"), 0));
  auto primary = writer.append(codec::RecordType::source_bytes, stream, 0,
                               10'000'000, std::array{std::byte{0x01}});
  auto secondary = writer.append(codec::RecordType::source_bytes, child, 0,
                                 10'000'000, std::array{std::byte{0x02}});
  auto encoded = codec::encode_pcm16_state(pcm());
  EXPECT_TRUE(primary);
  EXPECT_TRUE(secondary);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*primary, *secondary};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, hls_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_corrupt(video::query_verified_video_pcm16_audio(*archive));
  std::filesystem::remove(path);
}

TEST(video_audio_state_reader_rejects_malformed_pcm16_payload) {
  const auto path = strict_path("malformed-pcm.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-malformed-pcm");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              10'000'000, std::array{std::byte{0x01}});
  EXPECT_TRUE(source);
  const std::array malformed{std::byte{'n'}, std::byte{'o'}, std::byte{'t'}};
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 10'000'000, malformed);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, direct_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_corrupt(video::query_verified_video_pcm16_audio(*archive));
  std::filesystem::remove(path);
}

TEST(video_audio_state_reader_rejects_interval_duration_mismatch) {
  const auto path = strict_path("duration-mismatch.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-duration-mismatch");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              11'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(pcm());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 0, 11'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, direct_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_corrupt(video::query_verified_video_pcm16_audio(*archive));
  std::filesystem::remove(path);
}

TEST(video_audio_state_reader_rejects_multiple_states_per_stream_v1) {
  const auto path = strict_path("multiple-states.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-multiple-states");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(video_descriptor(stream), 0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              20'000'000, std::array{std::byte{0x01}});
  auto encoded = codec::encode_pcm16_state(pcm());
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
      *first, codec::TruthClass::state_exact, inputs, direct_process()));
  EXPECT_TRUE(writer.append_stream_provenance(
      *second, codec::TruthClass::state_exact, inputs, direct_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto result = video::query_verified_video_pcm16_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = std::nullopt,
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 4096,
                });
  expect_corrupt(result);
  std::filesystem::remove(path);
}
