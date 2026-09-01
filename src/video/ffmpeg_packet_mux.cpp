#include "ffmpeg_packet_mux.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef CODEC_HAS_FFMPEG_VIDEO
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
}
#endif

namespace codec::profiles::video {
namespace {

#ifdef CODEC_HAS_FFMPEG_VIDEO

constexpr AVRational kNanoseconds{1, 1'000'000'000};
constexpr AVRational kMp4VideoTimeBase{1, 1'000'000};

std::string ffmpeg_error_text(int code) {
  std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
  if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
    return "unknown FFmpeg error";
  }
  return std::string{buffer.data()};
}

template <typename T>
Result<T> ffmpeg_failure(ErrorCode code, std::string message, int error) {
  message += ": ";
  message += ffmpeg_error_text(error);
  return fail<T>(code, std::move(message));
}

struct OutputFormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    if (value != nullptr) avformat_free_context(value);
  }
};

struct InputFormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    if (value != nullptr) avformat_close_input(&value);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept {
    if (value != nullptr) av_packet_free(&value);
  }
};

struct BsfDeleter {
  void operator()(AVBSFContext* value) const noexcept {
    if (value != nullptr) av_bsf_free(&value);
  }
};

struct AvioDeleter {
  void operator()(AVIOContext* value) const noexcept {
    if (value == nullptr) return;
    av_freep(&value->buffer);
    avio_context_free(&value);
  }
};

using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using BsfPtr = std::unique_ptr<AVBSFContext, BsfDeleter>;
using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;

class DynamicOutput final {
 public:
  explicit DynamicOutput(AVFormatContext* format) : format_(format) {}
  DynamicOutput(const DynamicOutput&) = delete;
  DynamicOutput& operator=(const DynamicOutput&) = delete;

  ~DynamicOutput() {
    if (!active_ || format_ == nullptr || format_->pb == nullptr) return;
    std::uint8_t* buffer = nullptr;
    (void)avio_close_dyn_buf(format_->pb, &buffer);
    format_->pb = nullptr;
    av_free(buffer);
  }

  Result<void> open() {
    const auto opened = avio_open_dyn_buf(&format_->pb);
    if (opened < 0) {
      return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                  "cannot allocate MP4 output buffer", opened);
    }
    active_ = true;
    return {};
  }

  Result<void> require_within(std::uint64_t maximum_bytes) const {
    const auto position = avio_tell(format_->pb);
    if (position < 0) {
      return fail(ErrorCode::internal,
                  "cannot determine current MP4 output size");
    }
    if (static_cast<std::uint64_t>(position) > maximum_bytes) {
      return fail(ErrorCode::resource_exhausted,
                  "encoded packet MP4 export exceeds the configured output limit");
    }
    return {};
  }

  Result<std::vector<std::byte>> close(std::uint64_t maximum_bytes) {
    std::uint8_t* buffer = nullptr;
    const auto size = avio_close_dyn_buf(format_->pb, &buffer);
    format_->pb = nullptr;
    active_ = false;
    if (size < 0) {
      av_free(buffer);
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::internal, "cannot finalize MP4 output buffer", size);
    }
    if (static_cast<std::uint64_t>(size) > maximum_bytes) {
      av_free(buffer);
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "encoded packet MP4 export exceeds the configured output limit");
    }
    std::vector<std::byte> output(static_cast<std::size_t>(size));
    if (size > 0) {
      std::memcpy(output.data(), buffer, static_cast<std::size_t>(size));
    }
    av_free(buffer);
    return output;
  }

 private:
  AVFormatContext* format_{};
  bool active_{};
};

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
    if (input.bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      return AVERROR(EOVERFLOW);
    }
    return static_cast<std::int64_t>(input.bytes.size());
  }
  const int origin = whence & ~AVSEEK_FORCE;
  std::int64_t base = 0;
  switch (origin) {
    case SEEK_SET: break;
    case SEEK_CUR:
      base = static_cast<std::int64_t>(input.offset);
      break;
    case SEEK_END:
      if (input.bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        return AVERROR(EOVERFLOW);
      }
      base = static_cast<std::int64_t>(input.bytes.size());
      break;
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

bool starts_with_annex_b(std::span<const std::byte> bytes) noexcept {
  return (bytes.size() >= 3U && bytes[0] == std::byte{0x00} &&
          bytes[1] == std::byte{0x00} && bytes[2] == std::byte{0x01}) ||
         (bytes.size() >= 4U && bytes[0] == std::byte{0x00} &&
          bytes[1] == std::byte{0x00} && bytes[2] == std::byte{0x00} &&
          bytes[3] == std::byte{0x01});
}

bool looks_like_avcc(std::span<const std::byte> bytes) noexcept {
  return bytes.size() >= 7U && bytes.front() == std::byte{0x01};
}

Result<void> replace_extradata(AVCodecParameters& parameters,
                               std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return fail(ErrorCode::model_incompatible,
                "MP4 packet passthrough requires codec configuration");
  }
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return fail(ErrorCode::resource_exhausted,
                "codec configuration exceeds FFmpeg bounds");
  }
  av_freep(&parameters.extradata);
  parameters.extradata_size = 0;
  parameters.extradata = static_cast<std::uint8_t*>(
      av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (parameters.extradata == nullptr) {
    return fail(ErrorCode::resource_exhausted,
                "cannot allocate MP4 codec configuration");
  }
  std::memcpy(parameters.extradata, bytes.data(), bytes.size());
  parameters.extradata_size = static_cast<int>(bytes.size());
  return {};
}

Result<BsfPtr> make_bsf(const char* name, AVMediaType media_type,
                        AVCodecID codec_id, AVRational time_base,
                        std::int32_t profile, std::int32_t level,
                        std::uint32_t width, std::uint32_t height,
                        std::uint32_t sample_rate,
                        std::uint32_t channels) {
  const AVBitStreamFilter* filter = av_bsf_get_by_name(name);
  if (filter == nullptr) {
    return fail<BsfPtr>(ErrorCode::model_incompatible,
                        std::string{"FFmpeg bitstream filter is unavailable: "} +
                            name);
  }
  AVBSFContext* raw = nullptr;
  const auto allocated = av_bsf_alloc(filter, &raw);
  if (allocated < 0 || raw == nullptr) {
    return ffmpeg_failure<BsfPtr>(
        ErrorCode::resource_exhausted,
        std::string{"cannot allocate FFmpeg bitstream filter: "} + name,
        allocated < 0 ? allocated : AVERROR(ENOMEM));
  }
  BsfPtr context{raw};
  context->par_in->codec_type = media_type;
  context->par_in->codec_id = codec_id;
  context->par_in->codec_tag = 0;
  context->par_in->profile = profile;
  context->par_in->level = level;
  if (media_type == AVMEDIA_TYPE_VIDEO) {
    context->par_in->width = static_cast<int>(width);
    context->par_in->height = static_cast<int>(height);
  } else {
    context->par_in->sample_rate = static_cast<int>(sample_rate);
    context->par_in->frame_size = 1024;
    av_channel_layout_default(&context->par_in->ch_layout,
                              static_cast<int>(channels));
  }
  context->time_base_in = time_base;
  const auto initialized = av_bsf_init(context.get());
  if (initialized < 0) {
    return ffmpeg_failure<BsfPtr>(
        ErrorCode::model_incompatible,
        std::string{"cannot initialize FFmpeg bitstream filter: "} + name,
        initialized);
  }
  return context;
}

Result<PacketPtr> packet_from_bytes(std::span<const std::byte> payload,
                                    std::int64_t pts, std::int64_t dts,
                                    std::uint64_t duration,
                                    std::uint32_t flags) {
  if (payload.empty() ||
      payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      duration == 0U ||
      duration > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
    return fail<PacketPtr>(ErrorCode::archive_corrupt,
                           "verified encoded packet geometry is invalid");
  }
  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail<PacketPtr>(ErrorCode::resource_exhausted,
                           "cannot allocate FFmpeg packet");
  }
  const auto allocated =
      av_new_packet(packet.get(), static_cast<int>(payload.size()));
  if (allocated < 0) {
    return ffmpeg_failure<PacketPtr>(ErrorCode::resource_exhausted,
                                     "cannot allocate encoded packet payload",
                                     allocated);
  }
  std::memcpy(packet->data, payload.data(), payload.size());
  packet->pts = pts;
  packet->dts = dts;
  packet->duration = static_cast<std::int64_t>(duration);
  packet->flags = static_cast<int>(flags);
  packet->pos = -1;
  return packet;
}

Result<std::vector<std::byte>> recover_h264_extradata(
    const EncodedVideoState& state) {
  auto filter = make_bsf("extract_extradata", AVMEDIA_TYPE_VIDEO,
                         AV_CODEC_ID_H264, kNanoseconds, state.codec_profile,
                         state.codec_level, state.coded_width,
                         state.coded_height, 0U, 0U);
  if (!filter) return filter.error();

  PacketPtr filtered{av_packet_alloc()};
  if (!filtered) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot allocate H.264 extradata recovery packet");
  }
  for (const auto& encoded : state.packets) {
    auto packet = packet_from_bytes(encoded.payload, encoded.pts_offset_ns,
                                    encoded.dts_offset_ns,
                                    encoded.duration_ns, encoded.flags);
    if (!packet) return packet.error();
    const auto sent = av_bsf_send_packet(filter->get(), packet->get());
    if (sent < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::model_incompatible,
          "H.264 parameter-set extraction rejected a stored packet", sent);
    }
    for (;;) {
      const auto received = av_bsf_receive_packet(filter->get(), filtered.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
      if (received < 0) {
        return ffmpeg_failure<std::vector<std::byte>>(
            ErrorCode::model_incompatible,
            "H.264 parameter-set extraction failed", received);
      }
      std::size_t config_size = 0U;
      const auto* config = av_packet_get_side_data(
          filtered.get(), AV_PKT_DATA_NEW_EXTRADATA, &config_size);
      if (config != nullptr && config_size != 0U) {
        const auto* first = reinterpret_cast<const std::byte*>(config);
        std::vector<std::byte> output(first, first + config_size);
        av_packet_unref(filtered.get());
        return output;
      }
      av_packet_unref(filtered.get());
    }
  }
  return fail<std::vector<std::byte>>(
      ErrorCode::model_incompatible,
      "H.264 Annex-B packets do not expose recoverable SPS/PPS configuration");
}

Result<std::vector<std::byte>> h264_configuration_for_mp4(
    const EncodedVideoState& state) {
  const auto config = std::span<const std::byte>{state.decoder_config.data(),
                                                 state.decoder_config.size()};
  switch (state.framing) {
    case EncodedVideoPacketFraming::length_prefixed:
      if (!looks_like_avcc(config)) {
        return fail<std::vector<std::byte>>(
            ErrorCode::model_incompatible,
            "length-prefixed H.264 lacks a valid AVCDecoderConfigurationRecord");
      }
      return state.decoder_config;
    case EncodedVideoPacketFraming::annex_b:
      if (config.empty()) return recover_h264_extradata(state);
      if (!starts_with_annex_b(config) && !looks_like_avcc(config)) {
        return fail<std::vector<std::byte>>(
            ErrorCode::model_incompatible,
            "Annex-B H.264 has an unrecognized decoder configuration");
      }
      return state.decoder_config;
    case EncodedVideoPacketFraming::unknown:
      return fail<std::vector<std::byte>>(
          ErrorCode::model_incompatible,
          "H.264 packet framing is unknown and cannot be remuxed safely");
  }
  return fail<std::vector<std::byte>>(ErrorCode::model_incompatible,
                                      "H.264 packet framing is unsupported");
}

Result<void> validate_encoded_video_for_mux(
    const VerifiedVideoEncodedVideo& video) {
  const auto& state = video.state;
  if (state.codec != EncodedVideoCodec::h264 || state.coded_width == 0U ||
      state.coded_height == 0U || state.validated_frames == 0U ||
      state.packets.empty() ||
      state.coded_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      state.coded_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video is invalid before MP4 mux");
  }
  if (state.sample_aspect_ratio_numerator == 0U ||
      state.sample_aspect_ratio_denominator == 0U ||
      state.sample_aspect_ratio_numerator >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      state.sample_aspect_ratio_denominator >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video sample aspect ratio is invalid");
  }
  return {};
}

Result<InputFormatPtr> open_memory_mp4(std::span<const std::byte> bytes,
                                       MemoryInput& memory,
                                       AvioPtr& avio) {
  if (bytes.empty()) {
    return fail<InputFormatPtr>(ErrorCode::archive_corrupt,
                                "generated video MP4 is empty");
  }
  memory = MemoryInput{bytes, 0U};
  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (buffer == nullptr) {
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 input buffer");
  }
  avio.reset(avio_alloc_context(buffer, avio_buffer_bytes, 0, &memory,
                                &memory_read, nullptr, &memory_seek));
  if (!avio) {
    av_free(buffer);
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot create MP4 input context");
  }
  AVFormatContext* raw = avformat_alloc_context();
  if (raw == nullptr) {
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 input format context");
  }
  raw->pb = avio.get();
  raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (raw->protocol_whitelist == nullptr) {
    avformat_free_context(raw);
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 input protocol policy");
  }
  const auto opened = avformat_open_input(&raw, nullptr, nullptr, nullptr);
  if (opened < 0) {
    if (raw != nullptr) avformat_free_context(raw);
    return ffmpeg_failure<InputFormatPtr>(ErrorCode::decode,
                                          "cannot reopen generated video MP4",
                                          opened);
  }
  InputFormatPtr input{raw};
  const auto info = avformat_find_stream_info(input.get(), nullptr);
  if (info < 0) {
    return ffmpeg_failure<InputFormatPtr>(ErrorCode::decode,
                                          "cannot inspect generated video MP4",
                                          info);
  }
  return input;
}

Result<void> validate_encoded_audio_for_adts_mux(
    const VerifiedVideoEncodedAudio& audio) {
  const auto& state = audio.state;
  if (state.codec != EncodedAudioCodec::aac || state.channels == 0U ||
      state.channels > 2U || state.sample_rate == 0U ||
      state.sample_rate >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      state.packets.empty()) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded audio is invalid before ADTS recovery");
  }
  if (!state.decoder_config.empty()) {
    return fail(ErrorCode::invalid_argument,
                "ADTS recovery is only valid when AAC decoder config is absent");
  }
  if (state.trim_start_frames != 0U) {
    return fail(ErrorCode::model_incompatible,
                "encoded AAC leading trim cannot be represented by H.1 MP4 passthrough");
  }
  return {};
}

Result<std::int64_t> nonnegative_delta(std::int64_t value,
                                       std::int64_t origin,
                                       const char* label) {
  if (value < origin) {
    return fail<std::int64_t>(ErrorCode::invalid_argument,
                              std::string{label} + " is not monotonic");
  }
  if (origin < 0 && value > std::numeric_limits<std::int64_t>::max() + origin) {
    return fail<std::int64_t>(ErrorCode::resource_exhausted,
                              std::string{label} + " exceeds export range");
  }
  return value - origin;
}

Result<PacketPtr> encoded_audio_packet_for_bsf(
    const EncodedAudioPacket& encoded, std::int64_t audio_delta,
    std::int64_t presentation_duration) {
  if (encoded.payload.empty() ||
      encoded.payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      encoded.pts_offset_ns < 0 || encoded.dts_offset_ns < 0 ||
      encoded.pts_offset_ns >= presentation_duration) {
    return fail<PacketPtr>(
        encoded.pts_offset_ns < 0 || encoded.dts_offset_ns < 0
            ? ErrorCode::model_incompatible
            : ErrorCode::archive_corrupt,
        "encoded AAC packet timing cannot be represented by MP4 passthrough");
  }
  if (audio_delta > std::numeric_limits<std::int64_t>::max() -
                        encoded.pts_offset_ns ||
      audio_delta > std::numeric_limits<std::int64_t>::max() -
                        encoded.dts_offset_ns) {
    return fail<PacketPtr>(ErrorCode::resource_exhausted,
                           "encoded AAC packet timestamp exceeds MP4 bounds");
  }
  const auto remaining = static_cast<std::uint64_t>(
      presentation_duration - encoded.pts_offset_ns);
  const auto duration = std::min(encoded.duration_ns, remaining);
  if (duration == 0U ||
      duration > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max())) {
    return fail<PacketPtr>(ErrorCode::archive_corrupt,
                           "encoded AAC packet has invalid presentation duration");
  }
  return packet_from_bytes(encoded.payload,
                           audio_delta + encoded.pts_offset_ns,
                           audio_delta + encoded.dts_offset_ns, duration,
                           encoded.flags);
}

#endif

}  // namespace

Result<std::vector<std::byte>> mux_verified_encoded_video_packets(
    const VerifiedVideoEncodedVideo& video,
    std::uint64_t maximum_output_bytes) {
#ifndef CODEC_HAS_FFMPEG_VIDEO
  (void)video;
  (void)maximum_output_bytes;
  return fail<std::vector<std::byte>>(
      ErrorCode::model_incompatible,
      "FFmpeg video export backend is unavailable");
#else
  if (maximum_output_bytes == 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "video MP4 export output limit must be non-zero");
  }
  auto valid = validate_encoded_video_for_mux(video);
  if (!valid) return valid.error();
  auto config = h264_configuration_for_mp4(video.state);
  if (!config) return config.error();

  AVFormatContext* raw = nullptr;
  const auto allocated =
      avformat_alloc_output_context2(&raw, nullptr, "mp4", nullptr);
  if (allocated < 0 || raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for encoded video",
        allocated < 0 ? allocated : AVERROR(ENOMEM));
  }
  OutputFormatPtr output{raw};
  AVStream* stream = avformat_new_stream(output.get(), nullptr);
  if (stream == nullptr || stream->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate MP4 H.264 stream");
  }
  stream->time_base = kMp4VideoTimeBase;
  stream->sample_aspect_ratio = AVRational{
      static_cast<int>(video.state.sample_aspect_ratio_numerator),
      static_cast<int>(video.state.sample_aspect_ratio_denominator)};
  stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  stream->codecpar->codec_id = AV_CODEC_ID_H264;
  stream->codecpar->codec_tag = 0;
  stream->codecpar->profile = video.state.codec_profile;
  stream->codecpar->level = video.state.codec_level;
  stream->codecpar->width = static_cast<int>(video.state.coded_width);
  stream->codecpar->height = static_cast<int>(video.state.coded_height);
  stream->codecpar->sample_aspect_ratio = stream->sample_aspect_ratio;
  auto configured = replace_extradata(*stream->codecpar, *config);
  if (!configured) return configured.error();

  DynamicOutput dynamic{output.get()};
  auto opened = dynamic.open();
  if (!opened) return opened.error();
  const auto header = avformat_write_header(output.get(), nullptr);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot write encoded H.264 MP4 header", header);
  }
  auto header_bound = dynamic.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();

  for (const auto& encoded : video.state.packets) {
    auto packet = packet_from_bytes(encoded.payload, encoded.pts_offset_ns,
                                    encoded.dts_offset_ns,
                                    encoded.duration_ns, encoded.flags);
    if (!packet) return packet.error();
    av_packet_rescale_ts(packet->get(), kNanoseconds, stream->time_base);
    if (packet->get()->duration <= 0) packet->get()->duration = 1;
    packet->get()->stream_index = stream->index;
    packet->get()->pos = -1;
    const auto written = av_interleaved_write_frame(output.get(), packet->get());
    if (written < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::model_incompatible,
          "cannot write stored H.264 packet to MP4", written);
    }
    auto bound = dynamic.require_within(maximum_output_bytes);
    if (!bound) return bound.error();
  }

  const auto trailer = av_write_trailer(output.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot finalize encoded H.264 MP4", trailer);
  }
  auto trailer_bound = dynamic.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic.close(maximum_output_bytes);
#endif
}

Result<std::vector<std::byte>> mux_verified_adts_encoded_audio(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes) {
#ifndef CODEC_HAS_FFMPEG_VIDEO
  (void)video_mp4;
  (void)audio;
  (void)video_origin_ns;
  (void)maximum_output_bytes;
  return fail<std::vector<std::byte>>(
      ErrorCode::model_incompatible,
      "FFmpeg video export backend is unavailable");
#else
  if (maximum_output_bytes == 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "video MP4 export output limit must be non-zero");
  }
  auto valid = validate_encoded_audio_for_adts_mux(audio);
  if (!valid) return valid.error();
  auto presentation_duration = nonnegative_delta(
      audio.state_record.end_ns, audio.state_record.start_ns,
      "encoded AAC presentation duration");
  if (!presentation_duration) return presentation_duration.error();
  if (*presentation_duration <= 0) {
    return fail<std::vector<std::byte>>(
        ErrorCode::archive_corrupt,
        "encoded AAC presentation duration is empty");
  }
  auto audio_delta = nonnegative_delta(audio.state_record.start_ns,
                                       video_origin_ns,
                                       "video MP4 encoded-audio timestamp");
  if (!audio_delta) return audio_delta.error();

  auto filter = make_bsf("aac_adtstoasc", AVMEDIA_TYPE_AUDIO,
                         AV_CODEC_ID_AAC, kNanoseconds,
                         audio.state.codec_profile, 0, 0U, 0U,
                         audio.state.sample_rate, audio.state.channels);
  if (!filter) return filter.error();
  auto first_input = encoded_audio_packet_for_bsf(
      audio.state.packets.front(), *audio_delta, *presentation_duration);
  if (!first_input) return first_input.error();
  const auto first_sent = av_bsf_send_packet(filter->get(), first_input->get());
  if (first_sent < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "AAC ADTS-to-ASC conversion rejected the first stored packet",
        first_sent);
  }
  PacketPtr first_filtered{av_packet_alloc()};
  if (!first_filtered) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot allocate filtered AAC packet");
  }
  const auto first_received =
      av_bsf_receive_packet(filter->get(), first_filtered.get());
  if (first_received < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "AAC ADTS-to-ASC conversion produced no usable first packet",
        first_received);
  }
  std::size_t asc_size = 0U;
  const auto* asc = av_packet_get_side_data(
      first_filtered.get(), AV_PKT_DATA_NEW_EXTRADATA, &asc_size);
  if (asc == nullptr || asc_size == 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "AAC ADTS packets do not expose recoverable AudioSpecificConfig");
  }

  MemoryInput memory{};
  AvioPtr input_avio{};
  auto input = open_memory_mp4(video_mp4, memory, input_avio);
  if (!input) return input.error();
  const auto video_index = av_find_best_stream(
      input->get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0 ||
      static_cast<unsigned>(video_index) >= input->get()->nb_streams ||
      input->get()->streams[video_index] == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::decode,
        "generated MP4 has no video stream for recovered AAC mux");
  }
  AVStream* input_video = input->get()->streams[video_index];

  AVFormatContext* raw = nullptr;
  const auto format_allocated =
      avformat_alloc_output_context2(&raw, nullptr, "mp4", nullptr);
  if (format_allocated < 0 || raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for recovered AAC",
        format_allocated < 0 ? format_allocated : AVERROR(ENOMEM));
  }
  OutputFormatPtr output{raw};
  AVStream* output_video = avformat_new_stream(output.get(), nullptr);
  if (output_video == nullptr || output_video->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate recovered-AAC MP4 video stream");
  }
  const auto copied_video =
      avcodec_parameters_copy(output_video->codecpar, input_video->codecpar);
  if (copied_video < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot copy recovered-AAC MP4 video parameters", copied_video);
  }
  output_video->codecpar->codec_tag = 0;
  output_video->time_base = input_video->time_base;

  AVStream* output_audio = avformat_new_stream(output.get(), nullptr);
  if (output_audio == nullptr || output_audio->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate recovered-AAC MP4 audio stream");
  }
  const auto copied_audio =
      avcodec_parameters_copy(output_audio->codecpar, filter->get()->par_out);
  if (copied_audio < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot copy filtered AAC stream parameters", copied_audio);
  }
  output_audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  output_audio->codecpar->codec_id = AV_CODEC_ID_AAC;
  output_audio->codecpar->codec_tag = 0;
  output_audio->codecpar->profile = audio.state.codec_profile;
  output_audio->codecpar->sample_rate =
      static_cast<int>(audio.state.sample_rate);
  output_audio->codecpar->frame_size = 1024;
  if (output_audio->codecpar->ch_layout.nb_channels == 0) {
    av_channel_layout_default(&output_audio->codecpar->ch_layout,
                              static_cast<int>(audio.state.channels));
  }
  const auto asc_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(asc), asc_size};
  auto configured = replace_extradata(*output_audio->codecpar, asc_bytes);
  if (!configured) return configured.error();
  output_audio->time_base =
      AVRational{1, static_cast<int>(audio.state.sample_rate)};

  DynamicOutput dynamic{output.get()};
  auto opened = dynamic.open();
  if (!opened) return opened.error();
  const auto header = avformat_write_header(output.get(), nullptr);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot write MP4 header with recovered AAC configuration", header);
  }
  auto header_bound = dynamic.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();

  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot allocate MP4 remux packet");
  }
  for (;;) {
    const auto read = av_read_frame(input->get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::decode,
          "cannot read generated MP4 video packet for recovered AAC", read);
    }
    if (packet->stream_index == video_index) {
      av_packet_rescale_ts(packet.get(), input_video->time_base,
                           output_video->time_base);
      packet->stream_index = output_video->index;
      packet->pos = -1;
      const auto written = av_interleaved_write_frame(output.get(), packet.get());
      if (written < 0) {
        av_packet_unref(packet.get());
        return ffmpeg_failure<std::vector<std::byte>>(
            ErrorCode::internal,
            "cannot write video packet while muxing recovered AAC", written);
      }
      auto bound = dynamic.require_within(maximum_output_bytes);
      if (!bound) return bound.error();
    }
    av_packet_unref(packet.get());
  }

  const auto filter_time_base =
      filter->get()->time_base_out.num > 0 && filter->get()->time_base_out.den > 0
          ? filter->get()->time_base_out
          : kNanoseconds;
  auto write_filtered = [&](AVPacket& filtered) -> Result<void> {
    av_packet_rescale_ts(&filtered, filter_time_base, output_audio->time_base);
    if (filtered.duration <= 0) filtered.duration = 1;
    filtered.stream_index = output_audio->index;
    filtered.pos = -1;
    const auto written = av_interleaved_write_frame(output.get(), &filtered);
    if (written < 0) {
      return ffmpeg_failure<void>(ErrorCode::model_incompatible,
                                  "cannot write filtered AAC packet to MP4",
                                  written);
    }
    return dynamic.require_within(maximum_output_bytes);
  };

  auto first_written = write_filtered(*first_filtered);
  if (!first_written) return first_written.error();
  av_packet_unref(first_filtered.get());

  PacketPtr filtered{av_packet_alloc()};
  if (!filtered) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot allocate AAC bitstream-filter output packet");
  }
  for (std::size_t index = 1U; index < audio.state.packets.size(); ++index) {
    auto input_packet = encoded_audio_packet_for_bsf(
        audio.state.packets[index], *audio_delta, *presentation_duration);
    if (!input_packet) return input_packet.error();
    const auto sent = av_bsf_send_packet(filter->get(), input_packet->get());
    if (sent < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::model_incompatible,
          "AAC ADTS-to-ASC conversion rejected a stored packet", sent);
    }
    for (;;) {
      const auto received =
          av_bsf_receive_packet(filter->get(), filtered.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) break;
      if (received < 0) {
        return ffmpeg_failure<std::vector<std::byte>>(
            ErrorCode::model_incompatible,
            "AAC ADTS-to-ASC conversion failed", received);
      }
      auto written = write_filtered(*filtered);
      av_packet_unref(filtered.get());
      if (!written) return written.error();
    }
  }
  const auto flushed = av_bsf_send_packet(filter->get(), nullptr);
  if (flushed < 0 && flushed != AVERROR_EOF) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot flush AAC ADTS-to-ASC conversion", flushed);
  }
  for (;;) {
    const auto received = av_bsf_receive_packet(filter->get(), filtered.get());
    if (received == AVERROR_EOF || received == AVERROR(EAGAIN)) break;
    if (received < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::model_incompatible,
          "AAC ADTS-to-ASC flush failed", received);
    }
    auto written = write_filtered(*filtered);
    av_packet_unref(filtered.get());
    if (!written) return written.error();
  }

  const auto trailer = av_write_trailer(output.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot finalize MP4 with recovered AAC configuration", trailer);
  }
  auto trailer_bound = dynamic.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic.close(maximum_output_bytes);
#endif
}

}  // namespace codec::profiles::video
