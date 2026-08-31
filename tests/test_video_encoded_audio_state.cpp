#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace video = codec::profiles::video;

namespace {

video::EncodedAudioState one_packet_state() {
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
          .payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                      std::byte{0x04}},
      }},
  };
}

void set_u32(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    bytes[offset++] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void set_u64(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes[offset++] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

}  // namespace

TEST(video_encoded_audio_state_round_trips_exact_packet_bytes) {
  const video::EncodedAudioState state{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 48'000,
      .channels = 2,
      .decoded_frames = 2'048,
      .trim_start_frames = 0,
      .presentation_frames = 2'048,
      .decoder_config = {std::byte{0x12}, std::byte{0x10}},
      .packets = {
          video::EncodedAudioPacket{
              .pts_offset_ns = 0,
              .dts_offset_ns = 0,
              .duration_ns = 21'333'333,
              .flags = 1,
              .payload = {std::byte{0x01}, std::byte{0x80}, std::byte{0xff}},
          },
          video::EncodedAudioPacket{
              .pts_offset_ns = 21'333'333,
              .dts_offset_ns = 21'333'333,
              .duration_ns = 21'333'334,
              .flags = 0,
              .payload = {std::byte{0x7f}, std::byte{0x00}},
          },
      },
  };

  auto encoded = video::encode_encoded_audio_state(state);
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  EXPECT_EQ(encoded->size(), std::size_t{135});
  EXPECT_EQ((*encoded)[0], std::byte{'E'});
  EXPECT_EQ((*encoded)[1], std::byte{'A'});
  EXPECT_EQ((*encoded)[2], std::byte{'P'});
  EXPECT_EQ((*encoded)[3], std::byte{'1'});

  auto decoded = video::decode_encoded_audio_state(*encoded);
  EXPECT_TRUE(decoded);
  if (!decoded) return;
  EXPECT_EQ(decoded->codec, video::EncodedAudioCodec::aac);
  EXPECT_EQ(decoded->codec_profile, std::int32_t{1});
  EXPECT_EQ(decoded->sample_rate, std::uint32_t{48'000});
  EXPECT_EQ(decoded->channels, std::uint16_t{2});
  EXPECT_EQ(decoded->decoded_frames, std::uint64_t{2'048});
  EXPECT_EQ(decoded->trim_start_frames, std::uint64_t{0});
  EXPECT_EQ(decoded->presentation_frames, std::uint64_t{2'048});
  EXPECT_EQ(decoded->decoder_config,
            (std::vector<std::byte>{std::byte{0x12}, std::byte{0x10}}));
  EXPECT_EQ(decoded->packets.size(), std::size_t{2});
  EXPECT_EQ(decoded->packets[0].pts_offset_ns, std::int64_t{0});
  EXPECT_EQ(decoded->packets[0].duration_ns, std::uint64_t{21'333'333});
  EXPECT_EQ(decoded->packets[0].flags, std::uint32_t{1});
  EXPECT_EQ(decoded->packets[0].payload,
            (std::vector<std::byte>{std::byte{0x01}, std::byte{0x80},
                                    std::byte{0xff}}));
  EXPECT_EQ(decoded->packets[1].pts_offset_ns,
            std::int64_t{21'333'333});
  EXPECT_EQ(decoded->packets[1].dts_offset_ns,
            std::int64_t{21'333'333});
  EXPECT_EQ(decoded->packets[1].duration_ns, std::uint64_t{21'333'334});
  EXPECT_EQ(decoded->packets[1].payload,
            (std::vector<std::byte>{std::byte{0x7f}, std::byte{0x00}}));
}

TEST(video_encoded_audio_state_rejects_trailing_bytes) {
  video::EncodedAudioState state{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 8'000,
      .channels = 1,
      .decoded_frames = 1'024,
      .trim_start_frames = 0,
      .presentation_frames = 1'024,
      .decoder_config = {},
      .packets = {video::EncodedAudioPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 128'000'000,
          .flags = 1,
          .payload = {std::byte{0x01}},
      }},
  };
  auto encoded = video::encode_encoded_audio_state(state);
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  encoded->push_back(std::byte{0x00});
  EXPECT_FALSE(video::decode_encoded_audio_state(*encoded));
}

TEST(video_encoded_audio_state_enforces_packet_count_limit) {
  const video::EncodedAudioState state{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 8'000,
      .channels = 1,
      .decoded_frames = 2'048,
      .trim_start_frames = 0,
      .presentation_frames = 2'048,
      .decoder_config = {},
      .packets = {
          video::EncodedAudioPacket{.pts_offset_ns = 0,
                                    .dts_offset_ns = 0,
                                    .duration_ns = 128'000'000,
                                    .flags = 1,
                                    .payload = {std::byte{0x01}}},
          video::EncodedAudioPacket{.pts_offset_ns = 128'000'000,
                                    .dts_offset_ns = 128'000'000,
                                    .duration_ns = 128'000'000,
                                    .flags = 1,
                                    .payload = {std::byte{0x02}}},
      },
  };
  auto encoded = video::encode_encoded_audio_state(state);
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  auto decoded = video::decode_encoded_audio_state(
      *encoded,
      video::EncodedAudioDecodeLimits{
          .maximum_packets = 1,
          .maximum_decoder_config_bytes = 1024,
          .maximum_packet_bytes = 1024,
          .maximum_payload_bytes = 1024,
      });
  EXPECT_FALSE(decoded);
  if (!decoded) {
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }
}

TEST(video_encoded_audio_state_rejects_unknown_packet_flags) {
  const video::EncodedAudioState state{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 48'000,
      .channels = 2,
      .decoded_frames = 1'024,
      .trim_start_frames = 0,
      .presentation_frames = 1'024,
      .decoder_config = {std::byte{0x12}, std::byte{0x10}},
      .packets = {video::EncodedAudioPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 21'333'333,
          .flags = 0x80000000U,
          .payload = {std::byte{0x01}},
      }},
  };

  auto encoded = video::encode_encoded_audio_state(state);
  EXPECT_FALSE(encoded);
  if (!encoded) {
    EXPECT_EQ(encoded.error().code, codec::ErrorCode::invalid_argument);
  }
}

TEST(video_encoded_audio_state_rejects_regressing_packet_dts) {
  const video::EncodedAudioState state{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 48'000,
      .channels = 1,
      .decoded_frames = 2'048,
      .trim_start_frames = 0,
      .presentation_frames = 2'048,
      .decoder_config = {std::byte{0x11}, std::byte{0x88}},
      .packets = {
          video::EncodedAudioPacket{
              .pts_offset_ns = 21'333'333,
              .dts_offset_ns = 21'333'333,
              .duration_ns = 21'333'333,
              .flags = 1,
              .payload = {std::byte{0x01}},
          },
          video::EncodedAudioPacket{
              .pts_offset_ns = 0,
              .dts_offset_ns = 0,
              .duration_ns = 21'333'333,
              .flags = 1,
              .payload = {std::byte{0x02}},
          },
      },
  };

  auto encoded = video::encode_encoded_audio_state(state);
  EXPECT_FALSE(encoded);
  if (!encoded) {
    EXPECT_EQ(encoded.error().code, codec::ErrorCode::invalid_argument);
  }
}

TEST(video_encoded_audio_state_enforces_running_payload_limit_before_copy) {
  auto encoded = video::encode_encoded_audio_state(one_packet_state());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  set_u64(*encoded, 56U, 1U);

  auto decoded = video::decode_encoded_audio_state(
      *encoded,
      video::EncodedAudioDecodeLimits{
          .maximum_packets = 8,
          .maximum_decoder_config_bytes = 16,
          .maximum_packet_bytes = 16,
          .maximum_payload_bytes = 1,
      });
  EXPECT_FALSE(decoded);
  if (!decoded) {
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }
}

TEST(video_encoded_audio_state_rejects_impossible_declared_packet_count) {
  auto encoded = video::encode_encoded_audio_state(one_packet_state());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  set_u32(*encoded, 20U, 1'000'000U);

  auto decoded = video::decode_encoded_audio_state(
      *encoded,
      video::EncodedAudioDecodeLimits{
          .maximum_packets = 1'000'000,
          .maximum_decoder_config_bytes = 16,
          .maximum_packet_bytes = 16,
          .maximum_payload_bytes = 16,
      });
  EXPECT_FALSE(decoded);
  if (!decoded) {
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::decode);
  }
}

TEST(video_encoded_audio_state_rejects_reserved_header_bits) {
  auto encoded = video::encode_encoded_audio_state(one_packet_state());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  (*encoded)[18] = std::byte{0x01};
  EXPECT_FALSE(video::decode_encoded_audio_state(*encoded));
}

TEST(video_encoded_audio_state_enforces_individual_packet_limit) {
  auto encoded = video::encode_encoded_audio_state(one_packet_state());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  auto decoded = video::decode_encoded_audio_state(
      *encoded,
      video::EncodedAudioDecodeLimits{
          .maximum_packets = 8,
          .maximum_decoder_config_bytes = 16,
          .maximum_packet_bytes = 3,
          .maximum_payload_bytes = 16,
      });
  EXPECT_FALSE(decoded);
  if (!decoded) {
    EXPECT_EQ(decoded.error().code, codec::ErrorCode::resource_exhausted);
  }
}

TEST(video_encoded_audio_state_rejects_truncated_packet_payload) {
  auto encoded = video::encode_encoded_audio_state(one_packet_state());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  encoded->pop_back();
  auto decoded = video::decode_encoded_audio_state(*encoded);
  EXPECT_FALSE(decoded);
  if (!decoded) EXPECT_EQ(decoded.error().code, codec::ErrorCode::decode);
}

TEST(video_encoded_audio_state_rejects_packet_outside_presentation) {
  auto state = one_packet_state();
  state.packets.front().pts_offset_ns = 128'000'000;
  state.packets.front().dts_offset_ns = 128'000'000;
  EXPECT_FALSE(video::encode_encoded_audio_state(state));
}

TEST(video_encoded_audio_state_rejects_packet_timestamp_overflow) {
  auto state = one_packet_state();
  state.packets.front().pts_offset_ns =
      std::numeric_limits<std::int64_t>::max() - 1;
  state.packets.front().dts_offset_ns =
      std::numeric_limits<std::int64_t>::max() - 1;
  EXPECT_FALSE(video::encode_encoded_audio_state(state));
}
