#include <codec/profiles/video.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace codec::profiles::video {
namespace {

constexpr std::size_t kHeaderSize = 80U;
constexpr std::size_t kPacketHeaderSize = 32U;
constexpr std::uint32_t kSupportedPacketFlags = 0x1fU;
constexpr std::uint64_t kMaximumPixels = 268435456ULL;
constexpr std::uint32_t kMaximumDimension = 16384U;

void put_u16(std::vector<std::byte>& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::byte>(value & 0xffU));
}

void put_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void put_u64(std::vector<std::byte>& output, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

std::uint16_t get_u16(std::span<const std::byte> input, std::size_t offset) {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(input[offset]) << 8U) |
      std::to_integer<std::uint16_t>(input[offset + 1U]));
}

std::uint32_t get_u32(std::span<const std::byte> input, std::size_t offset) {
  std::uint32_t value = 0U;
  for (std::size_t index = 0; index < 4U; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint32_t>(input[offset + index]);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::byte> input, std::size_t offset) {
  std::uint64_t value = 0U;
  for (std::size_t index = 0; index < 8U; ++index) {
    value = (value << 8U) |
            std::to_integer<std::uint64_t>(input[offset + index]);
  }
  return value;
}

bool has_magic(std::span<const std::byte> input) {
  return input.size() >= 4U && input[0] == std::byte{'E'} &&
         input[1] == std::byte{'V'} && input[2] == std::byte{'P'} &&
         input[3] == std::byte{'1'};
}

bool valid_framing(EncodedVideoPacketFraming framing) {
  switch (framing) {
    case EncodedVideoPacketFraming::unknown:
    case EncodedVideoPacketFraming::length_prefixed:
    case EncodedVideoPacketFraming::annex_b: return true;
  }
  return false;
}

Result<void> validate_state(const EncodedVideoState& state, ErrorCode code) {
  if (state.codec != EncodedVideoCodec::h264 || !valid_framing(state.framing) ||
      state.coded_width == 0U || state.coded_height == 0U ||
      state.coded_width > kMaximumDimension ||
      state.coded_height > kMaximumDimension ||
      state.sample_aspect_ratio_denominator == 0U ||
      state.validated_frames == 0U || state.packets.empty() ||
      state.presentation_lead_ns >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return fail(code, "EVP1 state has invalid video geometry");
  }
  const auto pixels = static_cast<std::uint64_t>(state.coded_width) *
                      state.coded_height;
  if (pixels > kMaximumPixels) {
    return fail(code, "EVP1 dimensions exceed video profile bounds");
  }

  bool first = true;
  std::int64_t previous_dts = 0;
  for (const auto& packet : state.packets) {
    if (packet.payload.empty() ||
        (packet.flags & ~kSupportedPacketFlags) != 0U ||
        packet.duration_ns == 0U ||
        packet.duration_ns > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max()) ||
        packet.dts_offset_ns < 0) {
      return fail(code, "EVP1 packet is invalid");
    }
    const auto duration = static_cast<std::int64_t>(packet.duration_ns);
    if (packet.pts_offset_ns >
            std::numeric_limits<std::int64_t>::max() - duration ||
        packet.dts_offset_ns >
            std::numeric_limits<std::int64_t>::max() - duration) {
      return fail(code, "EVP1 packet timing exceeds bounds");
    }
    if ((first && packet.dts_offset_ns != 0) ||
        (!first && packet.dts_offset_ns < previous_dts)) {
      return fail(code, "EVP1 packet DTS is not canonical");
    }
    previous_dts = packet.dts_offset_ns;
    first = false;
  }
  return {};
}

}  // namespace

Result<std::vector<std::byte>> encode_encoded_video_state(
    const EncodedVideoState& state) {
  auto valid = validate_state(state, ErrorCode::invalid_argument);
  if (!valid) return valid.error();
  if (state.packets.size() > std::numeric_limits<std::uint32_t>::max() ||
      state.decoder_config.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                       "EVP1 state exceeds format bounds");
  }

  std::uint64_t packet_bytes = 0U;
  std::uint64_t encoded_size = kHeaderSize + state.decoder_config.size();
  for (const auto& packet : state.packets) {
    if (packet.payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        packet_bytes > std::numeric_limits<std::uint64_t>::max() -
                           packet.payload.size() ||
        encoded_size > std::numeric_limits<std::uint64_t>::max() -
                           kPacketHeaderSize - packet.payload.size()) {
      return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                         "EVP1 packet size exceeds bounds");
    }
    packet_bytes += packet.payload.size();
    encoded_size += kPacketHeaderSize + packet.payload.size();
  }
  if (encoded_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                       "EVP1 state exceeds process bounds");
  }

  std::vector<std::byte> output;
  output.reserve(static_cast<std::size_t>(encoded_size));
  output.insert(output.end(), {std::byte{'E'}, std::byte{'V'}, std::byte{'P'},
                               std::byte{'1'}});
  put_u16(output, 1U);
  put_u16(output, static_cast<std::uint16_t>(state.codec));
  output.push_back(static_cast<std::byte>(state.framing));
  output.push_back(std::byte{0x00});
  put_u16(output, 0U);
  put_u32(output, std::bit_cast<std::uint32_t>(state.codec_profile));
  put_u32(output, std::bit_cast<std::uint32_t>(state.codec_level));
  put_u32(output, state.coded_width);
  put_u32(output, state.coded_height);
  put_u32(output, state.sample_aspect_ratio_numerator);
  put_u32(output, state.sample_aspect_ratio_denominator);
  put_u32(output, static_cast<std::uint32_t>(state.packets.size()));
  put_u32(output, static_cast<std::uint32_t>(state.decoder_config.size()));
  put_u32(output, 0U);
  put_u64(output, state.validated_frames);
  put_u64(output, state.presentation_lead_ns);
  put_u64(output, packet_bytes);
  put_u64(output, 0U);
  output.insert(output.end(), state.decoder_config.begin(),
                state.decoder_config.end());
  for (const auto& packet : state.packets) {
    put_u64(output, std::bit_cast<std::uint64_t>(packet.pts_offset_ns));
    put_u64(output, std::bit_cast<std::uint64_t>(packet.dts_offset_ns));
    put_u64(output, packet.duration_ns);
    put_u32(output, packet.flags);
    put_u32(output, static_cast<std::uint32_t>(packet.payload.size()));
    output.insert(output.end(), packet.payload.begin(), packet.payload.end());
  }
  return output;
}

Result<EncodedVideoState> decode_encoded_video_state(
    std::span<const std::byte> payload, EncodedVideoDecodeLimits limits) {
  if (limits.maximum_packets == 0U ||
      limits.maximum_decoder_config_bytes == 0U ||
      limits.maximum_packet_bytes == 0U || limits.maximum_payload_bytes == 0U) {
    return fail<EncodedVideoState>(ErrorCode::invalid_argument,
                                   "EVP1 decode limits must be non-zero");
  }
  if (payload.size() < kHeaderSize || !has_magic(payload) ||
      get_u16(payload, 4U) != 1U || payload[9] != std::byte{0x00} ||
      get_u16(payload, 10U) != 0U || get_u32(payload, 44U) != 0U ||
      get_u64(payload, 72U) != 0U) {
    return fail<EncodedVideoState>(ErrorCode::decode,
                                   "EVP1 payload has an invalid header");
  }

  const auto packet_count = get_u32(payload, 36U);
  const auto config_size = get_u32(payload, 40U);
  const auto declared_packet_bytes = get_u64(payload, 64U);
  if (packet_count > limits.maximum_packets ||
      config_size > limits.maximum_decoder_config_bytes ||
      declared_packet_bytes > limits.maximum_payload_bytes) {
    return fail<EncodedVideoState>(ErrorCode::resource_exhausted,
                                   "EVP1 payload exceeds configured limits");
  }
  if (config_size > payload.size() - kHeaderSize) {
    return fail<EncodedVideoState>(ErrorCode::decode,
                                   "EVP1 decoder configuration is truncated");
  }
  const auto packet_region_bytes =
      payload.size() - kHeaderSize - static_cast<std::size_t>(config_size);
  if (packet_count > packet_region_bytes / (kPacketHeaderSize + 1U)) {
    return fail<EncodedVideoState>(
        ErrorCode::decode,
        "EVP1 packet count cannot fit in the encoded payload");
  }

  EncodedVideoState state{
      .codec = static_cast<EncodedVideoCodec>(get_u16(payload, 6U)),
      .framing = static_cast<EncodedVideoPacketFraming>(
          std::to_integer<std::uint8_t>(payload[8U])),
      .codec_profile = std::bit_cast<std::int32_t>(get_u32(payload, 12U)),
      .codec_level = std::bit_cast<std::int32_t>(get_u32(payload, 16U)),
      .coded_width = get_u32(payload, 20U),
      .coded_height = get_u32(payload, 24U),
      .sample_aspect_ratio_numerator = get_u32(payload, 28U),
      .sample_aspect_ratio_denominator = get_u32(payload, 32U),
      .validated_frames = get_u64(payload, 48U),
      .presentation_lead_ns = get_u64(payload, 56U),
      .decoder_config = {},
      .packets = {},
  };
  std::size_t offset = kHeaderSize;
  state.decoder_config.assign(
      payload.begin() + static_cast<std::ptrdiff_t>(offset),
      payload.begin() +
          static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(config_size)));
  offset += config_size;
  state.packets.reserve(packet_count);
  std::uint64_t actual_packet_bytes = 0U;
  for (std::uint32_t index = 0U; index < packet_count; ++index) {
    if (payload.size() - offset < kPacketHeaderSize) {
      return fail<EncodedVideoState>(ErrorCode::decode,
                                     "EVP1 packet header is truncated");
    }
    const auto packet_size = get_u32(payload, offset + 28U);
    if (packet_size > limits.maximum_packet_bytes) {
      return fail<EncodedVideoState>(
          ErrorCode::resource_exhausted,
          "EVP1 packet exceeds the configured individual limit");
    }
    if (packet_size == 0U ||
        packet_size > payload.size() - offset - kPacketHeaderSize ||
        actual_packet_bytes > std::numeric_limits<std::uint64_t>::max() -
                                  packet_size) {
      return fail<EncodedVideoState>(ErrorCode::decode,
                                     "EVP1 packet payload is invalid");
    }
    if (actual_packet_bytes > limits.maximum_payload_bytes ||
        packet_size > limits.maximum_payload_bytes - actual_packet_bytes) {
      return fail<EncodedVideoState>(
          ErrorCode::resource_exhausted,
          "EVP1 packet payloads exceed configured limits");
    }
    if (actual_packet_bytes > declared_packet_bytes ||
        packet_size > declared_packet_bytes - actual_packet_bytes) {
      return fail<EncodedVideoState>(
          ErrorCode::decode,
          "EVP1 packet payloads exceed the declared aggregate size");
    }
    EncodedVideoPacket packet{
        .pts_offset_ns =
            std::bit_cast<std::int64_t>(get_u64(payload, offset)),
        .dts_offset_ns =
            std::bit_cast<std::int64_t>(get_u64(payload, offset + 8U)),
        .duration_ns = get_u64(payload, offset + 16U),
        .flags = get_u32(payload, offset + 24U),
        .payload = {},
    };
    offset += kPacketHeaderSize;
    packet.payload.assign(
        payload.begin() + static_cast<std::ptrdiff_t>(offset),
        payload.begin() + static_cast<std::ptrdiff_t>(
                              offset + static_cast<std::size_t>(packet_size)));
    offset += packet_size;
    actual_packet_bytes += packet_size;
    state.packets.push_back(std::move(packet));
  }
  if (offset != payload.size() || actual_packet_bytes != declared_packet_bytes) {
    return fail<EncodedVideoState>(ErrorCode::decode,
                                   "EVP1 payload size does not match its header");
  }
  auto valid = validate_state(state, ErrorCode::decode);
  if (!valid) return valid.error();
  return state;
}

}  // namespace codec::profiles::video
