#pragma once

#include <codec/result.hpp>

#include <cstdint>
#include <filesystem>
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

}  // namespace codec

