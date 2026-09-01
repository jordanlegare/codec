#pragma once

#include <codec/profiles/video.hpp>
#include <codec/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace codec::profiles::video {

Result<std::vector<std::byte>> mux_verified_encoded_video_packets(
    const VerifiedVideoEncodedVideo& video,
    std::uint64_t maximum_output_bytes);

Result<std::vector<std::byte>> mux_verified_encoded_audio_packets(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes);

Result<std::vector<std::byte>> mux_verified_adts_encoded_audio(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes);

Result<std::vector<std::byte>> mux_verified_pcm16_audio(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoPcm16Audio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes);

}  // namespace codec::profiles::video
