#include "ffmpeg_audio_capture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace codec::profiles::video::detail {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

Result<std::uint64_t> frames_for_ns(std::uint64_t nanoseconds,
                                    std::uint32_t sample_rate, bool round_up) {
  const auto seconds = nanoseconds / kNanosecondsPerSecond;
  const auto remainder = nanoseconds % kNanosecondsPerSecond;
  if (seconds > std::numeric_limits<std::uint64_t>::max() / sample_rate) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "audio time conversion exceeds bounds");
  }
  auto frames = seconds * sample_rate;
  const auto fractional = remainder * sample_rate;
  auto partial = fractional / kNanosecondsPerSecond;
  if (round_up && fractional % kNanosecondsPerSecond != 0U) ++partial;
  if (frames > std::numeric_limits<std::uint64_t>::max() - partial) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "audio frame count exceeds bounds");
  }
  return frames + partial;
}

Result<std::uint64_t> nanoseconds_for_frames(std::uint64_t frames,
                                             std::uint32_t sample_rate) {
  const auto seconds = frames / sample_rate;
  const auto remainder = frames % sample_rate;
  if (seconds > std::numeric_limits<std::uint64_t>::max() /
                    kNanosecondsPerSecond) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "audio duration exceeds bounds");
  }
  return seconds * kNanosecondsPerSecond +
         (remainder * kNanosecondsPerSecond) / sample_rate;
}

FfmpegCapturedEncodedAudio profile_error(bool present, ErrorCode code,
                                         std::string message) {
  return FfmpegCapturedEncodedAudio{
      .present = present,
      .state = std::nullopt,
      .start_ns = 0,
      .end_ns = 0,
      .error = Error{code, std::move(message), false},
  };
}

}  // namespace

Result<FfmpegCapturedEncodedAudio> finalize_ffmpeg_encoded_audio_capture(
    const FfmpegVideoIngestRequest& request,
    const FfmpegEncodedAudioCaptureBoundary& boundary) {
  if (request.maximum_decoded_audio_bytes == 0U ||
      request.maximum_decoded_audio_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail<FfmpegCapturedEncodedAudio>(
        ErrorCode::invalid_argument,
        "FFmpeg video ingest audio limit is outside process bounds");
  }
  if (!boundary.present) return FfmpegCapturedEncodedAudio{};
  if (boundary.error.has_value()) {
    return FfmpegCapturedEncodedAudio{
        .present = true,
        .state = std::nullopt,
        .start_ns = 0,
        .end_ns = 0,
        .error = boundary.error,
    };
  }
  if (boundary.codec != EncodedAudioCodec::aac ||
      boundary.sample_rate == 0U || boundary.channels == 0U ||
      boundary.channels > 2U || boundary.decoded_frames == 0U ||
      !boundary.first_audio_ns.has_value() ||
      !boundary.video_origin_ns.has_value() || boundary.packets.empty()) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio cannot be synchronized to video");
  }

  const auto maximum_bytes = request.maximum_decoded_audio_bytes;
  std::uint64_t captured_bytes = boundary.decoder_config.size();
  for (const auto& packet : boundary.packets) {
    if (packet.payload.empty() || packet.duration_ns == 0U ||
        captured_bytes > maximum_bytes ||
        packet.payload.size() > maximum_bytes - captured_bytes) {
      return profile_error(true, ErrorCode::resource_exhausted,
                           "encoded audio exceeds the configured aggregate byte limit");
    }
    captured_bytes += packet.payload.size();
  }

  if (*boundary.first_audio_ns < 0 &&
      *boundary.video_origin_ns >
          std::numeric_limits<std::int64_t>::max() +
              *boundary.first_audio_ns) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio synchronization delta overflows");
  }
  auto relative_start_ns =
      *boundary.first_audio_ns - *boundary.video_origin_ns;
  std::uint64_t trim_start_frames = 0U;
  if (relative_start_ns < 0) {
    if (relative_start_ns == std::numeric_limits<std::int64_t>::min()) {
      return profile_error(true, ErrorCode::decode,
                           "encoded audio starts outside synchronization bounds");
    }
    auto trim = frames_for_ns(static_cast<std::uint64_t>(-relative_start_ns),
                              boundary.sample_rate, true);
    if (!trim) return trim.error();
    trim_start_frames = *trim;
    relative_start_ns = 0;
  }
  if (trim_start_frames >= boundary.decoded_frames ||
      relative_start_ns >
          std::numeric_limits<std::int64_t>::max() - request.start_ns) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio has no presentation within the video interval");
  }
  const auto start_ns = request.start_ns + relative_start_ns;
  if (start_ns < request.start_ns || start_ns >= request.end_ns) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio begins outside the requested interval");
  }
  auto maximum_frames = frames_for_ns(
      static_cast<std::uint64_t>(request.end_ns - start_ns),
      boundary.sample_rate, false);
  if (!maximum_frames) return maximum_frames.error();
  const auto remaining_frames = boundary.decoded_frames - trim_start_frames;
  const auto presentation_frames =
      std::min(remaining_frames, *maximum_frames);
  if (presentation_frames == 0U) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio presentation contains no complete frames");
  }
  auto duration_ns =
      nanoseconds_for_frames(presentation_frames, boundary.sample_rate);
  if (!duration_ns || *duration_ns == 0U ||
      *duration_ns >
          static_cast<std::uint64_t>(request.end_ns - start_ns)) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio duration is outside the requested interval");
  }
  if (*duration_ns > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max() - start_ns)) {
    return profile_error(true, ErrorCode::resource_exhausted,
                         "encoded audio archive timestamp overflows");
  }
  const auto end_ns = start_ns + static_cast<std::int64_t>(*duration_ns);

  if (*boundary.video_origin_ns >
      std::numeric_limits<std::int64_t>::max() - relative_start_ns) {
    return profile_error(true, ErrorCode::resource_exhausted,
                         "encoded audio source timestamp overflows");
  }
  const auto source_window_start =
      *boundary.video_origin_ns + relative_start_ns;
  if (*duration_ns > static_cast<std::uint64_t>(
                         std::numeric_limits<std::int64_t>::max() -
                         source_window_start)) {
    return profile_error(true, ErrorCode::resource_exhausted,
                         "encoded audio source interval overflows");
  }
  const auto source_window_end =
      source_window_start + static_cast<std::int64_t>(*duration_ns);

  std::vector<EncodedAudioPacket> packets;
  packets.reserve(boundary.packets.size());
  for (const auto& packet : boundary.packets) {
    if (packet.duration_ns > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max() -
                                 packet.pts_ns)) {
      return profile_error(true, ErrorCode::decode,
                           "encoded audio packet interval overflows");
    }
    const auto packet_end =
        packet.pts_ns + static_cast<std::int64_t>(packet.duration_ns);
    if (packet.pts_ns >= source_window_end || packet_end <= source_window_start) {
      continue;
    }
    packets.push_back(EncodedAudioPacket{
        .pts_offset_ns = packet.pts_ns - source_window_start,
        .dts_offset_ns = packet.dts_ns - source_window_start,
        .duration_ns = packet.duration_ns,
        .flags = packet.flags,
        .payload = packet.payload,
    });
  }
  if (packets.empty()) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio has no packets in its presentation window");
  }

  EncodedAudioState state{
      .codec = boundary.codec,
      .codec_profile = boundary.codec_profile,
      .sample_rate = boundary.sample_rate,
      .channels = boundary.channels,
      .decoded_frames = boundary.decoded_frames,
      .trim_start_frames = trim_start_frames,
      .presentation_frames = presentation_frames,
      .decoder_config = boundary.decoder_config,
      .packets = std::move(packets),
  };
  auto encoded = encode_encoded_audio_state(state);
  if (!encoded) {
    return profile_error(true, encoded.error().code, encoded.error().message);
  }
  if (encoded->size() > maximum_bytes) {
    return profile_error(true, ErrorCode::resource_exhausted,
                         "encoded audio state exceeds the configured aggregate byte limit");
  }
  return FfmpegCapturedEncodedAudio{
      .present = true,
      .state = std::move(state),
      .start_ns = start_ns,
      .end_ns = end_ns,
      .error = std::nullopt,
  };
}

}  // namespace codec::profiles::video::detail
