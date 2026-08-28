#pragma once

#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace codec {

struct WavPcm16 {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::vector<std::int16_t> samples;

  static Result<WavPcm16> read(const std::filesystem::path& path);
  Result<void> write(const std::filesystem::path& path) const;
  std::size_t frames() const noexcept {
    return channels == 0 ? 0 : samples.size() / channels;
  }
  double duration_seconds() const noexcept {
    return sample_rate == 0 ? 0.0
                            : static_cast<double>(frames()) / sample_rate;
  }
};

// Canonical, profile-specific audio state. Unlike WavPcm16, this type contains
// no container-level representation such as RIFF chunks or padding.
struct Pcm16State {
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::vector<std::int16_t> samples;

  std::size_t frames() const noexcept {
    return channels == 0 ? 0 : samples.size() / channels;
  }
  double duration_seconds() const noexcept {
    return sample_rate == 0 ? 0.0
                            : static_cast<double>(frames()) / sample_rate;
  }
};

Result<Pcm16State> canonicalize_pcm16(const WavPcm16& wav);
Result<std::vector<std::byte>> encode_pcm16_state(const Pcm16State& state);
Result<Pcm16State> decode_pcm16_state(std::span<const std::byte> encoded);

}  // namespace codec
