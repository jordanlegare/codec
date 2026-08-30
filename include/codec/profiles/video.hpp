#pragma once

#include <codec/archive.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec::profiles::video {

inline constexpr RecordTypeCode video_profile_descriptor_record_type = 0x0100;
inline constexpr RecordTypeCode raw_video_frame_state_record_type = 0x0101;

enum class PixelLayout : std::uint8_t {
  gray8 = 1,
  rgb24 = 2,
  rgba32 = 3,
  yuv420p8 = 4,
};

enum class ColorRange : std::uint8_t {
  unspecified = 0,
  limited = 1,
  full = 2,
};

enum class ColorPrimaries : std::uint8_t {
  unspecified = 0,
  bt709 = 1,
  bt2020 = 2,
};

enum class TransferCharacteristics : std::uint8_t {
  unspecified = 0,
  linear = 1,
  srgb = 2,
  bt709 = 3,
  pq = 4,
  hlg = 5,
};

enum class MatrixCoefficients : std::uint8_t {
  unspecified = 0,
  identity = 1,
  bt709 = 2,
  bt2020_ncl = 3,
};

struct VideoDecodeLimits {
  std::uint32_t maximum_width{16384};
  std::uint32_t maximum_height{16384};
  std::uint64_t maximum_pixels{268435456};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VideoProfileDescriptor {
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  PixelLayout pixel_layout{PixelLayout::gray8};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint32_t nominal_frame_rate_numerator{};
  std::uint32_t nominal_frame_rate_denominator{1};
  ColorRange color_range{ColorRange::unspecified};
  ColorPrimaries color_primaries{ColorPrimaries::unspecified};
  TransferCharacteristics transfer{TransferCharacteristics::unspecified};
  MatrixCoefficients matrix{MatrixCoefficients::unspecified};
  auto operator<=>(const VideoProfileDescriptor&) const = default;
};

struct RawVideoFrameState {
  VideoProfileDescriptor descriptor;
  std::vector<std::byte> pixels;
  bool operator==(const RawVideoFrameState&) const = default;
};

Result<std::vector<std::byte>> encode_video_profile_descriptor(
    const VideoProfileDescriptor& descriptor);
Result<VideoProfileDescriptor> decode_video_profile_descriptor(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_raw_video_frame_state(
    const RawVideoFrameState& frame);
Result<RawVideoFrameState> decode_raw_video_frame_state(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});

}  // namespace codec::profiles::video
