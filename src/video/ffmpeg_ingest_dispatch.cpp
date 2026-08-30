#ifdef CODEC_HAS_FFMPEG_VIDEO
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace {

int open_hls_without_http_persistence(AVFormatContext** context,
                                      const char* url,
                                      const AVInputFormat* format,
                                      AVDictionary** options) {
  if (format != nullptr && format->name != nullptr &&
      std::strcmp(format->name, "hls") == 0 && options == nullptr) {
    AVDictionary* hls_options = nullptr;
    const auto configured =
        av_dict_set(&hls_options, "http_persistent", "0", 0);
    if (configured < 0) {
      av_dict_free(&hls_options);
      return configured;
    }
    const auto opened =
        avformat_open_input(context, url, format, &hls_options);
    av_dict_free(&hls_options);
    return opened;
  }
  return avformat_open_input(context, url, format, options);
}

}  // namespace

#define avformat_open_input open_hls_without_http_persistence
#endif

#define ffmpeg_video_ingest_available ffmpeg_video_ingest_available_hls_embedded
#define ingest_video_ffmpeg_direct ingest_video_ffmpeg_hls_embedded_direct
#include "ffmpeg_ingest_hls.cpp"
#undef ingest_video_ffmpeg_direct
#undef ffmpeg_video_ingest_available

#ifdef CODEC_HAS_FFMPEG_VIDEO
#undef avformat_open_input
#endif
