#include <codec/profiles/video.hpp>

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
  return ingest_video_ffmpeg_direct(request);
}

}  // namespace codec::profiles::video
