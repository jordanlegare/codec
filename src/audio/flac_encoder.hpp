#pragma once

#include <codec/audio.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace codec::profiles::audio::detail {

Result<std::vector<std::byte>> encode_flac_pcm16(
    const Pcm16State& state, std::uint64_t maximum_output_bytes);

}  // namespace codec::profiles::audio::detail
