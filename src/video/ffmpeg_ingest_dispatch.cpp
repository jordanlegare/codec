#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace codec::profiles::video {

Result<FfmpegVideoIngestReport> ingest_video_ffmpeg_direct(
    const FfmpegVideoIngestRequest& request);

Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request) {
  if (request.maximum_hls_resources == 0U ||
      request.maximum_hls_resource_bytes == 0U ||
      request.maximum_hls_total_bytes == 0U) {
    return fail<FfmpegVideoIngestReport>(
        ErrorCode::invalid_argument,
        "FFmpeg video ingest HLS limits must all be non-zero");
  }
  if (request.maximum_hls_resources > 999999U) {
    return fail<FfmpegVideoIngestReport>(
        ErrorCode::invalid_argument,
        "FFmpeg video ingest HLS resource limit exceeds child identity bounds");
  }
  if (request.maximum_hls_resource_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      request.maximum_hls_total_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail<FfmpegVideoIngestReport>(
        ErrorCode::invalid_argument,
        "FFmpeg video ingest HLS byte limits exceed process bounds");
  }
  return ingest_video_ffmpeg_direct(request);
}

}  // namespace codec::profiles::video
