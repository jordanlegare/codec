#pragma once

#include <codec/profiles/video.hpp>

#include <cstdint>
#include <optional>

namespace codec::profiles::video::detail {

struct FfmpegCapturedPcm16Audio {
  bool present{};
  std::optional<Pcm16State> state{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::optional<Error> error{};
};

Result<void> begin_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request, bool enabled);
FfmpegCapturedPcm16Audio take_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request);

}  // namespace codec::profiles::video::detail
