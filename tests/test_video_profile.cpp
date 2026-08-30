#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace video = codec::profiles::video;

namespace {

video::VideoProfileDescriptor rgb24_descriptor() {
  return video::VideoProfileDescriptor{
      .coded_width = 2,
      .coded_height = 2,
      .pixel_layout = video::PixelLayout::rgb24,
      .sample_aspect_ratio_numerator = 1,
      .sample_aspect_ratio_denominator = 1,
      .nominal_frame_rate_numerator = 30,
      .nominal_frame_rate_denominator = 1,
      .color_range = video::ColorRange::full,
      .color_primaries = video::ColorPrimaries::bt709,
      .transfer = video::TransferCharacteristics::bt709,
      .matrix = video::MatrixCoefficients::bt709,
  };
}

std::vector<std::byte> vpd1_fixture() {
  return {
      std::byte{'V'}, std::byte{'P'}, std::byte{'D'}, std::byte{'1'},
      std::byte{0x01}, std::byte{0x02}, std::byte{0x02}, std::byte{0x01},
      std::byte{0x03}, std::byte{0x02}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x1e},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
  };
}

std::vector<std::byte> rgb24_pixels() {
  return {
      std::byte{0x00}, std::byte{0x10}, std::byte{0x20},
      std::byte{0x30}, std::byte{0x40}, std::byte{0x50},
      std::byte{0x60}, std::byte{0x70}, std::byte{0x80},
      std::byte{0x90}, std::byte{0xa0}, std::byte{0xb0},
  };
}

std::vector<std::byte> vfr1_fixture() {
  std::vector<std::byte> fixture{
      std::byte{'V'}, std::byte{'F'}, std::byte{'R'}, std::byte{'1'},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x24},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0c},
  };
  const auto descriptor = vpd1_fixture();
  const auto pixels = rgb24_pixels();
  fixture.insert(fixture.end(), descriptor.begin(), descriptor.end());
  fixture.insert(fixture.end(), pixels.begin(), pixels.end());
  return fixture;
}

video::RawVideoFrameState frame(video::PixelLayout layout,
                                std::size_t pixel_bytes) {
  auto descriptor = rgb24_descriptor();
  descriptor.pixel_layout = layout;
  std::vector<std::byte> pixels(pixel_bytes);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    pixels[index] = static_cast<std::byte>(index + 1U);
  }
  return video::RawVideoFrameState{
      .descriptor = descriptor,
      .pixels = std::move(pixels),
  };
}

}  // namespace

TEST(video_profile_record_codes_are_profile_owned_and_stable) {
  EXPECT_EQ(video::video_profile_descriptor_record_type,
            codec::RecordTypeCode{0x0100});
  EXPECT_EQ(video::raw_video_frame_state_record_type,
            codec::RecordTypeCode{0x0101});
}

TEST(video_profile_descriptor_encoding_matches_vpd1_golden_bytes) {
  auto encoded = video::encode_video_profile_descriptor(rgb24_descriptor());
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  EXPECT_EQ(encoded->size(), std::size_t{36});
  EXPECT_EQ(*encoded, vpd1_fixture());

  auto decoded = video::decode_video_profile_descriptor(*encoded);
  EXPECT_TRUE(decoded);
  if (decoded) EXPECT_EQ(*decoded, rgb24_descriptor());
}

TEST(video_profile_frame_encoding_matches_vfr1_golden_bytes) {
  const video::RawVideoFrameState state{
      .descriptor = rgb24_descriptor(),
      .pixels = rgb24_pixels(),
  };
  auto encoded = video::encode_raw_video_frame_state(state);
  EXPECT_TRUE(encoded);
  if (!encoded) return;
  EXPECT_EQ(*encoded, vfr1_fixture());
}

TEST(video_profile_frame_round_trips_all_supported_layouts_exactly) {
  const std::array fixtures{
      frame(video::PixelLayout::gray8, 4),
      frame(video::PixelLayout::rgb24, 12),
      frame(video::PixelLayout::rgba32, 16),
      frame(video::PixelLayout::yuv420p8, 6),
  };

  for (const auto& expected : fixtures) {
    auto first = video::encode_raw_video_frame_state(expected);
    EXPECT_TRUE(first);
    if (!first) continue;
    auto decoded = video::decode_raw_video_frame_state(*first);
    EXPECT_TRUE(decoded);
    if (!decoded) continue;
    EXPECT_EQ(*decoded, expected);
    auto second = video::encode_raw_video_frame_state(*decoded);
    EXPECT_TRUE(second);
    if (second) EXPECT_EQ(*second, *first);
  }
}

TEST(video_profile_encoding_rejects_invalid_geometry) {
  auto descriptor = rgb24_descriptor();
  descriptor.coded_width = 0;
  auto zero_width = video::encode_video_profile_descriptor(descriptor);
  EXPECT_FALSE(zero_width);
  if (!zero_width) {
    EXPECT_EQ(zero_width.error().code, codec::ErrorCode::invalid_argument);
  }

  descriptor = rgb24_descriptor();
  descriptor.sample_aspect_ratio_denominator = 0;
  EXPECT_FALSE(video::encode_video_profile_descriptor(descriptor));

  auto odd_yuv = frame(video::PixelLayout::yuv420p8, 6);
  odd_yuv.descriptor.coded_width = 3;
  EXPECT_FALSE(video::encode_raw_video_frame_state(odd_yuv));

  auto wrong_size = frame(video::PixelLayout::rgb24, 11);
  EXPECT_FALSE(video::encode_raw_video_frame_state(wrong_size));
}

TEST(video_profile_decoding_rejects_malformed_and_over_limit_payloads) {
  auto descriptor = vpd1_fixture();
  descriptor[0] = std::byte{'X'};
  EXPECT_FALSE(video::decode_video_profile_descriptor(descriptor));

  descriptor = vpd1_fixture();
  descriptor[4] = std::byte{0x02};
  EXPECT_FALSE(video::decode_video_profile_descriptor(descriptor));

  descriptor = vpd1_fixture();
  descriptor[5] = std::byte{0xff};
  EXPECT_FALSE(video::decode_video_profile_descriptor(descriptor));

  descriptor = vpd1_fixture();
  descriptor[10] = std::byte{0x01};
  EXPECT_FALSE(video::decode_video_profile_descriptor(descriptor));

  descriptor = vpd1_fixture();
  descriptor.push_back(std::byte{0x00});
  EXPECT_FALSE(video::decode_video_profile_descriptor(descriptor));

  auto limited = video::decode_video_profile_descriptor(
      vpd1_fixture(), video::VideoDecodeLimits{
                          .maximum_width = 1,
                          .maximum_height = 16384,
                          .maximum_pixels = 268435456,
                          .maximum_payload_bytes = 1024,
                      });
  EXPECT_FALSE(limited);
  if (!limited) {
    EXPECT_EQ(limited.error().code, codec::ErrorCode::resource_exhausted);
  }

  auto frame_bytes = vfr1_fixture();
  frame_bytes[6] = std::byte{0x01};
  EXPECT_FALSE(video::decode_raw_video_frame_state(frame_bytes));

  frame_bytes = vfr1_fixture();
  frame_bytes[11] = std::byte{0x23};
  EXPECT_FALSE(video::decode_raw_video_frame_state(frame_bytes));

  frame_bytes = vfr1_fixture();
  frame_bytes[19] = std::byte{0x0d};
  EXPECT_FALSE(video::decode_raw_video_frame_state(frame_bytes));

  frame_bytes = vfr1_fixture();
  frame_bytes.pop_back();
  EXPECT_FALSE(video::decode_raw_video_frame_state(frame_bytes));

  frame_bytes = vfr1_fixture();
  frame_bytes.push_back(std::byte{0x00});
  EXPECT_FALSE(video::decode_raw_video_frame_state(frame_bytes));
}
