#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path audio_reader_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-h1-video-audio-" + std::string{name});
}

codec::Pcm16State direct_pcm_state() {
  return codec::Pcm16State{
      .sample_rate = 1000,
      .channels = 1,
      .samples = {0, 100, -100, 200, -200, 300, -300, 400, -400, 0},
  };
}

codec::ProvenanceProcess direct_audio_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.pcm16.canonicalize",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 10'000'100,
      .details_type =
          "application/vnd.codec.video.audio-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

}  // namespace

TEST(video_audio_state_reader_accepts_direct_pcm16_lineage) {
  const auto path = audio_reader_path("direct.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-audio-direct");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "camera",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      100));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 100,
                              10'000'100, std::array{std::byte{0x01}});
  EXPECT_TRUE(source);
  auto encoded = codec::encode_pcm16_state(direct_pcm_state());
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_pcm16_audio_state_record_type,
                                 stream, 100, 10'000'100, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, direct_audio_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_pcm16_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1,
          .maximum_encoded_bytes = 1024,
      });
  EXPECT_TRUE(audio);
  if (audio) {
    EXPECT_EQ(audio->size(), std::size_t{1});
    EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{1000});
    EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
    EXPECT_EQ(audio->front().state.samples.size(), std::size_t{10});
    EXPECT_EQ(audio->front().source_records.size(), std::size_t{1});
    EXPECT_EQ(audio->front().source_records.front().sequence,
              source->sequence);
  }
  std::filesystem::remove(path);
}
