#pragma once

#include <codec/audio.hpp>

#include <cstddef>
#include <span>

namespace codec::detail {

Result<WavPcm16> decode_wav_pcm16(std::span<const std::byte> encoded);

}  // namespace codec::detail
