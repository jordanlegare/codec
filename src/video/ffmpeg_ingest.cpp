#include <codec/profiles/video.hpp>

#include "../core/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace codec::profiles::video {
namespace {

constexpr std::size_t minimum_capture_chunk_bytes = 4096U;
constexpr std::size_t maximum_capture_chunk_bytes = 16U * 1024U * 1024U;
constexpr std::uint32_t maximum_capture_redirects = 20U;

bool supported_layout(PixelLayout layout) noexcept {
  switch (layout) {
    case PixelLayout::gray8:
    case PixelLayout::rgb24:
    case PixelLayout::rgba32:
    case PixelLayout::yuv420p8:
      return true;
  }
  return false;
}

Result<void> validate_request(const FfmpegVideoIngestRequest& request) {
  if (request.source_uri.empty()) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest requires a source URI");
  }
  if (request.archive_path.empty() || request.archive_path.filename().empty()) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest requires an archive file path");
  }
  if (request.end_ns <= request.start_ns) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest interval must have positive duration");
  }
  if (request.capture_chunk_bytes < minimum_capture_chunk_bytes ||
      request.capture_chunk_bytes > maximum_capture_chunk_bytes) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest capture chunk is outside the supported bounds");
  }
  if (request.maximum_source_bytes == 0U ||
      request.maximum_source_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest source limit is outside the process bounds");
  }
  if (request.maximum_decoded_bytes == 0U ||
      request.maximum_decoded_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest decoded limit is outside the process bounds");
  }
  if (request.maximum_frames == 0U) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest frame limit must be non-zero");
  }
  if (request.maximum_redirects > maximum_capture_redirects) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest redirect limit is outside the supported bounds");
  }
  if (request.descriptor.type != StreamType::video ||
      request.descriptor.payload_type.empty()) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest requires a typed video stream descriptor");
  }
  if (!supported_layout(request.output_layout)) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest output layout is unsupported");
  }
  auto encoded_descriptor =
      ::codec::detail::encode_stream_descriptor(request.descriptor);
  if (!encoded_descriptor) return encoded_descriptor.error();

  std::error_code inspection_error;
  const auto status =
      std::filesystem::symlink_status(request.archive_path, inspection_error);
  if (inspection_error == std::errc::no_such_file_or_directory) {
    inspection_error.clear();
  } else if (inspection_error) {
    return fail(ErrorCode::archive_io,
                "cannot inspect FFmpeg video ingest archive output: " +
                    inspection_error.message());
  }
  if (!inspection_error &&
      status.type() != std::filesystem::file_type::not_found &&
      status.type() != std::filesystem::file_type::none) {
    return fail(ErrorCode::archive_io,
                "FFmpeg video ingest refuses to replace an existing archive path");
  }
  return {};
}

}  // namespace

bool ffmpeg_video_ingest_available() noexcept {
#ifdef CODEC_HAS_FFMPEG_VIDEO
  return true;
#else
  return false;
#endif
}

Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request) {
  auto valid = validate_request(request);
  if (!valid) return valid.error();
  return fail<FfmpegVideoIngestReport>(
      ErrorCode::model_incompatible,
      "FFmpeg video ingest backend is unavailable");
}

}  // namespace codec::profiles::video
