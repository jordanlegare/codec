#pragma once

#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec::profiles::video {

Result<std::vector<std::byte>> mux_verified_encoded_audio_trim_aware(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes);

}  // namespace codec::profiles::video
