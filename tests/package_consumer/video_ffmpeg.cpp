#include <codec/profiles/video.hpp>
#include <codec/profiles/video_export.hpp>

int main() {
  const auto ingest_available =
      codec::profiles::video::ffmpeg_video_ingest_available();
  const auto export_available =
      codec::profiles::video::ffmpeg_video_export_available();
  (void)ingest_available;
  (void)export_available;

  codec::profiles::video::FfmpegVideoIngestRequest request{};
  if (request.maximum_hls_resources == 0U ||
      request.maximum_hls_resource_bytes == 0U ||
      request.maximum_hls_total_bytes == 0U) {
    return 1;
  }
  const codec::profiles::video::FfmpegVideoIngestReport report{};
  if (!report.secondary_descriptors.empty() ||
      !report.secondary_sources.empty()) {
    return 1;
  }
  const codec::profiles::video::VideoMp4ExportLimits export_limits{};
  if (export_limits.maximum_output_bytes == 0U) {
    return 1;
  }
  const codec::profiles::video::VerifiedVideoMp4Export export_report{};
  if (!export_report.state_records.empty() ||
      !export_report.provenance.empty()) {
    return 1;
  }
  auto invalid = codec::profiles::video::ingest_video_ffmpeg(request);
  if (invalid || invalid.error().code != codec::ErrorCode::invalid_argument) {
    return 1;
  }
  return 0;
}