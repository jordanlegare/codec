#pragma once

#include <codec/audio.hpp>

#include <cstdint>
#include <span>

namespace codec::profiles::audio::detail {

Result<Pcm16State> decode_flac_pcm16(
    std::span<const std::byte> source,
    std::uint64_t maximum_decoded_pcm_bytes);

}  // namespace codec::profiles::audio::detail
