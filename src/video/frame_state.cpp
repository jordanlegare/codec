#include <codec/profiles/video.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::video {
namespace {

constexpr std::size_t kVpd1Size = 36;
constexpr std::size_t kVfr1HeaderSize = 20;

bool valid_pixel_layout(PixelLayout value) {
  switch (value) {
    case PixelLayout::gray8:
    case PixelLayout::rgb24:
    case PixelLayout::rgba32:
    case PixelLayout::yuv420p8:
      return true;
  }
  return false;
}

bool valid_color_range(ColorRange value) {
  switch (value) {
    case ColorRange::unspecified:
    case ColorRange::limited:
    case ColorRange::full:
      return true;
  }
  return false;
}

bool valid_color_primaries(ColorPrimaries value) {
  switch (value) {
    case ColorPrimaries::unspecified:
    case ColorPrimaries::bt709:
    case ColorPrimaries::bt2020:
      return true;
  }
  return false;
}

bool valid_transfer(TransferCharacteristics value) {
  switch (value) {
    case TransferCharacteristics::unspecified:
    case TransferCharacteristics::linear:
    case TransferCharacteristics::srgb:
    case TransferCharacteristics::bt709:
    case TransferCharacteristics::pq:
    case TransferCharacteristics::hlg:
      return true;
  }
  return false;
}

bool valid_matrix(MatrixCoefficients value) {
  switch (value) {
    case MatrixCoefficients::unspecified:
    case MatrixCoefficients::identity:
    case MatrixCoefficients::bt709:
    case MatrixCoefficients::bt2020_ncl:
      return true;
  }
  return false;
}

Result<std::uint64_t> invalid_descriptor(ErrorCode code,
                                         std::string_view message) {
  return fail<std::uint64_t>(code, std::string{message});
}

Result<std::uint64_t> canonical_pixel_bytes(
    const VideoProfileDescriptor& descriptor, const VideoDecodeLimits& limits,
    ErrorCode invalid_code) {
  if (limits.maximum_width == 0 || limits.maximum_height == 0 ||
      limits.maximum_pixels == 0 || limits.maximum_payload_bytes == 0) {
    return invalid_descriptor(ErrorCode::invalid_argument,
                              "video decode limits must be non-zero");
  }
  if (descriptor.coded_width == 0 || descriptor.coded_height == 0) {
    return invalid_descriptor(invalid_code,
                              "video dimensions must be non-zero");
  }
  if (descriptor.sample_aspect_ratio_denominator == 0 ||
      descriptor.nominal_frame_rate_denominator == 0) {
    return invalid_descriptor(invalid_code,
                              "video rational denominators must be non-zero");
  }
  if (!valid_pixel_layout(descriptor.pixel_layout) ||
      !valid_color_range(descriptor.color_range) ||
      !valid_color_primaries(descriptor.color_primaries) ||
      !valid_transfer(descriptor.transfer) ||
      !valid_matrix(descriptor.matrix)) {
    return invalid_descriptor(invalid_code,
                              "video descriptor contains an unknown enum value");
  }
  if (descriptor.pixel_layout == PixelLayout::yuv420p8 &&
      ((descriptor.coded_width & 1U) != 0 ||
       (descriptor.coded_height & 1U) != 0)) {
    return invalid_descriptor(invalid_code,
                              "YUV420P8 dimensions must both be even");
  }
  if (descriptor.coded_width > limits.maximum_width ||
      descriptor.coded_height > limits.maximum_height) {
    return invalid_descriptor(ErrorCode::resource_exhausted,
                              "video dimensions exceed configured limits");
  }

  const auto pixels = static_cast<std::uint64_t>(descriptor.coded_width) *
                      descriptor.coded_height;
  if (pixels > limits.maximum_pixels) {
    return invalid_descriptor(ErrorCode::resource_exhausted,
                              "video pixel count exceeds configured limits");
  }

  std::uint64_t bytes = pixels;
  switch (descriptor.pixel_layout) {
    case PixelLayout::gray8:
      break;
    case PixelLayout::rgb24:
      if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        return invalid_descriptor(ErrorCode::resource_exhausted,
                                  "RGB24 frame size overflows");
      }
      bytes = pixels * 3U;
      break;
    case PixelLayout::rgba32:
      if (pixels > std::numeric_limits<std::uint64_t>::max() / 4U) {
        return invalid_descriptor(ErrorCode::resource_exhausted,
                                  "RGBA32 frame size overflows");
      }
      bytes = pixels * 4U;
      break;
    case PixelLayout::yuv420p8:
      if (pixels > std::numeric_limits<std::uint64_t>::max() - pixels / 2U) {
        return invalid_descriptor(ErrorCode::resource_exhausted,
                                  "YUV420P8 frame size overflows");
      }
      bytes = pixels + pixels / 2U;
      break;
  }
  if (bytes > limits.maximum_payload_bytes) {
    return invalid_descriptor(ErrorCode::resource_exhausted,
                              "video pixels exceed the configured byte limit");
  }
  return bytes;
}

void put_u32_be(std::vector<std::byte>& output, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void put_u64_be(std::vector<std::byte>& output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

std::uint32_t get_u32_be(std::span<const std::byte> input,
                         std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint32_t>(input[offset + index]);
  }
  return value;
}

std::uint64_t get_u64_be(std::span<const std::byte> input,
                         std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint64_t>(input[offset + index]);
  }
  return value;
}

bool has_magic(std::span<const std::byte> input, std::string_view magic) {
  if (input.size() < magic.size()) return false;
  for (std::size_t index = 0; index < magic.size(); ++index) {
    if (input[index] != static_cast<std::byte>(magic[index])) return false;
  }
  return true;
}

}  // namespace

Result<std::vector<std::byte>> encode_video_profile_descriptor(
    const VideoProfileDescriptor& descriptor) {
  auto pixel_bytes = canonical_pixel_bytes(
      descriptor, VideoDecodeLimits{}, ErrorCode::invalid_argument);
  if (!pixel_bytes) return pixel_bytes.error();

  std::vector<std::byte> output;
  output.reserve(kVpd1Size);
  output.insert(output.end(), {std::byte{'V'}, std::byte{'P'}, std::byte{'D'},
                               std::byte{'1'}, std::byte{0x01},
                               static_cast<std::byte>(descriptor.pixel_layout),
                               static_cast<std::byte>(descriptor.color_range),
                               static_cast<std::byte>(descriptor.color_primaries),
                               static_cast<std::byte>(descriptor.transfer),
                               static_cast<std::byte>(descriptor.matrix),
                               std::byte{0x00}, std::byte{0x00}});
  put_u32_be(output, descriptor.coded_width);
  put_u32_be(output, descriptor.coded_height);
  put_u32_be(output, descriptor.sample_aspect_ratio_numerator);
  put_u32_be(output, descriptor.sample_aspect_ratio_denominator);
  put_u32_be(output, descriptor.nominal_frame_rate_numerator);
  put_u32_be(output, descriptor.nominal_frame_rate_denominator);
  return output;
}

Result<VideoProfileDescriptor> decode_video_profile_descriptor(
    std::span<const std::byte> payload, VideoDecodeLimits limits) {
  if (payload.size() != kVpd1Size) {
    return fail<VideoProfileDescriptor>(
        ErrorCode::decode, "VPD1 payload must be exactly 36 bytes");
  }
  if (!has_magic(payload, "VPD1")) {
    return fail<VideoProfileDescriptor>(ErrorCode::decode,
                                        "VPD1 payload has invalid magic");
  }
  if (payload[4] != std::byte{0x01}) {
    return fail<VideoProfileDescriptor>(
        ErrorCode::decode, "VPD1 payload has an unsupported version");
  }
  if (payload[10] != std::byte{0x00} || payload[11] != std::byte{0x00}) {
    return fail<VideoProfileDescriptor>(
        ErrorCode::decode, "VPD1 payload has non-zero reserved bytes");
  }
  if (limits.maximum_payload_bytes < kVpd1Size) {
    return fail<VideoProfileDescriptor>(
        ErrorCode::resource_exhausted,
        "VPD1 payload exceeds the configured byte limit");
  }

  VideoProfileDescriptor descriptor{
      .coded_width = get_u32_be(payload, 12),
      .coded_height = get_u32_be(payload, 16),
      .pixel_layout = static_cast<PixelLayout>(
          std::to_integer<std::uint8_t>(payload[5])),
      .sample_aspect_ratio_numerator = get_u32_be(payload, 20),
      .sample_aspect_ratio_denominator = get_u32_be(payload, 24),
      .nominal_frame_rate_numerator = get_u32_be(payload, 28),
      .nominal_frame_rate_denominator = get_u32_be(payload, 32),
      .color_range = static_cast<ColorRange>(
          std::to_integer<std::uint8_t>(payload[6])),
      .color_primaries = static_cast<ColorPrimaries>(
          std::to_integer<std::uint8_t>(payload[7])),
      .transfer = static_cast<TransferCharacteristics>(
          std::to_integer<std::uint8_t>(payload[8])),
      .matrix = static_cast<MatrixCoefficients>(
          std::to_integer<std::uint8_t>(payload[9])),
  };
  auto valid = canonical_pixel_bytes(descriptor, limits, ErrorCode::decode);
  if (!valid) return valid.error();
  return descriptor;
}

Result<std::vector<std::byte>> encode_raw_video_frame_state(
    const RawVideoFrameState& frame) {
  auto expected = canonical_pixel_bytes(
      frame.descriptor, VideoDecodeLimits{}, ErrorCode::invalid_argument);
  if (!expected) return expected.error();
  if (*expected != frame.pixels.size()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "raw video pixels do not match the declared canonical layout");
  }

  auto descriptor = encode_video_profile_descriptor(frame.descriptor);
  if (!descriptor) return descriptor.error();
  const auto total = static_cast<std::uint64_t>(kVfr1HeaderSize) +
                     descriptor->size() + frame.pixels.size();
  if (total > std::numeric_limits<std::size_t>::max() ||
      total > VideoDecodeLimits{}.maximum_payload_bytes) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "VFR1 payload exceeds the configured byte limit");
  }

  std::vector<std::byte> output;
  output.reserve(static_cast<std::size_t>(total));
  output.insert(output.end(), {std::byte{'V'}, std::byte{'F'}, std::byte{'R'},
                               std::byte{'1'}, std::byte{0x01},
                               std::byte{0x00}, std::byte{0x00},
                               std::byte{0x00}});
  put_u32_be(output, static_cast<std::uint32_t>(descriptor->size()));
  put_u64_be(output, static_cast<std::uint64_t>(frame.pixels.size()));
  output.insert(output.end(), descriptor->begin(), descriptor->end());
  output.insert(output.end(), frame.pixels.begin(), frame.pixels.end());
  return output;
}

Result<RawVideoFrameState> decode_raw_video_frame_state(
    std::span<const std::byte> payload, VideoDecodeLimits limits) {
  if (payload.size() < kVfr1HeaderSize) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode, "VFR1 payload is shorter than its header");
  }
  if (!has_magic(payload, "VFR1")) {
    return fail<RawVideoFrameState>(ErrorCode::decode,
                                    "VFR1 payload has invalid magic");
  }
  if (payload[4] != std::byte{0x01}) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode, "VFR1 payload has an unsupported version");
  }
  if (payload[5] != std::byte{0x00} || payload[6] != std::byte{0x00} ||
      payload[7] != std::byte{0x00}) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode, "VFR1 payload has non-zero reserved bytes");
  }

  const auto descriptor_bytes = get_u32_be(payload, 8);
  const auto pixel_bytes = get_u64_be(payload, 12);
  if (descriptor_bytes != kVpd1Size) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode, "VFR1 descriptor length is not canonical");
  }
  if (pixel_bytes > std::numeric_limits<std::size_t>::max()) {
    return fail<RawVideoFrameState>(ErrorCode::resource_exhausted,
                                    "VFR1 pixels exceed this platform");
  }
  if (payload.size() > limits.maximum_payload_bytes) {
    return fail<RawVideoFrameState>(
        ErrorCode::resource_exhausted,
        "VFR1 payload exceeds the configured byte limit");
  }

  const auto total = static_cast<std::uint64_t>(kVfr1HeaderSize) +
                     descriptor_bytes + pixel_bytes;
  if (total != payload.size()) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode, "VFR1 lengths do not match the exact payload size");
  }

  auto descriptor = decode_video_profile_descriptor(
      payload.subspan(kVfr1HeaderSize, descriptor_bytes), limits);
  if (!descriptor) return descriptor.error();
  auto expected = canonical_pixel_bytes(*descriptor, limits, ErrorCode::decode);
  if (!expected) return expected.error();
  if (*expected != pixel_bytes) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode,
        "VFR1 pixel length does not match the canonical layout");
  }

  const auto pixels = payload.subspan(kVfr1HeaderSize + descriptor_bytes,
                                      static_cast<std::size_t>(pixel_bytes));
  return RawVideoFrameState{
      .descriptor = std::move(*descriptor),
      .pixels = std::vector<std::byte>{pixels.begin(), pixels.end()},
  };
}

}  // namespace codec::profiles::video
