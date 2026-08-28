#include <codec/audio.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace codec {
namespace {

constexpr std::size_t kHeaderSize = 24;

Result<void> validate_pcm16(std::uint32_t sample_rate,
                            std::uint16_t channels,
                            std::size_t sample_count) {
  if (sample_rate == 0) {
    return fail(ErrorCode::invalid_argument,
                "PCM16 sample rate must be non-zero");
  }
  if (channels == 0) {
    return fail(ErrorCode::invalid_argument,
                "PCM16 channel count must be non-zero");
  }
  if (sample_count % channels != 0) {
    return fail(ErrorCode::invalid_argument,
                "PCM16 samples must contain complete interleaved frames");
  }
  return {};
}

void put_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value & 0xffU));
  bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void put_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint16_t>(bytes[offset]) |
      (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= std::to_integer<std::uint32_t>(bytes[offset + index])
             << (index * 8U);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= std::to_integer<std::uint64_t>(bytes[offset + index])
             << (index * 8U);
  }
  return value;
}

}  // namespace

Result<Pcm16State> canonicalize_pcm16(const WavPcm16& wav) {
  auto valid = validate_pcm16(wav.sample_rate, wav.channels,
                              wav.samples.size());
  if (!valid) return valid.error();

  return Pcm16State{
      .sample_rate = wav.sample_rate,
      .channels = wav.channels,
      .samples = wav.samples,
  };
}

Result<std::vector<std::byte>> encode_pcm16_state(const Pcm16State& state) {
  auto valid = validate_pcm16(state.sample_rate, state.channels,
                              state.samples.size());
  if (!valid) return valid.error();
  if (state.samples.size() >
      (std::numeric_limits<std::size_t>::max() - kHeaderSize) / 2) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                       "PCM16 state is too large to encode");
  }

  std::vector<std::byte> encoded;
  encoded.reserve(kHeaderSize + state.samples.size() * 2);
  encoded.insert(encoded.end(), {std::byte{'A'}, std::byte{'P'},
                                 std::byte{'S'}, std::byte{'1'}});
  put_u16(encoded, 1);  // APS1 version.
  put_u16(encoded, 1);  // Signed little-endian PCM16.
  put_u32(encoded, state.sample_rate);
  put_u16(encoded, state.channels);
  put_u16(encoded, 0);  // Reserved; canonical encodings keep this zero.
  put_u64(encoded, static_cast<std::uint64_t>(state.samples.size()));
  for (const auto sample : state.samples) {
    put_u16(encoded, std::bit_cast<std::uint16_t>(sample));
  }
  return encoded;
}

Result<Pcm16State> decode_pcm16_state(std::span<const std::byte> encoded) {
  if (encoded.size() < kHeaderSize) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload is shorter than its header");
  }
  constexpr std::byte kMagic[] = {
      std::byte{'A'}, std::byte{'P'}, std::byte{'S'}, std::byte{'1'}};
  for (std::size_t index = 0; index < 4; ++index) {
    if (encoded[index] != kMagic[index]) {
      return fail<Pcm16State>(ErrorCode::decode,
                              "APS1 payload has invalid magic");
    }
  }
  if (get_u16(encoded, 4) != 1) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload has an unsupported version");
  }
  if (get_u16(encoded, 6) != 1) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload is not signed little-endian PCM16");
  }
  if (get_u16(encoded, 14) != 0) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload has non-zero reserved bits");
  }

  const auto sample_rate = get_u32(encoded, 8);
  const auto channels = get_u16(encoded, 12);
  const auto encoded_sample_count = get_u64(encoded, 16);
  if (sample_rate == 0 || channels == 0) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload has invalid PCM16 geometry");
  }
  if (encoded_sample_count > std::numeric_limits<std::size_t>::max()) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 sample count exceeds this platform");
  }
  const auto sample_count = static_cast<std::size_t>(encoded_sample_count);
  if (sample_count >
      (std::numeric_limits<std::size_t>::max() - kHeaderSize) / 2 ||
      kHeaderSize + sample_count * 2 != encoded.size()) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 sample count does not match payload size");
  }
  if (sample_count % channels != 0) {
    return fail<Pcm16State>(ErrorCode::decode,
                            "APS1 payload contains an incomplete frame");
  }

  std::vector<std::int16_t> samples;
  samples.reserve(sample_count);
  for (std::size_t index = 0; index < sample_count; ++index) {
    samples.push_back(std::bit_cast<std::int16_t>(
        get_u16(encoded, kHeaderSize + index * 2)));
  }
  return Pcm16State{
      .sample_rate = sample_rate,
      .channels = channels,
      .samples = std::move(samples),
  };
}

}  // namespace codec
