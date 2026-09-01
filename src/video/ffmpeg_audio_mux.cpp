#include "ffmpeg_packet_mux.hpp"

#include <algorithm>
#include <array>
#include <climits>
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
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace codec::profiles::video {
namespace {

#ifdef CODEC_HAS_FFMPEG_VIDEO

constexpr AVRational kNanoseconds{1, 1'000'000'000};

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

struct CodecDeleter {
  void operator()(AVCodecContext* value) const noexcept {
    if (value != nullptr) avcodec_free_context(&value);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* value) const noexcept {
    if (value != nullptr) av_frame_free(&value);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept {
    if (value != nullptr) av_packet_free(&value);
  }
};

struct AvioDeleter {
  void operator()(AVIOContext* value) const noexcept {
    if (value == nullptr) return;
    av_freep(&value->buffer);
    avio_context_free(&value);
  }
};

struct SwrDeleter {
  void operator()(SwrContext* value) const noexcept {
    if (value != nullptr) swr_free(&value);
  }
};

struct AudioFifoDeleter {
  void operator()(AVAudioFifo* value) const noexcept {
    if (value != nullptr) av_audio_fifo_free(value);
  }
};

using OutputFormatPtr = std::unique_ptr<AVFormatContext, OutputFormatDeleter>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

class DynamicOutput final {
 public:
  explicit DynamicOutput(AVFormatContext* format) : format_(format) {}
  ~DynamicOutput() {
    if (!active_ || format_ == nullptr || format_->pb == nullptr) return;
    std::uint8_t* buffer = nullptr;
    (void)avio_close_dyn_buf(format_->pb, &buffer);
    format_->pb = nullptr;
    av_free(buffer);
  }

  Result<void> open() {
    const auto result = avio_open_dyn_buf(&format_->pb);
    if (result < 0) {
      return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                  "cannot allocate MP4 output buffer", result);
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
                  "verified video MP4 export exceeds the configured output limit");
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
          "verified video MP4 export exceeds the configured output limit");
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
  const auto count = std::min(static_cast<std::size_t>(buffer_size),
                              input.bytes.size() - input.offset);
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
  if (origin == SEEK_CUR) {
    base = static_cast<std::int64_t>(input.offset);
  } else if (origin == SEEK_END) {
    if (input.bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      return AVERROR(EOVERFLOW);
    }
    base = static_cast<std::int64_t>(input.bytes.size());
  } else if (origin != SEEK_SET) {
    return AVERROR(EINVAL);
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

struct OpenedVideoMp4 {
  MemoryInput memory{};
  AvioPtr avio{};
  InputFormatPtr input{};
  int video_index{-1};
  AVStream* video{};
};

Result<OpenedVideoMp4> open_video_mp4(std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return fail<OpenedVideoMp4>(ErrorCode::archive_corrupt,
                                "generated video MP4 is empty");
  }
  OpenedVideoMp4 opened{};
  opened.memory = MemoryInput{bytes, 0U};
  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (buffer == nullptr) {
    return fail<OpenedVideoMp4>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 remux input buffer");
  }
  opened.avio.reset(avio_alloc_context(buffer, avio_buffer_bytes, 0,
                                       &opened.memory, &memory_read, nullptr,
                                       &memory_seek));
  if (!opened.avio) {
    av_free(buffer);
    return fail<OpenedVideoMp4>(ErrorCode::resource_exhausted,
                                "cannot create MP4 remux input context");
  }
  AVFormatContext* raw = avformat_alloc_context();
  if (raw == nullptr) {
    return fail<OpenedVideoMp4>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 remux format context");
  }
  raw->pb = opened.avio.get();
  raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (raw->protocol_whitelist == nullptr) {
    avformat_free_context(raw);
    return fail<OpenedVideoMp4>(ErrorCode::resource_exhausted,
                                "cannot allocate MP4 remux protocol policy");
  }
  const auto input_opened = avformat_open_input(&raw, nullptr, nullptr, nullptr);
  if (input_opened < 0) {
    if (raw != nullptr) avformat_free_context(raw);
    return ffmpeg_failure<OpenedVideoMp4>(
        ErrorCode::decode, "cannot reopen generated MP4 for audio mux",
        input_opened);
  }
  opened.input.reset(raw);
  const auto info = avformat_find_stream_info(opened.input.get(), nullptr);
  if (info < 0) {
    return ffmpeg_failure<OpenedVideoMp4>(
        ErrorCode::decode, "cannot inspect generated MP4 for audio mux", info);
  }
  opened.video_index = av_find_best_stream(opened.input.get(),
                                            AVMEDIA_TYPE_VIDEO, -1, -1,
                                            nullptr, 0);
  if (opened.video_index < 0 ||
      static_cast<unsigned>(opened.video_index) >= opened.input->nb_streams ||
      opened.input->streams[opened.video_index] == nullptr) {
    return fail<OpenedVideoMp4>(
        ErrorCode::decode, "generated MP4 has no video stream for audio mux");
  }
  opened.video = opened.input->streams[opened.video_index];
  return opened;
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

Result<void> copy_extradata(AVCodecParameters& parameters,
                            std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return fail(ErrorCode::model_incompatible,
                "encoded AAC decoder configuration is empty");
  }
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return fail(ErrorCode::resource_exhausted,
                "encoded AAC decoder configuration exceeds FFmpeg bounds");
  }
  parameters.extradata = static_cast<std::uint8_t*>(
      av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (parameters.extradata == nullptr) {
    return fail(ErrorCode::resource_exhausted,
                "cannot allocate encoded AAC decoder configuration");
  }
  std::memcpy(parameters.extradata, bytes.data(), bytes.size());
  parameters.extradata_size = static_cast<int>(bytes.size());
  return {};
}

Result<void> copy_video_packets(OpenedVideoMp4& input,
                                AVFormatContext& output,
                                AVStream& output_video,
                                DynamicOutput& dynamic,
                                std::uint64_t maximum_output_bytes) {
  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail(ErrorCode::resource_exhausted,
                "cannot allocate MP4 remux packet");
  }
  for (;;) {
    const auto read = av_read_frame(input.input.get(), packet.get());
    if (read == AVERROR_EOF) return {};
    if (read < 0) {
      return ffmpeg_failure<void>(ErrorCode::decode,
                                  "cannot read generated MP4 video packet",
                                  read);
    }
    if (packet->stream_index == input.video_index) {
      av_packet_rescale_ts(packet.get(), input.video->time_base,
                           output_video.time_base);
      packet->stream_index = output_video.index;
      packet->pos = -1;
      const auto written = av_write_frame(&output, packet.get());
      av_packet_unref(packet.get());
      if (written < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot write remuxed MP4 video packet",
                                    written);
      }
      auto bound = dynamic.require_within(maximum_output_bytes);
      if (!bound) return bound.error();
    } else {
      av_packet_unref(packet.get());
    }
  }
}

int choose_aac_sample_rate(const AVCodec& codec, std::uint32_t requested) {
  if (requested == 0U || requested > static_cast<std::uint32_t>(INT_MAX)) {
    return 0;
  }
  const auto requested_int = static_cast<int>(requested);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 12, 100)
  const void* configurations = nullptr;
  int configuration_count = 0;
  const auto queried = avcodec_get_supported_config(
      nullptr, &codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configurations,
      &configuration_count);
  if (queried < 0) return 0;
  if (configurations == nullptr) return requested_int;
  if (configuration_count <= 0) return 0;
  const auto* rates = static_cast<const int*>(configurations);
#else
  if (codec.supported_samplerates == nullptr) return requested_int;
  const int* rates = codec.supported_samplerates;
#endif
  int best = 0;
  std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 12, 100)
  for (int index = 0; index < configuration_count; ++index) {
    const auto rate = rates[index];
#else
  for (const int* candidate = rates; *candidate != 0; ++candidate) {
    const auto rate = *candidate;
#endif
    if (rate <= 0) continue;
    if (rate == requested_int) return requested_int;
    const auto distance = static_cast<std::uint64_t>(
        rate > requested_int ? rate - requested_int : requested_int - rate);
    if (best == 0 || distance < best_distance ||
        (distance == best_distance && rate < best)) {
      best = rate;
      best_distance = distance;
    }
  }
  return best;
}

AVSampleFormat choose_aac_sample_format(const AVCodec& codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 12, 100)
  const void* configurations = nullptr;
  int configuration_count = 0;
  const auto queried = avcodec_get_supported_config(
      nullptr, &codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configurations,
      &configuration_count);
  if (queried < 0) return AV_SAMPLE_FMT_NONE;
  if (configurations == nullptr) return AV_SAMPLE_FMT_FLTP;
  if (configuration_count <= 0) return AV_SAMPLE_FMT_NONE;
  const auto* formats = static_cast<const AVSampleFormat*>(configurations);
  for (int index = 0; index < configuration_count; ++index) {
    if (formats[index] == AV_SAMPLE_FMT_FLTP) return formats[index];
  }
  return formats[0];
#else
  if (codec.sample_fmts == nullptr) return AV_SAMPLE_FMT_NONE;
  for (const AVSampleFormat* format = codec.sample_fmts;
       *format != AV_SAMPLE_FMT_NONE; ++format) {
    if (*format == AV_SAMPLE_FMT_FLTP) return *format;
  }
  return codec.sample_fmts[0];
#endif
}

#endif

}  // namespace

Result<std::vector<std::byte>> mux_verified_encoded_audio_packets(
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
  if (audio.state.decoder_config.empty()) {
    return mux_verified_adts_encoded_audio(video_mp4, audio, video_origin_ns,
                                           maximum_output_bytes);
  }
  if (audio.state.codec != EncodedAudioCodec::aac ||
      audio.state.channels == 0U || audio.state.channels > 2U ||
      audio.state.sample_rate == 0U ||
      audio.state.sample_rate >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      audio.state.packets.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::archive_corrupt,
        "verified encoded video audio is invalid before MP4 mux");
  }
  if (audio.state.trim_start_frames != 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "encoded AAC leading trim cannot be represented by H.1 MP4 passthrough");
  }
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

  auto input = open_video_mp4(video_mp4);
  if (!input) return input.error();
  AVFormatContext* raw = nullptr;
  const auto format_result =
      avformat_alloc_output_context2(&raw, nullptr, "mp4", nullptr);
  if (format_result < 0 || raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for encoded-audio export",
        format_result < 0 ? format_result : AVERROR(ENOMEM));
  }
  OutputFormatPtr output{raw};
  AVStream* output_video = avformat_new_stream(output.get(), nullptr);
  if (output_video == nullptr || output_video->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate encoded-audio remuxed MP4 video stream");
  }
  const auto copied_video =
      avcodec_parameters_copy(output_video->codecpar, input->video->codecpar);
  if (copied_video < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot copy encoded-audio MP4 video stream parameters",
        copied_video);
  }
  output_video->codecpar->codec_tag = 0;
  output_video->time_base = input->video->time_base;

  AVStream* output_audio = avformat_new_stream(output.get(), nullptr);
  if (output_audio == nullptr || output_audio->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate encoded-audio MP4 audio stream");
  }
  output_audio->time_base =
      AVRational{1, static_cast<int>(audio.state.sample_rate)};
  output_audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  output_audio->codecpar->codec_id = AV_CODEC_ID_AAC;
  output_audio->codecpar->codec_tag = 0;
  output_audio->codecpar->format = AV_SAMPLE_FMT_FLTP;
  output_audio->codecpar->sample_rate =
      static_cast<int>(audio.state.sample_rate);
  output_audio->codecpar->profile = audio.state.codec_profile;
  output_audio->codecpar->frame_size = 1024;
  av_channel_layout_default(&output_audio->codecpar->ch_layout,
                            static_cast<int>(audio.state.channels));
  auto configured = copy_extradata(
      *output_audio->codecpar,
      std::span<const std::byte>{audio.state.decoder_config.data(),
                                 audio.state.decoder_config.size()});
  if (!configured) return configured.error();

  DynamicOutput dynamic{output.get()};
  auto opened = dynamic.open();
  if (!opened) return opened.error();
  const auto header = avformat_write_header(output.get(), nullptr);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot write encoded-audio MP4 header", header);
  }
  auto header_bound = dynamic.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();
  auto video_copied = copy_video_packets(*input, *output, *output_video,
                                         dynamic, maximum_output_bytes);
  if (!video_copied) return video_copied.error();

  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot allocate encoded-audio MP4 packet");
  }
  for (const auto& encoded : audio.state.packets) {
    if (encoded.payload.empty() ||
        encoded.payload.size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        encoded.pts_offset_ns < 0 || encoded.dts_offset_ns < 0 ||
        encoded.pts_offset_ns >= *presentation_duration) {
      return fail<std::vector<std::byte>>(
          encoded.pts_offset_ns < 0 || encoded.dts_offset_ns < 0
              ? ErrorCode::model_incompatible
              : ErrorCode::archive_corrupt,
          "encoded AAC packet timing cannot be represented by MP4 passthrough");
    }
    if (*audio_delta > std::numeric_limits<std::int64_t>::max() -
                           encoded.pts_offset_ns ||
        *audio_delta > std::numeric_limits<std::int64_t>::max() -
                           encoded.dts_offset_ns) {
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "encoded AAC packet timestamp exceeds MP4 bounds");
    }
    const auto remaining = static_cast<std::uint64_t>(
        *presentation_duration - encoded.pts_offset_ns);
    const auto duration = std::min(encoded.duration_ns, remaining);
    if (duration == 0U ||
        duration > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
      return fail<std::vector<std::byte>>(
          ErrorCode::archive_corrupt,
          "encoded AAC packet has invalid presentation duration");
    }
    const auto allocated =
        av_new_packet(packet.get(), static_cast<int>(encoded.payload.size()));
    if (allocated < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "cannot allocate encoded AAC MP4 packet", allocated);
    }
    std::memcpy(packet->data, encoded.payload.data(), encoded.payload.size());
    packet->pts = av_rescale_q(*audio_delta + encoded.pts_offset_ns,
                               kNanoseconds, output_audio->time_base);
    packet->dts = av_rescale_q(*audio_delta + encoded.dts_offset_ns,
                               kNanoseconds, output_audio->time_base);
    packet->duration = std::max<std::int64_t>(
        1, av_rescale_q(static_cast<std::int64_t>(duration), kNanoseconds,
                        output_audio->time_base));
    packet->flags = static_cast<int>(encoded.flags);
    packet->stream_index = output_audio->index;
    packet->pos = -1;
    const auto written = av_write_frame(output.get(), packet.get());
    av_packet_unref(packet.get());
    if (written < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::internal,
          "cannot write encoded AAC MP4 packet", written);
    }
    auto bound = dynamic.require_within(maximum_output_bytes);
    if (!bound) return bound.error();
  }

  const auto trailer = av_write_trailer(output.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot finalize encoded-audio MP4", trailer);
  }
  auto trailer_bound = dynamic.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic.close(maximum_output_bytes);
#endif
}

Result<std::vector<std::byte>> mux_verified_pcm16_audio(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoPcm16Audio& audio,
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
  if (audio.state.channels == 0U || audio.state.channels > 2U ||
      audio.state.sample_rate == 0U || audio.state.samples.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::archive_corrupt,
        "verified video audio is invalid before MP4 mux");
  }
  const auto channels = static_cast<int>(audio.state.channels);
  if (audio.state.samples.size() % static_cast<std::size_t>(channels) != 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::archive_corrupt,
        "verified video audio sample geometry is invalid before MP4 mux");
  }
  auto input = open_video_mp4(video_mp4);
  if (!input) return input.error();
  const AVCodec* aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (aac == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "verified audio exists but FFmpeg AAC encoder is unavailable");
  }
  const auto output_rate = choose_aac_sample_rate(*aac, audio.state.sample_rate);
  const auto output_format = choose_aac_sample_format(*aac);
  if (output_rate <= 0 || output_format == AV_SAMPLE_FMT_NONE) {
    return fail<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg AAC encoder has no compatible audio format");
  }

  AVFormatContext* raw = nullptr;
  const auto format_result =
      avformat_alloc_output_context2(&raw, nullptr, "mp4", nullptr);
  if (format_result < 0 || raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for audiovisual export",
        format_result < 0 ? format_result : AVERROR(ENOMEM));
  }
  OutputFormatPtr output{raw};
  AVStream* output_video = avformat_new_stream(output.get(), nullptr);
  if (output_video == nullptr || output_video->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate remuxed MP4 video stream");
  }
  const auto copied_video =
      avcodec_parameters_copy(output_video->codecpar, input->video->codecpar);
  if (copied_video < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot copy MP4 video stream parameters",
        copied_video);
  }
  output_video->codecpar->codec_tag = 0;
  output_video->time_base = input->video->time_base;

  AVStream* output_audio = avformat_new_stream(output.get(), nullptr);
  if (output_audio == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate MP4 audio stream");
  }
  CodecPtr encoder{avcodec_alloc_context3(aac)};
  if (!encoder) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate FFmpeg AAC encoder");
  }
  encoder->codec_type = AVMEDIA_TYPE_AUDIO;
  encoder->codec_id = AV_CODEC_ID_AAC;
  encoder->sample_rate = output_rate;
  encoder->sample_fmt = output_format;
  encoder->time_base = AVRational{1, output_rate};
  encoder->bit_rate = channels == 1 ? 64'000 : 128'000;
  av_channel_layout_default(&encoder->ch_layout, channels);
  if ((output->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  const auto encoder_opened = avcodec_open2(encoder.get(), aac, nullptr);
  if (encoder_opened < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot open FFmpeg AAC encoder", encoder_opened);
  }
  output_audio->time_base = encoder->time_base;
  const auto copied_audio =
      avcodec_parameters_from_context(output_audio->codecpar, encoder.get());
  if (copied_audio < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot configure MP4 AAC stream parameters", copied_audio);
  }

  AVChannelLayout input_layout{};
  av_channel_layout_default(&input_layout, channels);
  SwrContext* swr_raw = nullptr;
  const auto swr_allocated = swr_alloc_set_opts2(
      &swr_raw, &encoder->ch_layout, encoder->sample_fmt,
      encoder->sample_rate, &input_layout, AV_SAMPLE_FMT_S16,
      static_cast<int>(audio.state.sample_rate), 0, nullptr);
  av_channel_layout_uninit(&input_layout);
  if (swr_allocated < 0 || swr_raw == nullptr) {
    if (swr_raw != nullptr) swr_free(&swr_raw);
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot create AAC sample converter",
        swr_allocated < 0 ? swr_allocated : AVERROR(ENOMEM));
  }
  SwrPtr swr{swr_raw};
  const auto swr_initialized = swr_init(swr.get());
  if (swr_initialized < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot initialize AAC sample converter", swr_initialized);
  }

  const auto frame_size = encoder->frame_size > 0 ? encoder->frame_size : 1024;
  AudioFifoPtr fifo{av_audio_fifo_alloc(encoder->sample_fmt, channels,
                                        std::max(frame_size, 1))};
  if (!fifo) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate AAC audio FIFO");
  }

  DynamicOutput dynamic{output.get()};
  auto opened = dynamic.open();
  if (!opened) return opened.error();
  const auto header = avformat_write_header(output.get(), nullptr);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot write audiovisual MP4 header", header);
  }
  auto header_bound = dynamic.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();
  auto video_copied = copy_video_packets(*input, *output, *output_video,
                                         dynamic, maximum_output_bytes);
  if (!video_copied) return video_copied.error();

  auto audio_delta = nonnegative_delta(audio.state_record.start_ns,
                                       video_origin_ns,
                                       "video MP4 audio timestamp");
  if (!audio_delta) return audio_delta.error();
  const auto audio_start_pts =
      av_rescale_q(*audio_delta, kNanoseconds, encoder->time_base);
  std::int64_t samples_submitted = 0;
  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate AAC export packet");
  }

  auto receive_packets = [&]() -> Result<void> {
    for (;;) {
      const auto received = avcodec_receive_packet(encoder.get(), packet.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
      if (received < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "AAC packet encode failed", received);
      }
      av_packet_rescale_ts(packet.get(), encoder->time_base,
                           output_audio->time_base);
      packet->stream_index = output_audio->index;
      packet->pos = -1;
      const auto written = av_write_frame(output.get(), packet.get());
      av_packet_unref(packet.get());
      if (written < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot write MP4 AAC packet", written);
      }
      auto bound = dynamic.require_within(maximum_output_bytes);
      if (!bound) return bound.error();
    }
  };

  auto encode_fifo = [&](bool final) -> Result<void> {
    while (av_audio_fifo_size(fifo.get()) >= frame_size ||
           (final && av_audio_fifo_size(fifo.get()) > 0)) {
      const auto available = av_audio_fifo_size(fifo.get());
      const auto take = std::min(available, frame_size);
      FramePtr frame{av_frame_alloc()};
      if (!frame) {
        return fail(ErrorCode::resource_exhausted,
                    "cannot allocate AAC export frame");
      }
      frame->nb_samples = frame_size;
      frame->format = encoder->sample_fmt;
      frame->sample_rate = encoder->sample_rate;
      const auto layout_copied =
          av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
      if (layout_copied < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot copy AAC channel layout",
                                    layout_copied);
      }
      const auto buffer_result = av_frame_get_buffer(frame.get(), 0);
      if (buffer_result < 0) {
        return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                    "cannot allocate AAC frame samples",
                                    buffer_result);
      }
      const auto read = av_audio_fifo_read(
          fifo.get(), reinterpret_cast<void**>(frame->data), take);
      if (read != take) {
        return fail(ErrorCode::internal,
                    "cannot drain AAC audio FIFO");
      }
      if (take < frame_size) {
        const auto silenced = av_samples_set_silence(
            frame->data, take, frame_size - take, channels,
            encoder->sample_fmt);
        if (silenced < 0) {
          return ffmpeg_failure<void>(ErrorCode::internal,
                                      "cannot pad final AAC frame", silenced);
        }
      }
      frame->pts = audio_start_pts + samples_submitted;
      samples_submitted += frame_size;
      const auto sent = avcodec_send_frame(encoder.get(), frame.get());
      if (sent < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot send AAC export frame", sent);
      }
      auto received = receive_packets();
      if (!received) return received.error();
    }
    return {};
  };

  auto append_converted = [&](const std::uint8_t* input_data,
                              int input_frames) -> Result<void> {
    const auto capacity = swr_get_out_samples(swr.get(), input_frames);
    if (capacity < 0) {
      return ffmpeg_failure<void>(ErrorCode::internal,
                                  "cannot size AAC sample conversion",
                                  capacity);
    }
    if (capacity == 0) return {};
    std::uint8_t** converted = nullptr;
    int linesize = 0;
    const auto allocated = av_samples_alloc_array_and_samples(
        &converted, &linesize, channels, capacity, encoder->sample_fmt, 0);
    if (allocated < 0) {
      return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                  "cannot allocate converted AAC samples",
                                  allocated);
    }
    const std::uint8_t* inputs[1]{input_data};
    const auto converted_frames = swr_convert(
        swr.get(), converted, capacity,
        input_data == nullptr ? nullptr : inputs, input_frames);
    if (converted_frames < 0) {
      av_freep(&converted[0]);
      av_freep(&converted);
      return ffmpeg_failure<void>(ErrorCode::internal,
                                  "AAC sample conversion failed",
                                  converted_frames);
    }
    if (converted_frames > 0) {
      const auto current = av_audio_fifo_size(fifo.get());
      if (current > std::numeric_limits<int>::max() - converted_frames) {
        av_freep(&converted[0]);
        av_freep(&converted);
        return fail(ErrorCode::resource_exhausted,
                    "AAC audio FIFO exceeds process bounds");
      }
      const auto resized =
          av_audio_fifo_realloc(fifo.get(), current + converted_frames);
      if (resized < 0) {
        av_freep(&converted[0]);
        av_freep(&converted);
        return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                    "cannot grow AAC audio FIFO", resized);
      }
      const auto written = av_audio_fifo_write(
          fifo.get(), reinterpret_cast<void**>(converted), converted_frames);
      if (written != converted_frames) {
        av_freep(&converted[0]);
        av_freep(&converted);
        return fail(ErrorCode::internal,
                    "cannot fill AAC audio FIFO");
      }
    }
    av_freep(&converted[0]);
    av_freep(&converted);
    return encode_fifo(false);
  };

  const auto total_input_frames = audio.state.frames();
  std::uint64_t input_offset = 0U;
  constexpr std::uint64_t chunk_frames = 4096U;
  while (input_offset < total_input_frames) {
    const auto count = std::min(chunk_frames, total_input_frames - input_offset);
    if (count > static_cast<std::uint64_t>(INT_MAX)) {
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "AAC conversion chunk exceeds process bounds");
    }
    const auto sample_offset = input_offset * audio.state.channels;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(
        audio.state.samples.data() + static_cast<std::size_t>(sample_offset));
    auto converted = append_converted(bytes, static_cast<int>(count));
    if (!converted) return converted.error();
    input_offset += count;
  }
  for (;;) {
    const auto before = av_audio_fifo_size(fifo.get());
    auto converted = append_converted(nullptr, 0);
    if (!converted) return converted.error();
    if (av_audio_fifo_size(fifo.get()) == before) break;
  }
  auto final_audio = encode_fifo(true);
  if (!final_audio) return final_audio.error();
  const auto flush_sent = avcodec_send_frame(encoder.get(), nullptr);
  if (flush_sent < 0 && flush_sent != AVERROR_EOF) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot flush AAC encoder", flush_sent);
  }
  auto final_packets = receive_packets();
  if (!final_packets) return final_packets.error();

  const auto trailer = av_write_trailer(output.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot finalize audiovisual MP4", trailer);
  }
  auto trailer_bound = dynamic.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic.close(maximum_output_bytes);
#endif
}

}  // namespace codec::profiles::video
