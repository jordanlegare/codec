#pragma once

#include <codec/profiles/video.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace codec::profiles::video::detail {

struct FfmpegCapturedEncodedPacket {
  std::int64_t pts_ns{};
  std::int64_t dts_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  bool has_skip_samples{};
  std::vector<std::byte> payload;
};

struct FfmpegEncodedAudioCaptureBoundary {
  bool present{};
  EncodedAudioCodec codec{EncodedAudioCodec::aac};
  std::int32_t codec_profile{};
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::uint64_t decoded_frames{};
  std::optional<std::int64_t> first_audio_ns{};
  std::optional<std::int64_t> video_origin_ns{};
  std::vector<std::byte> decoder_config;
  std::vector<FfmpegCapturedEncodedPacket> packets;
  std::optional<Error> error{};
};

struct FfmpegCapturedEncodedAudio {
  bool present{};
  std::optional<EncodedAudioState> state{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::optional<Error> error{};
};

Result<FfmpegCapturedEncodedAudio> finalize_ffmpeg_encoded_audio_capture(
    const FfmpegVideoIngestRequest& request,
    FfmpegEncodedAudioCaptureBoundary boundary);
Result<std::uint64_t> ffmpeg_audio_timeline_difference(
    std::int64_t first_audio_ns, std::uint64_t decoded_frames,
    std::uint32_t sample_rate, std::int64_t chunk_start_ns);

Result<void> begin_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request, bool enabled);
FfmpegCapturedEncodedAudio take_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request);

}  // namespace codec::profiles::video::detail
