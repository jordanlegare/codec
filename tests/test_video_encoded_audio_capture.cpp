#include "test.hpp"

#include "../src/video/ffmpeg_audio_capture.hpp"

#include <cstddef>
#include <cstdint>

namespace video = codec::profiles::video;
namespace detail = codec::profiles::video::detail;

TEST(video_encoded_audio_capture_keeps_packets_and_trims_logical_window) {
  video::FfmpegVideoIngestRequest request{};
  request.start_ns = 1'000'000'000;
  request.end_ns = 1'250'000'000;
  request.maximum_decoded_audio_bytes = 4096;
  const detail::FfmpegEncodedAudioCaptureBoundary boundary{
      .present = true,
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 8'000,
      .channels = 1,
      .decoded_frames = 2'048,
      .first_audio_ns = 4'000'000'000,
      .video_origin_ns = 4'000'000'000,
      .decoder_config = {std::byte{0x15}, std::byte{0x88}},
      .packets = {
          detail::FfmpegCapturedEncodedPacket{
              .pts_ns = 4'000'000'000,
              .dts_ns = 4'000'000'000,
              .duration_ns = 128'000'000,
              .flags = 1,
              .payload = {std::byte{0x01}, std::byte{0x02}},
          },
          detail::FfmpegCapturedEncodedPacket{
              .pts_ns = 4'128'000'000,
              .dts_ns = 4'128'000'000,
              .duration_ns = 128'000'000,
              .flags = 1,
              .payload = {std::byte{0x03}, std::byte{0x04}},
          },
      },
  };

  auto captured = detail::finalize_ffmpeg_encoded_audio_capture(request,
                                                                 boundary);
  EXPECT_TRUE(captured);
  if (!captured) return;
  EXPECT_TRUE(captured->present);
  EXPECT_FALSE(captured->error.has_value());
  EXPECT_TRUE(captured->state.has_value());
  EXPECT_EQ(captured->start_ns, std::int64_t{1'000'000'000});
  EXPECT_EQ(captured->end_ns, std::int64_t{1'250'000'000});
  if (!captured->state.has_value()) return;
  EXPECT_EQ(captured->state->decoded_frames, std::uint64_t{2'048});
  EXPECT_EQ(captured->state->trim_start_frames, std::uint64_t{0});
  EXPECT_EQ(captured->state->presentation_frames, std::uint64_t{2'000});
  EXPECT_EQ(captured->state->packets.size(), std::size_t{2});
  EXPECT_EQ(captured->state->packets[0].pts_offset_ns, std::int64_t{0});
  EXPECT_EQ(captured->state->packets[1].pts_offset_ns,
            std::int64_t{128'000'000});
  EXPECT_EQ(captured->state->packets[0].payload,
            (std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}}));

  auto encoded = video::encode_encoded_audio_state(*captured->state);
  EXPECT_TRUE(encoded);
  if (encoded) {
    EXPECT_TRUE(encoded->size() < 2'000U * sizeof(std::int16_t));
  }
}
