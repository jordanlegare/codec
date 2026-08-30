#ifdef CODEC_HAS_FFMPEG_VIDEO
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}

namespace {

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

thread_local HlsDecodeBoundary hls_boundary;

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

}  // namespace

#define avformat_open_input codec_avformat_open_input
#define avformat_find_stream_info codec_avformat_find_stream_info
#define av_find_best_stream codec_av_find_best_stream
#define av_read_frame codec_av_read_frame
#define avcodec_receive_frame codec_avcodec_receive_frame
#endif

#define ffmpeg_video_ingest_available ffmpeg_video_ingest_available_hls_embedded
#define ingest_video_ffmpeg_direct ingest_video_ffmpeg_hls_embedded_direct
#include "ffmpeg_ingest_hls.cpp"
#undef ingest_video_ffmpeg_direct
#undef ffmpeg_video_ingest_available

#ifdef CODEC_HAS_FFMPEG_VIDEO
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

  if (hls_callback_failed(hls_boundary.session)) return AVERROR_EXIT;
  return opened;
}

int codec_avformat_find_stream_info(AVFormatContext* context,
                                    AVDictionary** options) {
  const auto result = avformat_find_stream_info(context, options);
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
  if (hls_boundary.active && result >= 0 && type == AVMEDIA_TYPE_VIDEO &&
      context != nullptr &&
      static_cast<unsigned>(result) < context->nb_streams &&
      context->streams[result] != nullptr) {
    const auto* stream = context->streams[result];
    hls_boundary.time_base = stream->time_base;
    hls_boundary.frame_rate =
        stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
            ? stream->avg_frame_rate
            : stream->r_frame_rate;
  }
  return result;
}

int codec_av_read_frame(AVFormatContext* context, AVPacket* packet) {
  if (hls_boundary.active && hls_boundary.duration_reached) {
    return AVERROR_EOF;
  }
  const auto result = av_read_frame(context, packet);
  if (hls_boundary.active && hls_callback_failed(hls_boundary.session)) {
    if (result >= 0 && packet != nullptr) av_packet_unref(packet);
    return AVERROR_EXIT;
  }
  return result;
}

std::int64_t nominal_hls_frame_duration_ns() noexcept {
  const auto rate = hls_boundary.frame_rate;
  if (rate.num <= 0 || rate.den <= 0) return 0;
  const AVRational frame_duration{rate.den, rate.num};
  const AVRational nanoseconds{1, 1'000'000'000};
  return av_rescale_q(1, frame_duration, nanoseconds);
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
  const AVRational nanoseconds{1, 1'000'000'000};
  relative_ns = av_rescale_q(delta, hls_boundary.time_base, nanoseconds);
  return relative_ns >= 0;
}

int codec_avcodec_receive_frame(AVCodecContext* codec, AVFrame* frame) {
  if (hls_boundary.active && hls_boundary.duration_reached) {
    return AVERROR_EOF;
  }
  const auto result = avcodec_receive_frame(codec, frame);
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
#endif
