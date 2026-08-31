#pragma once

#include <codec/profiles/video.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace codec::profiles::video::detail {

struct FfmpegCapturedPcm16Audio {
  bool present{};
  std::optional<Pcm16State> state{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::optional<Error> error{};
};

struct FfmpegCapturedEncodedPacket {
  std::int64_t pts_ns{};
  std::int64_t dts_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
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
    const FfmpegEncodedAudioCaptureBoundary& boundary);

Result<void> begin_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request, bool enabled);
FfmpegCapturedPcm16Audio take_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request);

}  // namespace codec::profiles::video::detail
