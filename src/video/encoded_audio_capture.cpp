#include "ffmpeg_audio_capture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::video::detail {
namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kEncodedAudioHeaderBytes = 64U;
constexpr std::uint64_t kEncodedAudioPacketHeaderBytes = 32U;
constexpr std::size_t kMaximumEncodedAudioPackets = 1'000'000U;

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

Result<std::int64_t> add_nonnegative(std::int64_t base,
                                     std::uint64_t delta,
                                     std::string_view label) {
  if (delta > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max()) ||
      base > std::numeric_limits<std::int64_t>::max() -
                 static_cast<std::int64_t>(delta)) {
    return fail<std::int64_t>(ErrorCode::resource_exhausted,
                              std::string{label} + " overflows");
  }
  return base + static_cast<std::int64_t>(delta);
}

Result<std::int64_t> subtract(std::int64_t left, std::int64_t right,
                              std::string_view label) {
  if ((right > 0 &&
       left < std::numeric_limits<std::int64_t>::min() + right) ||
      (right < 0 &&
       left > std::numeric_limits<std::int64_t>::max() + right)) {
    return fail<std::int64_t>(ErrorCode::resource_exhausted,
                              std::string{label} + " overflows");
  }
  return left - right;
}

std::uint64_t positive_distance(std::int64_t end,
                                std::int64_t start) noexcept {
  return static_cast<std::uint64_t>(end) - static_cast<std::uint64_t>(start);
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

Result<std::uint64_t> ffmpeg_audio_timeline_difference(
    std::int64_t first_audio_ns, std::uint64_t decoded_frames,
    std::uint32_t sample_rate, std::int64_t chunk_start_ns) {
  if (sample_rate == 0U) {
    return fail<std::uint64_t>(ErrorCode::invalid_argument,
                               "audio timeline sample rate must be non-zero");
  }
  auto elapsed_ns = nanoseconds_for_frames(decoded_frames, sample_rate);
  if (!elapsed_ns) return elapsed_ns.error();
  auto expected = add_nonnegative(first_audio_ns, *elapsed_ns,
                                  "decoded audio expected timestamp");
  if (!expected) return expected.error();
  if (chunk_start_ns >= *expected) {
    return static_cast<std::uint64_t>(chunk_start_ns) -
           static_cast<std::uint64_t>(*expected);
  }
  return static_cast<std::uint64_t>(*expected) -
         static_cast<std::uint64_t>(chunk_start_ns);
}

Result<FfmpegCapturedEncodedAudio> finalize_ffmpeg_encoded_audio_capture(
    const FfmpegVideoIngestRequest& request,
    FfmpegEncodedAudioCaptureBoundary boundary) {
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
  if (boundary.packets.size() > kMaximumEncodedAudioPackets ||
      maximum_bytes < kEncodedAudioHeaderBytes ||
      boundary.decoder_config.size() >
          maximum_bytes - kEncodedAudioHeaderBytes) {
    return profile_error(true, ErrorCode::resource_exhausted,
                         "encoded audio state exceeds the configured aggregate byte limit");
  }
  std::uint64_t captured_bytes =
      kEncodedAudioHeaderBytes + boundary.decoder_config.size();
  for (const auto& packet : boundary.packets) {
    if (packet.payload.empty() || packet.duration_ns == 0U ||
        captured_bytes > maximum_bytes ||
        kEncodedAudioPacketHeaderBytes > maximum_bytes - captured_bytes ||
        packet.payload.size() > maximum_bytes - captured_bytes -
                                    kEncodedAudioPacketHeaderBytes) {
      return profile_error(true, ErrorCode::resource_exhausted,
                           "encoded audio exceeds the configured aggregate byte limit");
    }
    captured_bytes +=
        kEncodedAudioPacketHeaderBytes + packet.payload.size();
  }

  auto relative_start = subtract(*boundary.first_audio_ns,
                                 *boundary.video_origin_ns,
                                 "encoded audio synchronization delta");
  if (!relative_start) {
    return profile_error(true, relative_start.error().code,
                         relative_start.error().message);
  }
  auto relative_start_ns = *relative_start;
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
  if (trim_start_frames >= boundary.decoded_frames) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio has no presentation within the video interval");
  }
  auto start = add_nonnegative(
      request.start_ns, static_cast<std::uint64_t>(relative_start_ns),
      "encoded audio archive timestamp");
  if (!start) {
    return profile_error(true, start.error().code, start.error().message);
  }
  const auto start_ns = *start;
  if (start_ns < request.start_ns || start_ns >= request.end_ns) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio begins outside the requested interval");
  }
  auto maximum_frames = frames_for_ns(
      positive_distance(request.end_ns, start_ns),
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
      *duration_ns > positive_distance(request.end_ns, start_ns)) {
    return profile_error(true, ErrorCode::decode,
                         "encoded audio duration is outside the requested interval");
  }
  auto end = add_nonnegative(start_ns, *duration_ns,
                             "encoded audio archive timestamp");
  if (!end) {
    return profile_error(true, end.error().code, end.error().message);
  }
  const auto end_ns = *end;

  auto source_start = add_nonnegative(
      *boundary.video_origin_ns,
      static_cast<std::uint64_t>(relative_start_ns),
      "encoded audio source timestamp");
  if (!source_start) {
    return profile_error(true, source_start.error().code,
                         source_start.error().message);
  }
  const auto source_window_start = *source_start;
  auto source_end = add_nonnegative(source_window_start, *duration_ns,
                                    "encoded audio source interval");
  if (!source_end) {
    return profile_error(true, source_end.error().code,
                         source_end.error().message);
  }
  const auto source_window_end = *source_end;

  std::vector<EncodedAudioPacket> packets;
  packets.reserve(boundary.packets.size());
  for (auto& packet : boundary.packets) {
    auto packet_end = add_nonnegative(packet.pts_ns, packet.duration_ns,
                                      "encoded audio packet interval");
    if (!packet_end) {
      return profile_error(true, packet_end.error().code,
                           packet_end.error().message);
    }
    if (packet.pts_ns >= source_window_end ||
        *packet_end <= source_window_start) {
      continue;
    }
    if (packet.has_skip_samples) {
      return profile_error(
          true, ErrorCode::model_incompatible,
          "AAC skip/discard side data cannot be represented by EAP1 v1");
    }
    auto pts_offset = subtract(packet.pts_ns, source_window_start,
                               "encoded audio packet PTS offset");
    auto dts_offset = subtract(packet.dts_ns, source_window_start,
                               "encoded audio packet DTS offset");
    if (!pts_offset || !dts_offset) {
      const auto& error = !pts_offset ? pts_offset.error() : dts_offset.error();
      return profile_error(true, error.code, error.message);
    }
    packets.push_back(EncodedAudioPacket{
        .pts_offset_ns = *pts_offset,
        .dts_offset_ns = *dts_offset,
        .duration_ns = packet.duration_ns,
        .flags = packet.flags,
        .payload = std::move(packet.payload),
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
      .decoder_config = std::move(boundary.decoder_config),
      .packets = std::move(packets),
  };
  return FfmpegCapturedEncodedAudio{
      .present = true,
      .state = std::move(state),
      .start_ns = start_ns,
      .end_ns = end_ns,
      .error = std::nullopt,
  };
}

}  // namespace codec::profiles::video::detail
