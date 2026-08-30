#include <codec/integrity.hpp>
#include <codec/profiles/video.hpp>

#include "../capture/capture.hpp"
#include "../core/internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef CODEC_HAS_FFMPEG_VIDEO
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}
#endif

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
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      request.maximum_source_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
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

#ifdef CODEC_HAS_FFMPEG_VIDEO

constexpr AVRational nanosecond_time_base{1, 1'000'000'000};

std::string ffmpeg_error_text(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error " + std::to_string(code);
  }
  return std::string{buffer.data()};
}

Error ffmpeg_decode_error(std::string message, int code) {
  message += ": ";
  message += ffmpeg_error_text(code);
  return Error{ErrorCode::decode, std::move(message), false};
}

struct MemoryInput {
  std::span<const std::byte> bytes;
  std::size_t offset{};
};

int memory_read(void* opaque, std::uint8_t* buffer, int buffer_size) {
  if (opaque == nullptr || buffer == nullptr || buffer_size <= 0) {
    return AVERROR(EINVAL);
  }
  auto& input = *static_cast<MemoryInput*>(opaque);
  if (input.offset >= input.bytes.size()) return AVERROR_EOF;
  const auto requested = static_cast<std::size_t>(buffer_size);
  const auto count = std::min(requested, input.bytes.size() - input.offset);
  std::memcpy(buffer, input.bytes.data() + input.offset, count);
  input.offset += count;
  return static_cast<int>(count);
}

std::int64_t memory_seek(void* opaque, std::int64_t offset, int whence) {
  if (opaque == nullptr) return AVERROR(EINVAL);
  auto& input = *static_cast<MemoryInput*>(opaque);
  if ((whence & AVSEEK_SIZE) != 0) {
    return static_cast<std::int64_t>(input.bytes.size());
  }

  const int origin = whence & ~AVSEEK_FORCE;
  std::int64_t base = 0;
  switch (origin) {
    case SEEK_SET:
      base = 0;
      break;
    case SEEK_CUR:
      base = static_cast<std::int64_t>(input.offset);
      break;
    case SEEK_END:
      base = static_cast<std::int64_t>(input.bytes.size());
      break;
    default:
      return AVERROR(EINVAL);
  }

  if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
      (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)) {
    return AVERROR(EOVERFLOW);
  }
  const auto target = base + offset;
  if (target < 0 ||
      static_cast<std::uint64_t>(target) > input.bytes.size()) {
    return AVERROR(EINVAL);
  }
  input.offset = static_cast<std::size_t>(target);
  return target;
}

int deny_secondary_io_open(AVFormatContext*, AVIOContext**, const char*, int,
                           AVDictionary**) {
  return AVERROR(EPERM);
}

struct AvioDeleter {
  void operator()(AVIOContext* context) const noexcept {
    if (context == nullptr) return;
    av_freep(&context->buffer);
    avio_context_free(&context);
  }
};

struct FormatDeleter {
  void operator()(AVFormatContext* context) const noexcept {
    if (context == nullptr) return;
    avformat_close_input(&context);
  }
};

struct CodecDeleter {
  void operator()(AVCodecContext* context) const noexcept {
    if (context == nullptr) return;
    avcodec_free_context(&context);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept {
    if (packet == nullptr) return;
    av_packet_free(&packet);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept {
    if (frame == nullptr) return;
    av_frame_free(&frame);
  }
};

struct SwsDeleter {
  void operator()(SwsContext* context) const noexcept {
    if (context != nullptr) sws_freeContext(context);
  }
};

using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

AVPixelFormat target_pixel_format(PixelLayout layout) {
  switch (layout) {
    case PixelLayout::gray8:
      return AV_PIX_FMT_GRAY8;
    case PixelLayout::rgb24:
      return AV_PIX_FMT_RGB24;
    case PixelLayout::rgba32:
      return AV_PIX_FMT_RGBA;
    case PixelLayout::yuv420p8:
      return AV_PIX_FMT_YUV420P;
  }
  return AV_PIX_FMT_NONE;
}

ColorRange map_color_range(AVColorRange value) {
  switch (value) {
    case AVCOL_RANGE_MPEG:
      return ColorRange::limited;
    case AVCOL_RANGE_JPEG:
      return ColorRange::full;
    default:
      return ColorRange::unspecified;
  }
}

ColorPrimaries map_color_primaries(AVColorPrimaries value) {
  switch (value) {
    case AVCOL_PRI_BT709:
      return ColorPrimaries::bt709;
    case AVCOL_PRI_BT2020:
      return ColorPrimaries::bt2020;
    default:
      return ColorPrimaries::unspecified;
  }
}

TransferCharacteristics map_transfer(AVColorTransferCharacteristic value) {
  switch (value) {
    case AVCOL_TRC_LINEAR:
      return TransferCharacteristics::linear;
    case AVCOL_TRC_IEC61966_2_1:
      return TransferCharacteristics::srgb;
    case AVCOL_TRC_BT709:
      return TransferCharacteristics::bt709;
    case AVCOL_TRC_SMPTE2084:
      return TransferCharacteristics::pq;
    case AVCOL_TRC_ARIB_STD_B67:
      return TransferCharacteristics::hlg;
    default:
      return TransferCharacteristics::unspecified;
  }
}

MatrixCoefficients map_matrix(AVColorSpace value, PixelLayout layout) {
  if (layout == PixelLayout::rgb24 || layout == PixelLayout::rgba32) {
    return MatrixCoefficients::identity;
  }
  switch (value) {
    case AVCOL_SPC_RGB:
      return MatrixCoefficients::identity;
    case AVCOL_SPC_BT709:
      return MatrixCoefficients::bt709;
    case AVCOL_SPC_BT2020_NCL:
      return MatrixCoefficients::bt2020_ncl;
    default:
      return MatrixCoefficients::unspecified;
  }
}

std::pair<std::uint32_t, std::uint32_t> bounded_rational_or(
    AVRational value, std::uint32_t fallback_numerator,
    std::uint32_t fallback_denominator) {
  if (value.num <= 0 || value.den <= 0 ||
      static_cast<std::uint64_t>(value.num) >
          std::numeric_limits<std::uint32_t>::max() ||
      static_cast<std::uint64_t>(value.den) >
          std::numeric_limits<std::uint32_t>::max()) {
    return {fallback_numerator, fallback_denominator};
  }
  return {static_cast<std::uint32_t>(value.num),
          static_cast<std::uint32_t>(value.den)};
}

Result<std::size_t> canonical_byte_count(std::uint32_t width,
                                         std::uint32_t height,
                                         PixelLayout layout) {
  const VideoDecodeLimits limits{};
  if (width == 0U || height == 0U || width > limits.maximum_width ||
      height > limits.maximum_height) {
    return fail<std::size_t>(ErrorCode::resource_exhausted,
                             "decoded video dimensions exceed H.1 limits");
  }
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  if (pixels > limits.maximum_pixels) {
    return fail<std::size_t>(ErrorCode::resource_exhausted,
                             "decoded video pixel count exceeds H.1 limits");
  }
  if (layout == PixelLayout::yuv420p8 &&
      (((width & 1U) != 0U) || ((height & 1U) != 0U))) {
    return fail<std::size_t>(ErrorCode::decode,
                             "YUV420P8 decode requires even dimensions");
  }

  std::uint64_t bytes = pixels;
  switch (layout) {
    case PixelLayout::gray8:
      break;
    case PixelLayout::rgb24:
      if (pixels > std::numeric_limits<std::uint64_t>::max() / 3U) {
        return fail<std::size_t>(ErrorCode::resource_exhausted,
                                 "RGB24 decoded frame size overflows");
      }
      bytes = pixels * 3U;
      break;
    case PixelLayout::rgba32:
      if (pixels > std::numeric_limits<std::uint64_t>::max() / 4U) {
        return fail<std::size_t>(ErrorCode::resource_exhausted,
                                 "RGBA32 decoded frame size overflows");
      }
      bytes = pixels * 4U;
      break;
    case PixelLayout::yuv420p8:
      bytes = pixels + pixels / 2U;
      break;
  }
  if (bytes > limits.maximum_payload_bytes ||
      bytes > std::numeric_limits<std::size_t>::max()) {
    return fail<std::size_t>(ErrorCode::resource_exhausted,
                             "decoded frame exceeds H.1 payload limits");
  }
  return static_cast<std::size_t>(bytes);
}

AVRational usable_frame_rate(const AVStream& stream) {
  if (stream.avg_frame_rate.num > 0 && stream.avg_frame_rate.den > 0) {
    return stream.avg_frame_rate;
  }
  if (stream.r_frame_rate.num > 0 && stream.r_frame_rate.den > 0) {
    return stream.r_frame_rate;
  }
  return AVRational{0, 1};
}

VideoProfileDescriptor descriptor_for_frame(const AVFrame& frame,
                                             const AVStream& stream,
                                             PixelLayout layout) {
  AVRational sar = frame.sample_aspect_ratio;
  if (sar.num <= 0 || sar.den <= 0) sar = stream.sample_aspect_ratio;
  const auto [sar_num, sar_den] = bounded_rational_or(sar, 1U, 1U);
  const auto rate = usable_frame_rate(stream);
  const auto [rate_num, rate_den] = bounded_rational_or(rate, 0U, 1U);
  return VideoProfileDescriptor{
      .coded_width = static_cast<std::uint32_t>(frame.width),
      .coded_height = static_cast<std::uint32_t>(frame.height),
      .pixel_layout = layout,
      .sample_aspect_ratio_numerator = sar_num,
      .sample_aspect_ratio_denominator = sar_den,
      .nominal_frame_rate_numerator = rate_num,
      .nominal_frame_rate_denominator = rate_den,
      .color_range = map_color_range(frame.color_range),
      .color_primaries = map_color_primaries(frame.color_primaries),
      .transfer = map_transfer(frame.color_trc),
      .matrix = map_matrix(frame.colorspace, layout),
  };
}

Result<RawVideoFrameState> canonicalize_frame(const AVFrame& frame,
                                              const AVStream& stream,
                                              PixelLayout layout) {
  if (frame.width <= 0 || frame.height <= 0 ||
      static_cast<std::uint64_t>(frame.width) >
          std::numeric_limits<std::uint32_t>::max() ||
      static_cast<std::uint64_t>(frame.height) >
          std::numeric_limits<std::uint32_t>::max()) {
    return fail<RawVideoFrameState>(ErrorCode::decode,
                                    "decoded frame has invalid dimensions");
  }
  const auto width = static_cast<std::uint32_t>(frame.width);
  const auto height = static_cast<std::uint32_t>(frame.height);
  auto bytes = canonical_byte_count(width, height, layout);
  if (!bytes) return bytes.error();

  const auto destination_format = target_pixel_format(layout);
  const auto source_format = static_cast<AVPixelFormat>(frame.format);
  if (destination_format == AV_PIX_FMT_NONE || source_format == AV_PIX_FMT_NONE) {
    return fail<RawVideoFrameState>(ErrorCode::decode,
                                    "decoded frame has an unsupported pixel format");
  }

  SwsPtr scaler{sws_getContext(
      frame.width, frame.height, source_format, frame.width, frame.height,
      destination_format, SWS_BILINEAR, nullptr, nullptr, nullptr)};
  if (!scaler) {
    return fail<RawVideoFrameState>(ErrorCode::decode,
                                    "FFmpeg cannot create the pixel conversion context");
  }

  struct AlignedImage {
    std::array<std::uint8_t*, 4> data{};
    std::array<int, 4> lines{};

    ~AlignedImage() {
      if (data[0] != nullptr) av_freep(&data[0]);
    }
  };

  AlignedImage destination;
  constexpr int destination_alignment = 32;
  const auto allocated = av_image_alloc(
      destination.data.data(), destination.lines.data(), frame.width,
      frame.height, destination_format, destination_alignment);
  if (allocated < 0) {
    return ffmpeg_decode_error("FFmpeg cannot allocate the aligned frame",
                               allocated);
  }

  const auto scaled = sws_scale(
      scaler.get(), frame.data, frame.linesize, 0, frame.height,
      destination.data.data(), destination.lines.data());
  if (scaled != frame.height) {
    return fail<RawVideoFrameState>(ErrorCode::decode,
                                    "FFmpeg did not convert the complete video frame");
  }

  if (*bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return fail<RawVideoFrameState>(ErrorCode::resource_exhausted,
                                    "canonical video frame exceeds FFmpeg copy bounds");
  }
  std::vector<std::byte> pixels(*bytes);
  const std::array<const std::uint8_t*, 4> compact_source{
      destination.data[0], destination.data[1], destination.data[2],
      destination.data[3]};
  const auto copied = av_image_copy_to_buffer(
      reinterpret_cast<std::uint8_t*>(pixels.data()),
      static_cast<int>(pixels.size()), compact_source.data(),
      destination.lines.data(), destination_format, frame.width, frame.height,
      1);
  if (copied < 0) {
    return ffmpeg_decode_error("FFmpeg cannot compact the canonical frame",
                               copied);
  }
  if (static_cast<std::size_t>(copied) != pixels.size()) {
    return fail<RawVideoFrameState>(
        ErrorCode::decode,
        "FFmpeg compact frame size does not match the H.1 layout");
  }

  RawVideoFrameState state{
      .descriptor = descriptor_for_frame(frame, stream, layout),
      .pixels = std::move(pixels),
  };
  auto canonical = encode_raw_video_frame_state(state);
  if (!canonical) return canonical.error();
  return state;
}

struct DecodedCandidate {
  RawVideoFrameState state;
  std::int64_t best_effort_timestamp{AV_NOPTS_VALUE};
};

struct DecodedVideo {
  std::vector<DecodedCandidate> frames;
  AVRational time_base{0, 1};
  AVRational frame_rate{0, 1};
};

Result<DecodedVideo> decode_video_bytes(
    std::span<const std::byte> source_bytes,
    const FfmpegVideoIngestRequest& request) {
  MemoryInput input{source_bytes, 0U};
  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* avio_buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (avio_buffer == nullptr) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg input buffer");
  }
  AvioPtr avio{avio_alloc_context(avio_buffer, avio_buffer_bytes, 0, &input,
                                  &memory_read, nullptr, &memory_seek)};
  if (!avio) {
    av_free(avio_buffer);
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot create FFmpeg input context");
  }

  AVFormatContext* format_raw = avformat_alloc_context();
  if (format_raw == nullptr) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg format context");
  }
  format_raw->pb = avio.get();
  format_raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  format_raw->io_open = &deny_secondary_io_open;
  format_raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (format_raw->protocol_whitelist == nullptr) {
    avformat_free_context(format_raw);
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg protocol policy");
  }
  const auto opened = avformat_open_input(&format_raw, nullptr, nullptr, nullptr);
  if (opened < 0) {
    if (format_raw != nullptr) avformat_free_context(format_raw);
    return ffmpeg_decode_error("FFmpeg cannot open the captured media", opened);
  }
  FormatPtr format{format_raw};

  const auto stream_info = avformat_find_stream_info(format.get(), nullptr);
  if (stream_info < 0) {
    return ffmpeg_decode_error("FFmpeg cannot inspect the captured media",
                               stream_info);
  }

  const AVCodec* decoder = nullptr;
  const auto stream_index = av_find_best_stream(
      format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (stream_index < 0 || decoder == nullptr) {
    return ffmpeg_decode_error("captured media has no decodable video stream",
                               stream_index < 0 ? stream_index
                                                : AVERROR_DECODER_NOT_FOUND);
  }
  AVStream* stream = format->streams[stream_index];
  if (stream == nullptr || stream->codecpar == nullptr ||
      stream->time_base.num <= 0 || stream->time_base.den <= 0) {
    return fail<DecodedVideo>(ErrorCode::decode,
                              "selected FFmpeg video stream has invalid metadata");
  }

  CodecPtr codec{avcodec_alloc_context3(decoder)};
  if (!codec) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg decoder context");
  }
  const auto copied = avcodec_parameters_to_context(codec.get(), stream->codecpar);
  if (copied < 0) {
    return ffmpeg_decode_error("FFmpeg cannot copy video codec parameters",
                               copied);
  }
  codec->thread_count = 1;
  codec->max_pixels =
      static_cast<std::int64_t>(VideoDecodeLimits{}.maximum_pixels);
  const auto decoder_opened = avcodec_open2(codec.get(), decoder, nullptr);
  if (decoder_opened < 0) {
    return ffmpeg_decode_error("FFmpeg cannot open the selected video decoder",
                               decoder_opened);
  }

  PacketPtr packet{av_packet_alloc()};
  FramePtr frame{av_frame_alloc()};
  if (!packet || !frame) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg decode buffers");
  }

  DecodedVideo decoded;
  decoded.frames.reserve(std::min<std::size_t>(request.maximum_frames, 64U));
  decoded.time_base = stream->time_base;
  decoded.frame_rate = usable_frame_rate(*stream);
  std::uint64_t decoded_bytes = 0U;

  const auto accept_frame = [&]() -> Result<void> {
    if (decoded.frames.size() >= request.maximum_frames) {
      return fail(ErrorCode::resource_exhausted,
                  "decoded video exceeds the configured frame limit");
    }
    auto state = canonicalize_frame(*frame, *stream, request.output_layout);
    if (!state) return state.error();
    const auto frame_bytes = static_cast<std::uint64_t>(state->pixels.size());
    if (frame_bytes > request.maximum_decoded_bytes - decoded_bytes) {
      return fail(ErrorCode::resource_exhausted,
                  "decoded video exceeds the configured aggregate byte limit");
    }
    decoded_bytes += frame_bytes;
    decoded.frames.push_back(DecodedCandidate{
        .state = std::move(*state),
        .best_effort_timestamp = frame->best_effort_timestamp,
    });
    return {};
  };

  const auto receive_available = [&]() -> Result<void> {
    for (;;) {
      const auto received = avcodec_receive_frame(codec.get(), frame.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
      if (received < 0) {
        return ffmpeg_decode_error("FFmpeg video frame decode failed", received);
      }
      auto accepted = accept_frame();
      av_frame_unref(frame.get());
      if (!accepted) return accepted.error();
    }
  };

  for (;;) {
    const auto read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      return ffmpeg_decode_error("FFmpeg packet demux failed", read);
    }
    if (packet->stream_index == stream_index) {
      const auto sent = avcodec_send_packet(codec.get(), packet.get());
      av_packet_unref(packet.get());
      if (sent < 0) {
        return ffmpeg_decode_error("FFmpeg video packet decode failed", sent);
      }
      auto received = receive_available();
      if (!received) return received.error();
    } else {
      av_packet_unref(packet.get());
    }
  }

  const auto flushed = avcodec_send_packet(codec.get(), nullptr);
  if (flushed < 0 && flushed != AVERROR_EOF) {
    return ffmpeg_decode_error("FFmpeg video decoder flush failed", flushed);
  }
  auto received = receive_available();
  if (!received) return received.error();
  if (decoded.frames.empty()) {
    return fail<DecodedVideo>(ErrorCode::decode,
                              "captured media produced no decoded video frames");
  }
  return decoded;
}

Result<std::int64_t> nominal_frame_duration_ns(AVRational rate) {
  if (rate.num <= 0 || rate.den <= 0) return std::int64_t{0};
  const AVRational duration_base{rate.den, rate.num};
  const auto duration = av_rescale_q(1, duration_base, nanosecond_time_base);
  if (duration <= 0) {
    return fail<std::int64_t>(ErrorCode::decode,
                              "video frame rate cannot produce a positive duration");
  }
  return duration;
}

Result<std::vector<std::pair<std::int64_t, std::int64_t>>> map_frame_times(
    const DecodedVideo& decoded, const FfmpegVideoIngestRequest& request) {
  if (decoded.frames.empty() || decoded.time_base.num <= 0 ||
      decoded.time_base.den <= 0) {
    return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
        ErrorCode::decode, "decoded video has no usable timing metadata");
  }

  auto nominal = nominal_frame_duration_ns(decoded.frame_rate);
  if (!nominal) return nominal.error();
  const auto nominal_ns = *nominal;
  const bool synthetic_timeline =
      decoded.frames.front().best_effort_timestamp == AV_NOPTS_VALUE;
  if (synthetic_timeline && nominal_ns == 0) {
    return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
        ErrorCode::decode,
        "video frames lack timestamps and a usable nominal frame rate");
  }

  const auto base_timestamp = synthetic_timeline
                                  ? std::int64_t{0}
                                  : decoded.frames.front().best_effort_timestamp;
  std::vector<std::int64_t> starts;
  starts.reserve(decoded.frames.size());
  std::int64_t previous_relative = -1;
  for (std::size_t index = 0; index < decoded.frames.size(); ++index) {
    std::int64_t relative = 0;
    const auto timestamp = decoded.frames[index].best_effort_timestamp;
    if (synthetic_timeline || timestamp == AV_NOPTS_VALUE) {
      if (index == 0U) {
        relative = 0;
      } else {
        if (nominal_ns == 0 ||
            previous_relative >
                std::numeric_limits<std::int64_t>::max() - nominal_ns) {
          return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
              ErrorCode::decode,
              "video timestamps cannot be synthesized monotonically");
        }
        relative = previous_relative + nominal_ns;
      }
    } else {
      if (timestamp < base_timestamp) {
        return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
            ErrorCode::decode, "video frame timestamp regressed before its origin");
      }
      if (base_timestamp < 0 &&
          timestamp >
              std::numeric_limits<std::int64_t>::max() + base_timestamp) {
        return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
            ErrorCode::resource_exhausted,
            "video frame timestamp delta overflows");
      }
      const auto timestamp_delta = timestamp - base_timestamp;
      relative = av_rescale_q(timestamp_delta, decoded.time_base,
                              nanosecond_time_base);
    }
    if (relative < 0 || (index != 0U && relative <= previous_relative)) {
      return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
          ErrorCode::decode, "video frame timestamps are not strictly monotonic");
    }
    if (request.start_ns >
        std::numeric_limits<std::int64_t>::max() - relative) {
      return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
          ErrorCode::resource_exhausted, "video frame timestamp overflows");
    }
    const auto absolute = request.start_ns + relative;
    if (absolute < request.start_ns || absolute >= request.end_ns) {
      return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
          ErrorCode::decode,
          "decoded video frame falls outside the requested archive interval");
    }
    starts.push_back(absolute);
    previous_relative = relative;
  }

  std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
  intervals.reserve(starts.size());
  for (std::size_t index = 0; index < starts.size(); ++index) {
    auto end = request.end_ns;
    if (index + 1U < starts.size()) {
      end = starts[index + 1U];
    } else if (nominal_ns > 0 &&
               starts[index] <=
                   std::numeric_limits<std::int64_t>::max() - nominal_ns) {
      end = std::min(request.end_ns, starts[index] + nominal_ns);
    }
    if (end <= starts[index] || end > request.end_ns) {
      return fail<std::vector<std::pair<std::int64_t, std::int64_t>>>(
          ErrorCode::decode, "decoded video frame interval is invalid");
    }
    intervals.emplace_back(starts[index], end);
  }
  return intervals;
}

Sha256 ffmpeg_configuration_hash(PixelLayout layout) {
  std::string configuration = "ffmpeg=";
  configuration += av_version_info();
  configuration += ";layout=";
  configuration += std::to_string(static_cast<unsigned>(layout));
  return sha256(std::as_bytes(
      std::span<const char>{configuration.data(), configuration.size()}));
}

std::int64_t provenance_created_utc_ns() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

#endif

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
#ifndef CODEC_HAS_FFMPEG_VIDEO
  return fail<FfmpegVideoIngestReport>(
      ErrorCode::model_incompatible,
      "FFmpeg video ingest backend is unavailable");
#else
  auto prepared = ::codec::detail::PreparedCapture::prepare(
      request.source_uri,
      ::codec::detail::CaptureOptions{
          .chunk_bytes = request.capture_chunk_bytes,
          .maximum_bytes = request.maximum_source_bytes,
          .maximum_redirects = request.maximum_redirects,
          .deny_private_network = request.deny_private_network,
      });
  if (!prepared) return prepared.error();

  std::vector<std::byte> source_bytes;
  source_bytes.reserve(static_cast<std::size_t>(
      std::min<std::uint64_t>(request.maximum_source_bytes, 16U * 1024U * 1024U)));
  auto captured = prepared->run(
      [&source_bytes, &request](std::span<const std::byte> bytes) -> Result<void> {
        if (bytes.size() > request.maximum_source_bytes - source_bytes.size()) {
          return fail(ErrorCode::resource_exhausted,
                      "FFmpeg video ingest exceeded its source byte limit");
        }
        source_bytes.insert(source_bytes.end(), bytes.begin(), bytes.end());
        return {};
      });
  if (!captured) return captured.error();

  auto created = CodaWriter::create(request.archive_path);
  if (!created) return created.error();
  auto writer = std::move(*created);
  auto descriptor = writer.append_stream_descriptor(request.descriptor,
                                                     request.start_ns);
  if (!descriptor) return descriptor.error();
  auto source = writer.append(RecordType::source_bytes, request.descriptor.id,
                              request.start_ns, request.end_ns, source_bytes);
  if (!source) return source.error();

  FfmpegVideoIngestReport report{
      .archive_path = request.archive_path,
      .descriptor = *descriptor,
      .source = *source,
      .states = {},
      .provenance = {},
      .profile_error = std::nullopt,
  };
  const auto finish_source_only =
      [&writer, &report](Error error) -> Result<FfmpegVideoIngestReport> {
    auto finalized = writer.finalize();
    if (!finalized) return finalized.error();
    report.profile_error = std::move(error);
    return report;
  };

  auto decoded = decode_video_bytes(source_bytes, request);
  if (!decoded) return finish_source_only(decoded.error());
  auto intervals = map_frame_times(*decoded, request);
  if (!intervals) return finish_source_only(intervals.error());
  if (intervals->size() != decoded->frames.size()) {
    return finish_source_only(
        Error{ErrorCode::internal, "video frame timing count mismatch", false});
  }

  const auto configuration_hash = ffmpeg_configuration_hash(request.output_layout);
  const auto created_utc_ns = provenance_created_utc_ns();
  const std::array inputs{*source};
  report.states.reserve(decoded->frames.size());
  report.provenance.reserve(decoded->frames.size());
  for (std::size_t index = 0; index < decoded->frames.size(); ++index) {
    auto encoded = encode_raw_video_frame_state(decoded->frames[index].state);
    if (!encoded) return finish_source_only(encoded.error());
    const auto [frame_start, frame_end] = (*intervals)[index];
    auto state_record = writer.append_raw(
        raw_video_frame_state_record_type, request.descriptor.id, frame_start,
        frame_end, *encoded);
    if (!state_record) return state_record.error();

    const ProvenanceProcess process{
        .operation = "codec.video.raw-frame.canonicalize",
        .implementation_id = "codec.video",
        .implementation_version = "1",
        .implementation_hash = std::nullopt,
        .configuration_hash = configuration_hash,
        .created_utc_ns = created_utc_ns,
        .details_type = "application/vnd.codec.video.canonicalization.v1",
        .details = {std::byte{0x01}},
    };
    auto provenance = writer.append_stream_provenance(
        *state_record, TruthClass::state_exact, inputs, process);
    if (!provenance) return provenance.error();
    report.states.push_back(*state_record);
    report.provenance.push_back(*provenance);
  }

  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  return report;
#endif
}

}  // namespace codec::profiles::video