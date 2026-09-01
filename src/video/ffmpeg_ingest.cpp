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
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/rational.h>
#include <libavutil/version.h>
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
constexpr std::size_t maximum_encoded_video_packets = 1'000'000U;

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
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = static_cast<std::int64_t>(input.offset); break;
    case SEEK_END: base = static_cast<std::int64_t>(input.bytes.size()); break;
    default: return AVERROR(EINVAL);
  }

  if ((offset > 0 && base > std::numeric_limits<std::int64_t>::max() - offset) ||
      (offset < 0 && base < std::numeric_limits<std::int64_t>::min() - offset)) {
    return AVERROR(EOVERFLOW);
  }
  const auto target = base + offset;
  if (target < 0 || static_cast<std::uint64_t>(target) > input.bytes.size()) {
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
    if (context != nullptr) avformat_close_input(&context);
  }
};

struct CodecDeleter {
  void operator()(AVCodecContext* context) const noexcept {
    if (context != nullptr) avcodec_free_context(&context);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* packet) const noexcept {
    if (packet != nullptr) av_packet_free(&packet);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* frame) const noexcept {
    if (frame != nullptr) av_frame_free(&frame);
  }
};

using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;
using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

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
    case PixelLayout::gray8: break;
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
    case PixelLayout::yuv420p8: bytes = pixels + pixels / 2U; break;
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

bool starts_with_annex_b(std::span<const std::byte> payload) noexcept {
  return (payload.size() >= 3U && payload[0] == std::byte{0x00} &&
          payload[1] == std::byte{0x00} && payload[2] == std::byte{0x01}) ||
         (payload.size() >= 4U && payload[0] == std::byte{0x00} &&
          payload[1] == std::byte{0x00} && payload[2] == std::byte{0x00} &&
          payload[3] == std::byte{0x01});
}

Result<std::int64_t> subtract_timestamp(std::int64_t left,
                                        std::int64_t right,
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

struct EncodedVideoPacketCandidate {
  std::int64_t pts_ns{};
  std::int64_t dts_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
};

struct DecodedVideo {
  EncodedVideoState state;
  std::vector<EncodedVideoPacketCandidate> packets;
  AVRational time_base{0, 1};
  AVRational frame_rate{0, 1};
  std::uint64_t virtual_decoded_bytes{};
  std::uint64_t captured_packet_bytes{};
};

Result<void> initialize_encoded_video(DecodedVideo& decoded,
                                      const AVStream& stream,
                                      const FfmpegVideoIngestRequest& request) {
  (void)request;
  if (stream.codecpar == nullptr || stream.time_base.num <= 0 ||
      stream.time_base.den <= 0) {
    return fail(ErrorCode::decode,
                "selected FFmpeg video stream has invalid metadata");
  }
  if (stream.codecpar->codec_id != AV_CODEC_ID_H264) {
    return fail(ErrorCode::model_incompatible,
                "H.1 encoded video v1 supports H.264 only");
  }
  if (stream.codecpar->extradata_size < 0 ||
      (stream.codecpar->extradata_size > 0 &&
       stream.codecpar->extradata == nullptr)) {
    return fail(ErrorCode::decode,
                "selected H.264 stream has invalid decoder configuration");
  }
  const auto config_bytes =
      static_cast<std::uint64_t>(stream.codecpar->extradata_size);
  if (config_bytes > EncodedVideoDecodeLimits{}.maximum_decoder_config_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "H.264 decoder configuration exceeds configured limits");
  }

  decoded.time_base = stream.time_base;
  decoded.frame_rate = usable_frame_rate(stream);
  decoded.state.codec = EncodedVideoCodec::h264;
  decoded.state.codec_profile = stream.codecpar->profile;
  decoded.state.codec_level = stream.codecpar->level;
  if (stream.codecpar->width > 0 && stream.codecpar->height > 0 &&
      static_cast<std::uint64_t>(stream.codecpar->width) <=
          std::numeric_limits<std::uint32_t>::max() &&
      static_cast<std::uint64_t>(stream.codecpar->height) <=
          std::numeric_limits<std::uint32_t>::max()) {
    decoded.state.coded_width =
        static_cast<std::uint32_t>(stream.codecpar->width);
    decoded.state.coded_height =
        static_cast<std::uint32_t>(stream.codecpar->height);
  }
  const auto [sar_num, sar_den] = bounded_rational_or(
      stream.codecpar->sample_aspect_ratio, 1U, 1U);
  decoded.state.sample_aspect_ratio_numerator = sar_num;
  decoded.state.sample_aspect_ratio_denominator = sar_den;
  if (stream.codecpar->extradata_size > 0) {
    const auto* config =
        reinterpret_cast<const std::byte*>(stream.codecpar->extradata);
    decoded.state.decoder_config.assign(
        config, config + stream.codecpar->extradata_size);
    const auto config_view = std::span<const std::byte>{
        decoded.state.decoder_config.data(), decoded.state.decoder_config.size()};
    if (starts_with_annex_b(config_view)) {
      decoded.state.framing = EncodedVideoPacketFraming::annex_b;
    } else if (decoded.state.decoder_config.front() == std::byte{0x01}) {
      decoded.state.framing = EncodedVideoPacketFraming::length_prefixed;
    }
  }
  decoded.captured_packet_bytes = config_bytes;
  return {};
}

Result<void> capture_encoded_video_packet(
    DecodedVideo& decoded, const AVPacket& packet,
    const FfmpegVideoIngestRequest& request) {
  (void)request;
  if (packet.data == nullptr || packet.size <= 0) {
    return fail(ErrorCode::decode,
                "selected H.264 packet has no encoded payload");
  }
  auto pts = packet.pts;
  auto dts = packet.dts;
  if (pts == AV_NOPTS_VALUE) pts = dts;
  if (dts == AV_NOPTS_VALUE) dts = pts;
  if (pts == AV_NOPTS_VALUE || dts == AV_NOPTS_VALUE) {
    return fail(ErrorCode::decode,
                "selected H.264 packet lacks usable timestamps");
  }
  if (packet.side_data_elems < 0 ||
      (packet.side_data_elems > 0 && packet.side_data == nullptr)) {
    return fail(ErrorCode::decode,
                "selected H.264 packet has invalid side data");
  }
  for (std::size_t index = 0U;
       index < static_cast<std::size_t>(packet.side_data_elems); ++index) {
    if (packet.side_data[index].type == AV_PKT_DATA_MPEGTS_STREAM_ID) continue;
    return fail(ErrorCode::model_incompatible,
                "selected H.264 packet has side data unsupported by EVP1 v1");
  }
  if (decoded.packets.size() >= maximum_encoded_video_packets) {
    return fail(ErrorCode::resource_exhausted,
                "encoded video exceeds the configured packet count limit");
  }
  const auto payload_bytes = static_cast<std::uint64_t>(packet.size);
  const auto encoded_limits = EncodedVideoDecodeLimits{};
  if (payload_bytes > encoded_limits.maximum_packet_bytes ||
      decoded.captured_packet_bytes > encoded_limits.maximum_payload_bytes ||
      payload_bytes >
          encoded_limits.maximum_payload_bytes - decoded.captured_packet_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "encoded video exceeds the configured aggregate byte limit");
  }

  const auto pts_ns = av_rescale_q(pts, decoded.time_base,
                                   nanosecond_time_base);
  const auto dts_ns = av_rescale_q(dts, decoded.time_base,
                                   nanosecond_time_base);
  std::uint64_t duration_ns = 0U;
  if (packet.duration > 0) {
    const auto scaled = av_rescale_q(packet.duration, decoded.time_base,
                                     nanosecond_time_base);
    if (scaled > 0) duration_ns = static_cast<std::uint64_t>(scaled);
  }

  std::vector<std::byte> payload(static_cast<std::size_t>(packet.size));
  std::memcpy(payload.data(), packet.data, static_cast<std::size_t>(packet.size));
  if (decoded.state.framing == EncodedVideoPacketFraming::unknown &&
      starts_with_annex_b(payload)) {
    decoded.state.framing = EncodedVideoPacketFraming::annex_b;
  }
  decoded.packets.push_back(EncodedVideoPacketCandidate{
      .pts_ns = pts_ns,
      .dts_ns = dts_ns,
      .duration_ns = duration_ns,
      .flags = static_cast<std::uint32_t>(packet.flags) & 0x1fU,
      .payload = std::move(payload),
  });
  decoded.captured_packet_bytes += payload_bytes;
  return {};
}

Result<void> validate_decoded_video_frame(
    DecodedVideo& decoded, const AVFrame& frame, const AVStream& stream,
    const FfmpegVideoIngestRequest& request) {
  if (decoded.state.validated_frames >= request.maximum_frames) {
    return fail(ErrorCode::resource_exhausted,
                "decoded video exceeds the configured frame limit");
  }
  if (frame.width <= 0 || frame.height <= 0 ||
      static_cast<std::uint64_t>(frame.width) >
          std::numeric_limits<std::uint32_t>::max() ||
      static_cast<std::uint64_t>(frame.height) >
          std::numeric_limits<std::uint32_t>::max()) {
    return fail(ErrorCode::decode, "decoded frame has invalid dimensions");
  }
  const auto width = static_cast<std::uint32_t>(frame.width);
  const auto height = static_cast<std::uint32_t>(frame.height);
  auto bytes = canonical_byte_count(width, height, request.output_layout);
  if (!bytes) return bytes.error();
  const auto frame_bytes = static_cast<std::uint64_t>(*bytes);
  if (decoded.virtual_decoded_bytes > request.maximum_decoded_bytes ||
      frame_bytes > request.maximum_decoded_bytes -
                        decoded.virtual_decoded_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "decoded video exceeds the configured aggregate byte limit");
  }
  decoded.virtual_decoded_bytes += frame_bytes;

  if (decoded.state.coded_width == 0U || decoded.state.coded_height == 0U) {
    decoded.state.coded_width = width;
    decoded.state.coded_height = height;
  } else if (decoded.state.coded_width != width ||
             decoded.state.coded_height != height) {
    return fail(ErrorCode::model_incompatible,
                "H.264 coded dimensions changed within the captured stream");
  }
  if (decoded.state.validated_frames == 0U) {
    AVRational sar = frame.sample_aspect_ratio;
    if (sar.num <= 0 || sar.den <= 0) sar = stream.sample_aspect_ratio;
    const auto [sar_num, sar_den] = bounded_rational_or(sar, 1U, 1U);
    decoded.state.sample_aspect_ratio_numerator = sar_num;
    decoded.state.sample_aspect_ratio_denominator = sar_den;
  }
  ++decoded.state.validated_frames;
  return {};
}

Result<void> finalize_encoded_video(DecodedVideo& decoded,
                                    const FfmpegVideoIngestRequest& request) {
  if (decoded.state.validated_frames == 0U || decoded.packets.empty()) {
    return fail(ErrorCode::decode,
                "captured media produced no validated encoded video");
  }
  const auto duration_ns = positive_distance(request.end_ns, request.start_ns);
  if (duration_ns == 0U ||
      duration_ns > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
    return fail(ErrorCode::resource_exhausted,
                "video archive interval exceeds encoded timing bounds");
  }

  auto presentation_origin = decoded.packets.front().pts_ns;
  for (const auto& packet : decoded.packets) {
    presentation_origin = std::min(presentation_origin, packet.pts_ns);
  }
  if (presentation_origin > std::numeric_limits<std::int64_t>::max() -
                                static_cast<std::int64_t>(duration_ns)) {
    return fail(ErrorCode::resource_exhausted,
                "encoded video presentation window overflows");
  }
  const auto presentation_end =
      presentation_origin + static_cast<std::int64_t>(duration_ns);

  std::vector<std::size_t> retained;
  retained.reserve(decoded.packets.size());
  for (std::size_t index = 0U; index < decoded.packets.size(); ++index) {
    if (decoded.packets[index].pts_ns < presentation_end) retained.push_back(index);
  }
  if (retained.empty()) {
    return fail(ErrorCode::decode,
                "encoded video has no packets in the requested interval");
  }

  const auto first_dts = decoded.packets[retained.front()].dts_ns;
  std::int64_t previous_dts = first_dts;
  auto nominal = nominal_frame_duration_ns(decoded.frame_rate);
  if (!nominal) return nominal.error();
  const auto nominal_ns = *nominal;

  decoded.state.packets.clear();
  decoded.state.packets.reserve(retained.size());
  for (const auto source_index : retained) {
    auto& source = decoded.packets[source_index];
    if (source.dts_ns < previous_dts) {
      return fail(ErrorCode::decode,
                  "encoded H.264 packet DTS is not monotonic");
    }
    auto pts_offset = subtract_timestamp(source.pts_ns, first_dts,
                                         "encoded H.264 packet PTS offset");
    auto dts_offset = subtract_timestamp(source.dts_ns, first_dts,
                                         "encoded H.264 packet DTS offset");
    if (!pts_offset || !dts_offset || *dts_offset < 0) {
      return !pts_offset ? pts_offset.error()
                         : (!dts_offset ? dts_offset.error()
                                        : Error{ErrorCode::decode,
                                                "encoded H.264 DTS precedes its origin",
                                                false});
    }

    auto packet_duration = source.duration_ns;
    if (packet_duration == 0U && source_index + 1U < decoded.packets.size()) {
      const auto next_dts = decoded.packets[source_index + 1U].dts_ns;
      if (next_dts > source.dts_ns) {
        packet_duration = positive_distance(next_dts, source.dts_ns);
      }
    }
    if (packet_duration == 0U && nominal_ns > 0) {
      packet_duration = static_cast<std::uint64_t>(nominal_ns);
    }
    if (packet_duration == 0U ||
        packet_duration > static_cast<std::uint64_t>(
                              std::numeric_limits<std::int64_t>::max())) {
      return fail(ErrorCode::decode,
                  "encoded H.264 packet has no usable duration");
    }

    decoded.state.packets.push_back(EncodedVideoPacket{
        .pts_offset_ns = *pts_offset,
        .dts_offset_ns = *dts_offset,
        .duration_ns = packet_duration,
        .flags = source.flags,
        .payload = std::move(source.payload),
    });
    previous_dts = source.dts_ns;
  }

  if (presentation_origin >= first_dts) {
    decoded.state.presentation_lead_ns =
        positive_distance(presentation_origin, first_dts);
  } else {
    decoded.state.presentation_lead_ns = 0U;
  }
  if (decoded.state.presentation_lead_ns >= duration_ns) {
    return fail(ErrorCode::decode,
                "encoded H.264 presentation lead exceeds the requested interval");
  }
  return {};
}

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
  if (stream == nullptr) {
    return fail<DecodedVideo>(ErrorCode::decode,
                              "selected FFmpeg video stream is missing");
  }

  DecodedVideo decoded;
  auto initialized = initialize_encoded_video(decoded, *stream, request);
  if (!initialized) return initialized.error();

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

  decoded.packets.reserve(std::min<std::size_t>(request.maximum_frames, 64U));
  const auto receive_available = [&]() -> Result<void> {
    for (;;) {
      const auto received = avcodec_receive_frame(codec.get(), frame.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
      if (received < 0) {
        return ffmpeg_decode_error("FFmpeg video frame decode failed", received);
      }
      auto accepted =
          validate_decoded_video_frame(decoded, *frame, *stream, request);
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
      auto captured_packet =
          capture_encoded_video_packet(decoded, *packet, request);
      if (!captured_packet) {
        av_packet_unref(packet.get());
        return captured_packet.error();
      }
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
  auto finalized = finalize_encoded_video(decoded, request);
  if (!finalized) return finalized.error();
  return decoded;
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
      std::min<std::uint64_t>(request.maximum_source_bytes,
                              16U * 1024U * 1024U)));
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
  auto encoded = encode_encoded_video_state(decoded->state);
  if (!encoded) return finish_source_only(encoded.error());
  auto state_record = writer.append_raw(
      video_encoded_video_state_record_type, request.descriptor.id,
      request.start_ns, request.end_ns, *encoded);
  if (!state_record) return state_record.error();

  const ProvenanceProcess process{
      .operation = "codec.video.encoded-video.preserve",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = provenance_created_utc_ns(),
      .details_type = "application/vnd.codec.video.encoded-video.v1",
      .details = {std::byte{0x01}},
  };
  const std::array inputs{*source};
  auto provenance = writer.append_stream_provenance(
      *state_record, TruthClass::state_exact, inputs, process);
  if (!provenance) return provenance.error();
  report.states.push_back(*state_record);
  report.provenance.push_back(*provenance);

  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  return report;
#endif
}

}  // namespace codec::profiles::video
