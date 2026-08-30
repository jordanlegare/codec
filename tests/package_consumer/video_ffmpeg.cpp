#include <codec/profiles/video.hpp>

int main() {
  const auto available =
      codec::profiles::video::ffmpeg_video_ingest_available();
  (void)available;

  codec::profiles::video::FfmpegVideoIngestRequest request{};
  auto invalid = codec::profiles::video::ingest_video_ffmpeg(request);
  if (invalid || invalid.error().code != codec::ErrorCode::invalid_argument) {
    return 1;
  }
  return 0;
}
