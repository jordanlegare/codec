#pragma once

#include <codec/audio.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec::detail {

Result<WavPcm16> decode_wav_pcm16(std::span<const std::byte> encoded);
Result<std::size_t> encoded_wav_pcm16_size(std::uint32_t sample_rate,
                                           std::uint16_t channels,
                                           std::size_t sample_count);
Result<std::vector<std::byte>> encode_wav_pcm16(
    std::uint32_t sample_rate, std::uint16_t channels,
    std::span<const std::int16_t> samples);

}  // namespace codec::detail
