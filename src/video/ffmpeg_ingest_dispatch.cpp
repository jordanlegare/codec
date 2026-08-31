#ifdef CODEC_HAS_FFMPEG_VIDEO
#include "ffmpeg_audio_capture.hpp"
#include "hls_policy.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace {

constexpr AVRational kNanosecondTimeBase{1, 1'000'000'000};

struct HlsDecodeBoundary {
  bool active{};
  void* session{};
  std::int64_t requested_duration_ns{};
  AVRational time_base{0, 1};
  AVRational frame_rate{0, 1};
  bool timeline_initialized{};
  bool synthetic_timeline{};
  std::int64_t origin_timestamp{AV_NOPTS_VALUE};
  std::int64_t previous_relative_ns{-1};
  std::size_t frame_index{};
  bool duration_reached{};
};

struct AudioDecodeBoundary {
  bool enabled{};
  bool present{};
  bool decoder_ready{};
  bool flushed{};
  std::uint64_t maximum_bytes{};
  std::uint64_t captured_bytes{};
  int stream_index{-1};
  AVRational time_base{0, 1};
  AVCodecContext* codec{};
  AVFrame* frame{};
  codec::profiles::video::EncodedAudioCodec encoded_codec{
      codec::profiles::video::EncodedAudioCodec::aac};
  std::int32_t codec_profile{};
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::uint64_t decoded_frames{};
  std::vector<std::byte> decoder_config;
  std::vector<codec::profiles::video::detail::FfmpegCapturedEncodedPacket>
      packets;
  std::optional<std::int64_t> first_audio_ns{};
  AVRational video_time_base{0, 1};
  std::int64_t video_origin_timestamp{AV_NOPTS_VALUE};
  std::optional<codec::Error> error{};
};

thread_local HlsDecodeBoundary hls_boundary;
thread_local AudioDecodeBoundary audio_boundary;

bool hls_callback_failed(void* opaque) noexcept;
std::int64_t hls_requested_duration_ns(void* opaque) noexcept;

int codec_avformat_open_input(AVFormatContext** context, const char* url,
                              const AVInputFormat* format,
                              AVDictionary** options);
int codec_avformat_find_stream_info(AVFormatContext* context,
                                    AVDictionary** options);
int codec_av_find_best_stream(AVFormatContext* context, AVMediaType type,
                              int wanted_stream, int related_stream,
                              const AVCodec** decoder, int flags);
int codec_av_read_frame(AVFormatContext* context, AVPacket* packet);
int codec_avcodec_receive_frame(AVCodecContext* codec, AVFrame* frame);

void clear_audio_boundary() noexcept {
  if (audio_boundary.frame != nullptr) {
    av_frame_free(&audio_boundary.frame);
  }
  if (audio_boundary.codec != nullptr) {
    avcodec_free_context(&audio_boundary.codec);
  }
  audio_boundary = {};
}

std::string ffmpeg_audio_error_text(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error " + std::to_string(code);
  }
  return std::string{buffer.data()};
}

void remember_audio_error(codec::ErrorCode code, std::string message) {
  if (!audio_boundary.error.has_value()) {
    audio_boundary.error = codec::Error{code, std::move(message), false};
  }
  audio_boundary.decoder_ready = false;
}

void remember_audio_ffmpeg_error(std::string message, int code) {
  message += ": ";
  message += ffmpeg_audio_error_text(code);
  remember_audio_error(codec::ErrorCode::decode, std::move(message));
}

void configure_audio_decoder(AVFormatContext* context) {
  if (!audio_boundary.enabled || context == nullptr ||
      audio_boundary.error.has_value()) {
    return;
  }

  for (unsigned index = 0; index < context->nb_streams; ++index) {
    const auto* stream = context->streams[index];
    if (stream != nullptr && stream->codecpar != nullptr &&
        stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audio_boundary.present = true;
      break;
    }
  }
  if (!audio_boundary.present) return;

  const AVCodec* decoder = nullptr;
  const auto stream_index = av_find_best_stream(
      context, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
  if (stream_index < 0 || decoder == nullptr) {
    remember_audio_ffmpeg_error(
        "captured media has audio but no decodable audio stream",
        stream_index < 0 ? stream_index : AVERROR_DECODER_NOT_FOUND);
    return;
  }
  audio_boundary.stream_index = stream_index;
  if (static_cast<unsigned>(stream_index) >= context->nb_streams) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected FFmpeg audio stream index is invalid");
    return;
  }
  auto* stream = context->streams[stream_index];
  if (stream == nullptr || stream->codecpar == nullptr ||
      stream->time_base.num <= 0 || stream->time_base.den <= 0) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected FFmpeg audio stream has invalid metadata");
    return;
  }
  audio_boundary.time_base = stream->time_base;
  if (stream->codecpar->codec_id != AV_CODEC_ID_AAC) {
    remember_audio_error(codec::ErrorCode::decode,
                         "H.1 encoded audio v1 supports AAC only");
    return;
  }
  if (stream->codecpar->extradata_size < 0 ||
      (stream->codecpar->extradata_size > 0 &&
       stream->codecpar->extradata == nullptr)) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected FFmpeg audio stream has invalid decoder configuration");
    return;
  }
  const auto configuration_bytes =
      static_cast<std::uint64_t>(stream->codecpar->extradata_size);
  if (configuration_bytes > audio_boundary.maximum_bytes) {
    remember_audio_error(codec::ErrorCode::resource_exhausted,
                         "encoded audio decoder configuration exceeds the configured limit");
    return;
  }
  audio_boundary.codec_profile = stream->codecpar->profile;
  if (stream->codecpar->extradata_size > 0) {
    const auto* configuration =
        reinterpret_cast<const std::byte*>(stream->codecpar->extradata);
    audio_boundary.decoder_config.assign(
        configuration, configuration + stream->codecpar->extradata_size);
  }
  audio_boundary.captured_bytes = configuration_bytes;

  auto* codec_context = avcodec_alloc_context3(decoder);
  if (codec_context == nullptr) {
    remember_audio_error(codec::ErrorCode::resource_exhausted,
                         "cannot allocate FFmpeg audio decoder context");
    return;
  }
  audio_boundary.codec = codec_context;
  const auto copied =
      avcodec_parameters_to_context(codec_context, stream->codecpar);
  if (copied < 0) {
    remember_audio_ffmpeg_error("FFmpeg cannot copy audio codec parameters",
                                copied);
    return;
  }
  codec_context->thread_count = 1;
  const auto opened = avcodec_open2(codec_context, decoder, nullptr);
  if (opened < 0) {
    remember_audio_ffmpeg_error("FFmpeg cannot open the selected audio decoder",
                                opened);
    return;
  }

  if (codec_context->sample_rate <= 0 ||
      static_cast<std::uint64_t>(codec_context->sample_rate) >
          std::numeric_limits<std::uint32_t>::max()) {
    remember_audio_error(codec::ErrorCode::decode,
                         "decoded audio sample rate is unsupported");
    return;
  }
  const auto channel_count = codec_context->ch_layout.nb_channels;
  if (channel_count < 1 || channel_count > 2) {
    remember_audio_error(codec::ErrorCode::decode,
                         "H.1 audio canonicalization supports mono or stereo only");
    return;
  }
  audio_boundary.sample_rate =
      static_cast<std::uint32_t>(codec_context->sample_rate);
  audio_boundary.channels = static_cast<std::uint16_t>(channel_count);

  audio_boundary.frame = av_frame_alloc();
  if (audio_boundary.frame == nullptr) {
    remember_audio_error(codec::ErrorCode::resource_exhausted,
                         "cannot allocate FFmpeg audio frame");
    return;
  }
  audio_boundary.decoder_ready = true;
}

void validate_decoded_audio(const AVFrame& frame) {
  if (!audio_boundary.decoder_ready || audio_boundary.error.has_value()) {
    return;
  }
  if (frame.nb_samples <= 0 || frame.best_effort_timestamp == AV_NOPTS_VALUE) {
    remember_audio_error(codec::ErrorCode::decode,
                         "decoded audio frame lacks usable samples or timestamp");
    return;
  }
  if (frame.sample_rate > 0 &&
      static_cast<std::uint32_t>(frame.sample_rate) !=
          audio_boundary.sample_rate) {
    remember_audio_error(codec::ErrorCode::decode,
                         "decoded audio sample rate changed within the stream");
    return;
  }
  if (frame.ch_layout.nb_channels > 0 &&
      frame.ch_layout.nb_channels != audio_boundary.channels) {
    remember_audio_error(codec::ErrorCode::decode,
                         "decoded audio channel count changed within the stream");
    return;
  }

  const auto chunk_start_ns = av_rescale_q(frame.best_effort_timestamp,
                                           audio_boundary.time_base,
                                           kNanosecondTimeBase);
  if (!audio_boundary.first_audio_ns.has_value()) {
    audio_boundary.first_audio_ns = chunk_start_ns;
  } else {
    if (audio_boundary.decoded_frames >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      remember_audio_error(codec::ErrorCode::resource_exhausted,
                           "decoded audio frame count exceeds time-conversion bounds");
      return;
    }
    const auto expected =
        *audio_boundary.first_audio_ns +
        av_rescale_rnd(static_cast<std::int64_t>(audio_boundary.decoded_frames),
                       1'000'000'000LL, audio_boundary.sample_rate,
                       AV_ROUND_DOWN);
    const auto sample_tolerance = av_rescale_rnd(
        1, 1'000'000'000LL, audio_boundary.sample_rate, AV_ROUND_UP);
    const auto difference = chunk_start_ns >= expected
                                ? chunk_start_ns - expected
                                : expected - chunk_start_ns;
    if (difference > sample_tolerance) {
      remember_audio_error(codec::ErrorCode::decode,
                           "decoded audio timeline is discontinuous");
      return;
    }
  }
  const auto decoded = static_cast<std::uint64_t>(frame.nb_samples);
  if (decoded > std::numeric_limits<std::uint64_t>::max() -
                    audio_boundary.decoded_frames) {
    remember_audio_error(codec::ErrorCode::resource_exhausted,
                         "decoded audio frame count exceeds bounds");
    return;
  }
  audio_boundary.decoded_frames += decoded;
}

void receive_audio_frames() {
  if (!audio_boundary.decoder_ready || audio_boundary.codec == nullptr ||
      audio_boundary.frame == nullptr || audio_boundary.error.has_value()) {
    return;
  }
  for (;;) {
    const auto received =
        avcodec_receive_frame(audio_boundary.codec, audio_boundary.frame);
    if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
    if (received < 0) {
      remember_audio_ffmpeg_error("FFmpeg audio frame decode failed", received);
      return;
    }
    validate_decoded_audio(*audio_boundary.frame);
    av_frame_unref(audio_boundary.frame);
    if (audio_boundary.error.has_value()) return;
  }
}

void flush_audio_decoder() {
  if (audio_boundary.flushed) return;
  audio_boundary.flushed = true;
  if (!audio_boundary.decoder_ready || audio_boundary.codec == nullptr ||
      audio_boundary.error.has_value()) {
    return;
  }
  const auto sent = avcodec_send_packet(audio_boundary.codec, nullptr);
  if (sent < 0 && sent != AVERROR_EOF) {
    remember_audio_ffmpeg_error("FFmpeg audio decoder flush failed", sent);
    return;
  }
  receive_audio_frames();
}

void capture_encoded_audio_packet(const AVPacket& packet) {
  if (!audio_boundary.decoder_ready || audio_boundary.error.has_value()) return;
  if (packet.data == nullptr || packet.size <= 0) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected audio packet has no encoded payload");
    return;
  }
  auto pts = packet.pts;
  auto dts = packet.dts;
  if (pts == AV_NOPTS_VALUE) pts = dts;
  if (dts == AV_NOPTS_VALUE) dts = pts;
  if (pts == AV_NOPTS_VALUE || dts == AV_NOPTS_VALUE) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected audio packet lacks usable timestamps");
    return;
  }
  const auto payload_bytes = static_cast<std::uint64_t>(packet.size);
  if (audio_boundary.captured_bytes > audio_boundary.maximum_bytes ||
      payload_bytes >
          audio_boundary.maximum_bytes - audio_boundary.captured_bytes) {
    remember_audio_error(
        codec::ErrorCode::resource_exhausted,
        "encoded audio exceeds the configured aggregate byte limit");
    return;
  }

  auto duration = packet.duration;
  if (duration <= 0) {
    const auto frame_size = audio_boundary.codec != nullptr &&
                                    audio_boundary.codec->frame_size > 0
                                ? audio_boundary.codec->frame_size
                                : 1024;
    duration = av_rescale_q(frame_size,
                            AVRational{1, static_cast<int>(
                                              audio_boundary.sample_rate)},
                            audio_boundary.time_base);
  }
  if (duration <= 0) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected audio packet has no usable duration");
    return;
  }

  const auto pts_ns = av_rescale_q(pts, audio_boundary.time_base,
                                   kNanosecondTimeBase);
  const auto dts_ns = av_rescale_q(dts, audio_boundary.time_base,
                                   kNanosecondTimeBase);
  const auto duration_ns = av_rescale_q(duration, audio_boundary.time_base,
                                        kNanosecondTimeBase);
  if (duration_ns <= 0) {
    remember_audio_error(codec::ErrorCode::decode,
                         "selected audio packet duration is below nanosecond resolution");
    return;
  }

  std::vector<std::byte> payload(payload_bytes);
  std::memcpy(payload.data(), packet.data, static_cast<std::size_t>(packet.size));
  audio_boundary.packets.push_back(
      codec::profiles::video::detail::FfmpegCapturedEncodedPacket{
          .pts_ns = pts_ns,
          .dts_ns = dts_ns,
          .duration_ns = static_cast<std::uint64_t>(duration_ns),
          .flags = static_cast<std::uint32_t>(packet.flags) & 0x1fU,
          .payload = std::move(payload),
      });
  audio_boundary.captured_bytes += payload_bytes;
}

}  // namespace

namespace codec::profiles::video::detail {

Result<void> codec_require_same_hls_origin(const HlsOrigin& primary,
                                           std::string_view child_uri);

}  // namespace codec::profiles::video::detail

#define avformat_open_input codec_avformat_open_input
#define avformat_find_stream_info codec_avformat_find_stream_info
#define av_find_best_stream codec_av_find_best_stream
#define av_read_frame codec_av_read_frame
#define avcodec_receive_frame codec_avcodec_receive_frame
#define require_same_hls_origin codec_require_same_hls_origin
#endif

#define ffmpeg_video_ingest_available ffmpeg_video_ingest_available_hls_embedded
#define ingest_video_ffmpeg_direct ingest_video_ffmpeg_hls_embedded_direct
#include "ffmpeg_ingest_hls.cpp"
#undef ingest_video_ffmpeg_direct
#undef ffmpeg_video_ingest_available

#ifdef CODEC_HAS_FFMPEG_VIDEO
#undef require_same_hls_origin
#undef avcodec_receive_frame
#undef av_read_frame
#undef av_find_best_stream
#undef avformat_find_stream_info
#undef avformat_open_input

namespace {

bool hls_callback_failed(void* opaque) noexcept {
  if (opaque == nullptr) return false;
  const auto* session =
      static_cast<const codec::profiles::video::HlsCaptureSession*>(opaque);
  return session->callback_error.has_value();
}

std::int64_t hls_requested_duration_ns(void* opaque) noexcept {
  if (opaque == nullptr) return 0;
  const auto* session =
      static_cast<const codec::profiles::video::HlsCaptureSession*>(opaque);
  if (session->request == nullptr) return 0;
  const auto start = session->request->start_ns;
  const auto end = session->request->end_ns;
  if (start < 0 && end > std::numeric_limits<std::int64_t>::max() + start) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return end - start;
}

bool is_hls_format(const AVInputFormat* format) noexcept {
  return format != nullptr && format->name != nullptr &&
         std::strcmp(format->name, "hls") == 0;
}

int codec_avformat_open_input(AVFormatContext** context, const char* url,
                              const AVInputFormat* format,
                              AVDictionary** options) {
  const bool hls = is_hls_format(format);
  if (!hls) {
    hls_boundary = {};
    return avformat_open_input(context, url, format, options);
  }

  void* opaque =
      context != nullptr && *context != nullptr ? (*context)->opaque : nullptr;
  hls_boundary = HlsDecodeBoundary{
      .active = true,
      .session = opaque,
      .requested_duration_ns = hls_requested_duration_ns(opaque),
  };

  int opened = 0;
  if (options == nullptr) {
    AVDictionary* hls_options = nullptr;
    const auto configured =
        av_dict_set(&hls_options, "http_persistent", "0", 0);
    if (configured < 0) {
      av_dict_free(&hls_options);
      return configured;
    }
    opened = avformat_open_input(context, url, format, &hls_options);
    av_dict_free(&hls_options);
  } else {
    opened = avformat_open_input(context, url, format, options);
  }

  if (hls_callback_failed(hls_boundary.session)) {
    if (opened >= 0 && context != nullptr && *context != nullptr) {
      avformat_close_input(context);
    }
    return AVERROR_EXIT;
  }
  return opened;
}

int codec_avformat_find_stream_info(AVFormatContext* context,
                                    AVDictionary** options) {
  const auto result = avformat_find_stream_info(context, options);
  if (result >= 0) configure_audio_decoder(context);
  if (hls_boundary.active && hls_callback_failed(hls_boundary.session)) {
    return AVERROR_EXIT;
  }
  return result;
}

int codec_av_find_best_stream(AVFormatContext* context, AVMediaType type,
                              int wanted_stream, int related_stream,
                              const AVCodec** decoder, int flags) {
  const auto result = av_find_best_stream(context, type, wanted_stream,
                                          related_stream, decoder, flags);
  if (hls_boundary.active && hls_callback_failed(hls_boundary.session)) {
    return AVERROR_EXIT;
  }
  if (result >= 0 && type == AVMEDIA_TYPE_VIDEO && context != nullptr &&
      static_cast<unsigned>(result) < context->nb_streams &&
      context->streams[result] != nullptr) {
    const auto* stream = context->streams[result];
    audio_boundary.video_time_base = stream->time_base;
    if (hls_boundary.active) {
      hls_boundary.time_base = stream->time_base;
      hls_boundary.frame_rate =
          stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
              ? stream->avg_frame_rate
              : stream->r_frame_rate;
    }
  }
  return result;
}

int codec_av_read_frame(AVFormatContext* context, AVPacket* packet) {
  if (hls_boundary.active && hls_boundary.duration_reached) {
    flush_audio_decoder();
    return AVERROR_EOF;
  }

  for (;;) {
    const auto result = av_read_frame(context, packet);
    if (hls_boundary.active && hls_callback_failed(hls_boundary.session)) {
      if (result >= 0 && packet != nullptr) av_packet_unref(packet);
      return AVERROR_EXIT;
    }
    if (result == AVERROR_EOF) {
      flush_audio_decoder();
      return result;
    }
    if (result < 0 || packet == nullptr) return result;

    if (audio_boundary.enabled && audio_boundary.present &&
        packet->stream_index == audio_boundary.stream_index) {
      if (audio_boundary.decoder_ready && !audio_boundary.error.has_value()) {
        capture_encoded_audio_packet(*packet);
      }
      if (audio_boundary.decoder_ready && !audio_boundary.error.has_value()) {
        const auto sent =
            avcodec_send_packet(audio_boundary.codec, packet);
        if (sent < 0) {
          remember_audio_ffmpeg_error("FFmpeg audio packet decode failed", sent);
        } else {
          receive_audio_frames();
        }
      }
      av_packet_unref(packet);
      continue;
    }
    return result;
  }
}

std::int64_t nominal_hls_frame_duration_ns() noexcept {
  const auto rate = hls_boundary.frame_rate;
  if (rate.num <= 0 || rate.den <= 0) return 0;
  const AVRational frame_duration{rate.den, rate.num};
  return av_rescale_q(1, frame_duration, kNanosecondTimeBase);
}

bool hls_frame_relative_start(const AVFrame& frame,
                              std::int64_t& relative_ns) noexcept {
  const auto timestamp = frame.best_effort_timestamp;
  if (!hls_boundary.timeline_initialized) {
    hls_boundary.timeline_initialized = true;
    hls_boundary.synthetic_timeline = timestamp == AV_NOPTS_VALUE;
    if (!hls_boundary.synthetic_timeline) {
      hls_boundary.origin_timestamp = timestamp;
    }
  }

  if (hls_boundary.synthetic_timeline || timestamp == AV_NOPTS_VALUE) {
    const auto nominal_ns = nominal_hls_frame_duration_ns();
    if (nominal_ns <= 0) return false;
    if (hls_boundary.frame_index == 0U) {
      relative_ns = 0;
    } else {
      if (hls_boundary.previous_relative_ns < 0 ||
          hls_boundary.previous_relative_ns >
              std::numeric_limits<std::int64_t>::max() - nominal_ns) {
        return false;
      }
      relative_ns = hls_boundary.previous_relative_ns + nominal_ns;
    }
    return true;
  }

  const auto origin = hls_boundary.origin_timestamp;
  if (origin == AV_NOPTS_VALUE || timestamp < origin) return false;
  if (origin < 0 &&
      timestamp > std::numeric_limits<std::int64_t>::max() + origin) {
    return false;
  }
  const auto delta = timestamp - origin;
  if (hls_boundary.time_base.num <= 0 || hls_boundary.time_base.den <= 0) {
    return false;
  }
  relative_ns =
      av_rescale_q(delta, hls_boundary.time_base, kNanosecondTimeBase);
  return relative_ns >= 0;
}

int codec_avcodec_receive_frame(AVCodecContext* codec, AVFrame* frame) {
  if (hls_boundary.active && hls_boundary.duration_reached) {
    return AVERROR_EOF;
  }
  const auto result = avcodec_receive_frame(codec, frame);
  if (result >= 0 && frame != nullptr && audio_boundary.enabled &&
      audio_boundary.video_origin_timestamp == AV_NOPTS_VALUE) {
    audio_boundary.video_origin_timestamp = frame->best_effort_timestamp;
  }
  if (!hls_boundary.active || result < 0 || frame == nullptr) return result;

  std::int64_t relative_ns = 0;
  if (hls_frame_relative_start(*frame, relative_ns)) {
    if (relative_ns >= hls_boundary.requested_duration_ns) {
      hls_boundary.duration_reached = true;
      av_frame_unref(frame);
      return AVERROR_EOF;
    }
    hls_boundary.previous_relative_ns = relative_ns;
  }
  ++hls_boundary.frame_index;
  return result;
}

}  // namespace

namespace codec::profiles::video::detail {

Result<void> begin_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request, bool enabled) {
  clear_audio_boundary();
  if (request.maximum_decoded_audio_bytes == 0U ||
      request.maximum_decoded_audio_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest audio decoded limit is outside process bounds");
  }
  audio_boundary.enabled = enabled;
  audio_boundary.maximum_bytes = request.maximum_decoded_audio_bytes;
  return {};
}

FfmpegCapturedEncodedAudio take_ffmpeg_audio_capture(
    const FfmpegVideoIngestRequest& request) {
  flush_audio_decoder();
  std::optional<std::int64_t> video_origin_ns;
  if (audio_boundary.video_origin_timestamp != AV_NOPTS_VALUE &&
      audio_boundary.video_time_base.num > 0 &&
      audio_boundary.video_time_base.den > 0) {
    video_origin_ns = av_rescale_q(audio_boundary.video_origin_timestamp,
                                   audio_boundary.video_time_base,
                                   kNanosecondTimeBase);
  }
  FfmpegEncodedAudioCaptureBoundary boundary{
      .present = audio_boundary.enabled && audio_boundary.present,
      .codec = audio_boundary.encoded_codec,
      .codec_profile = audio_boundary.codec_profile,
      .sample_rate = audio_boundary.sample_rate,
      .channels = audio_boundary.channels,
      .decoded_frames = audio_boundary.decoded_frames,
      .first_audio_ns = audio_boundary.first_audio_ns,
      .video_origin_ns = video_origin_ns,
      .decoder_config = std::move(audio_boundary.decoder_config),
      .packets = std::move(audio_boundary.packets),
      .error = audio_boundary.error,
  };
  auto finalized = finalize_ffmpeg_encoded_audio_capture(request, boundary);
  FfmpegCapturedEncodedAudio result;
  if (finalized) {
    result = std::move(*finalized);
  } else {
    result = FfmpegCapturedEncodedAudio{
        .present = boundary.present,
        .state = std::nullopt,
        .start_ns = 0,
        .end_ns = 0,
        .error = finalized.error(),
    };
  }
  clear_audio_boundary();
  return result;
}

Result<void> codec_require_same_hls_origin(const HlsOrigin& primary,
                                           std::string_view child_uri) {
  if (hls_boundary.active && hls_callback_failed(hls_boundary.session)) {
    const auto* session =
        static_cast<const codec::profiles::video::HlsCaptureSession*>(
            hls_boundary.session);
    if (session != nullptr && session->callback_error.has_value()) {
      return *session->callback_error;
    }
    return fail(ErrorCode::internal,
                "HLS ingest stopped after a prior resource failure");
  }
  return require_same_hls_origin(primary, child_uri);
}

}  // namespace codec::profiles::video::detail
#endif
