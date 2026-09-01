#include "ffmpeg_aac_trim_mux.hpp"

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
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
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
                                  "cannot allocate trim-aware MP4 output buffer",
                                  opened);
    }
    active_ = true;
    return {};
  }

  Result<void> require_within(std::uint64_t maximum_bytes) const {
    const auto position = avio_tell(format_->pb);
    if (position < 0) {
      return fail(ErrorCode::internal,
                  "cannot determine trim-aware MP4 output size");
    }
    if (static_cast<std::uint64_t>(position) > maximum_bytes) {
      return fail(ErrorCode::resource_exhausted,
                  "trim-aware MP4 export exceeds the configured output limit");
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
          ErrorCode::internal, "cannot finalize trim-aware MP4 output", size);
    }
    if (static_cast<std::uint64_t>(size) > maximum_bytes) {
      av_free(buffer);
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "trim-aware MP4 export exceeds the configured output limit");
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
                                "cannot allocate trim-aware MP4 input buffer");
  }
  avio.reset(avio_alloc_context(buffer, avio_buffer_bytes, 0, &memory,
                                &memory_read, nullptr, &memory_seek));
  if (!avio) {
    av_free(buffer);
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot create trim-aware MP4 input context");
  }

  AVFormatContext* raw = avformat_alloc_context();
  if (raw == nullptr) {
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot allocate trim-aware MP4 input format");
  }
  raw->pb = avio.get();
  raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (raw->protocol_whitelist == nullptr) {
    avformat_free_context(raw);
    return fail<InputFormatPtr>(ErrorCode::resource_exhausted,
                                "cannot allocate trim-aware MP4 protocol policy");
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

Result<void> replace_extradata(AVCodecParameters& parameters,
                               std::span<const std::byte> bytes) {
  if (bytes.empty()) {
    return fail(ErrorCode::model_incompatible,
                "trim-aware AAC passthrough requires AudioSpecificConfig");
  }
  if (bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return fail(ErrorCode::resource_exhausted,
                "trim-aware AAC configuration exceeds FFmpeg bounds");
  }
  av_freep(&parameters.extradata);
  parameters.extradata_size = 0;
  parameters.extradata = static_cast<std::uint8_t*>(
      av_mallocz(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (parameters.extradata == nullptr) {
    return fail(ErrorCode::resource_exhausted,
                "cannot allocate trim-aware AAC configuration");
  }
  std::memcpy(parameters.extradata, bytes.data(), bytes.size());
  parameters.extradata_size = static_cast<int>(bytes.size());
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

Result<std::int64_t> checked_add(std::int64_t base,
                                 std::int64_t offset,
                                 const char* label) {
  if ((offset > 0 &&
       base > std::numeric_limits<std::int64_t>::max() - offset) ||
      (offset < 0 &&
       base < std::numeric_limits<std::int64_t>::min() - offset)) {
    return fail<std::int64_t>(ErrorCode::resource_exhausted,
                              std::string{label} + " exceeds MP4 bounds");
  }
  return base + offset;
}

std::uint64_t positive_distance(std::int64_t end,
                                std::int64_t start) noexcept {
  return static_cast<std::uint64_t>(end) -
         static_cast<std::uint64_t>(start);
}

struct PreparedAudioPacket {
  std::vector<std::byte> payload;
  std::int64_t pts_ns{};
  std::int64_t dts_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
};

struct PreparedAudioStream {
  std::vector<std::byte> decoder_config;
  std::vector<PreparedAudioPacket> packets;
};

Result<PreparedAudioPacket> prepare_stored_packet(
    const EncodedAudioPacket& encoded,
    std::int64_t audio_delta,
    std::int64_t presentation_duration) {
  if (encoded.payload.empty() ||
      encoded.payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      encoded.duration_ns == 0U ||
      encoded.duration_ns > static_cast<std::uint64_t>(
                                std::numeric_limits<std::int64_t>::max()) ||
      encoded.pts_offset_ns >= presentation_duration) {
    return fail<PreparedAudioPacket>(
        ErrorCode::archive_corrupt,
        "verified leading-trim AAC packet geometry is invalid");
  }

  const auto duration = static_cast<std::int64_t>(encoded.duration_ns);
  if (encoded.pts_offset_ns >
      std::numeric_limits<std::int64_t>::max() - duration) {
    return fail<PreparedAudioPacket>(
        ErrorCode::resource_exhausted,
        "leading-trim AAC packet presentation end exceeds bounds");
  }
  if (encoded.pts_offset_ns + duration <= 0) {
    return fail<PreparedAudioPacket>(
        ErrorCode::archive_corrupt,
        "leading-trim AAC packet does not overlap presentation");
  }

  auto pts = checked_add(audio_delta, encoded.pts_offset_ns,
                         "leading-trim AAC PTS");
  if (!pts) return pts.error();
  auto dts = checked_add(audio_delta, encoded.dts_offset_ns,
                         "leading-trim AAC DTS");
  if (!dts) return dts.error();

  const auto remaining =
      positive_distance(presentation_duration, encoded.pts_offset_ns);
  const auto clipped_duration = std::min(encoded.duration_ns, remaining);
  if (clipped_duration == 0U ||
      clipped_duration > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
    return fail<PreparedAudioPacket>(
        ErrorCode::archive_corrupt,
        "leading-trim AAC packet has invalid presentation duration");
  }

  return PreparedAudioPacket{
      .payload = encoded.payload,
      .pts_ns = *pts,
      .dts_ns = *dts,
      .duration_ns = clipped_duration,
      .flags = encoded.flags,
  };
}

Result<PacketPtr> packet_from_prepared(const PreparedAudioPacket& prepared) {
  if (prepared.payload.empty() ||
      prepared.payload.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      prepared.duration_ns == 0U ||
      prepared.duration_ns > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max())) {
    return fail<PacketPtr>(ErrorCode::archive_corrupt,
                           "prepared AAC packet is invalid");
  }
  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail<PacketPtr>(ErrorCode::resource_exhausted,
                           "cannot allocate trim-aware AAC packet");
  }
  const auto allocated =
      av_new_packet(packet.get(), static_cast<int>(prepared.payload.size()));
  if (allocated < 0) {
    return ffmpeg_failure<PacketPtr>(
        ErrorCode::resource_exhausted,
        "cannot allocate trim-aware AAC packet payload", allocated);
  }
  std::memcpy(packet->data, prepared.payload.data(), prepared.payload.size());
  packet->pts = prepared.pts_ns;
  packet->dts = prepared.dts_ns;
  packet->duration = static_cast<std::int64_t>(prepared.duration_ns);
  packet->flags = static_cast<int>(prepared.flags);
  packet->pos = -1;
  return packet;
}

Result<BsfPtr> make_adts_to_asc_filter(const EncodedAudioState& state) {
  const AVBitStreamFilter* filter = av_bsf_get_by_name("aac_adtstoasc");
  if (filter == nullptr) {
    return fail<BsfPtr>(ErrorCode::model_incompatible,
                        "FFmpeg aac_adtstoasc filter is unavailable");
  }
  AVBSFContext* raw = nullptr;
  const auto allocated = av_bsf_alloc(filter, &raw);
  if (allocated < 0 || raw == nullptr) {
    return ffmpeg_failure<BsfPtr>(
        ErrorCode::resource_exhausted,
        "cannot allocate AAC ADTS-to-ASC filter",
        allocated < 0 ? allocated : AVERROR(ENOMEM));
  }
  BsfPtr context{raw};
  context->par_in->codec_type = AVMEDIA_TYPE_AUDIO;
  context->par_in->codec_id = AV_CODEC_ID_AAC;
  context->par_in->codec_tag = 0;
  context->par_in->profile = state.codec_profile;
  context->par_in->sample_rate = static_cast<int>(state.sample_rate);
  context->par_in->frame_size = 1024;
  av_channel_layout_default(&context->par_in->ch_layout,
                            static_cast<int>(state.channels));
  context->time_base_in = kNanoseconds;
  const auto initialized = av_bsf_init(context.get());
  if (initialized < 0) {
    return ffmpeg_failure<BsfPtr>(
        ErrorCode::model_incompatible,
        "cannot initialize AAC ADTS-to-ASC filter", initialized);
  }
  return context;
}

Result<void> capture_filtered_packet(AVPacket& filtered,
                                     AVBSFContext& filter,
                                     PreparedAudioStream& output) {
  if (filtered.data == nullptr || filtered.size <= 0 ||
      filtered.pts == AV_NOPTS_VALUE || filtered.dts == AV_NOPTS_VALUE) {
    return fail(ErrorCode::model_incompatible,
                "AAC ADTS-to-ASC conversion produced invalid timing or payload");
  }

  if (output.decoder_config.empty()) {
    std::size_t config_size = 0U;
    const auto* config = av_packet_get_side_data(
        &filtered, AV_PKT_DATA_NEW_EXTRADATA, &config_size);
    if (config != nullptr && config_size != 0U) {
      const auto* first = reinterpret_cast<const std::byte*>(config);
      output.decoder_config.assign(first, first + config_size);
    }
  }

  const auto filter_time_base =
      filter.time_base_out.num > 0 && filter.time_base_out.den > 0
          ? filter.time_base_out
          : kNanoseconds;
  av_packet_rescale_ts(&filtered, filter_time_base, kNanoseconds);
  if (filtered.pts == AV_NOPTS_VALUE || filtered.dts == AV_NOPTS_VALUE ||
      filtered.duration <= 0) {
    return fail(ErrorCode::model_incompatible,
                "AAC ADTS-to-ASC conversion lost packet timing");
  }

  const auto* first = reinterpret_cast<const std::byte*>(filtered.data);
  output.packets.push_back(PreparedAudioPacket{
      .payload = std::vector<std::byte>(
          first, first + static_cast<std::size_t>(filtered.size)),
      .pts_ns = filtered.pts,
      .dts_ns = filtered.dts,
      .duration_ns = static_cast<std::uint64_t>(filtered.duration),
      .flags = static_cast<std::uint32_t>(filtered.flags),
  });
  return {};
}

Result<PreparedAudioStream> prepare_adts_audio(
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t audio_delta,
    std::int64_t presentation_duration) {
  auto filter = make_adts_to_asc_filter(audio.state);
  if (!filter) return filter.error();

  PreparedAudioStream prepared;
  prepared.packets.reserve(audio.state.packets.size());
  PacketPtr filtered{av_packet_alloc()};
  if (!filtered) {
    return fail<PreparedAudioStream>(
        ErrorCode::resource_exhausted,
        "cannot allocate AAC ADTS-to-ASC output packet");
  }

  const auto receive_available = [&]() -> Result<void> {
    for (;;) {
      const auto received = av_bsf_receive_packet(filter->get(), filtered.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
      if (received < 0) {
        return ffmpeg_failure<void>(ErrorCode::model_incompatible,
                                    "AAC ADTS-to-ASC conversion failed",
                                    received);
      }
      auto captured =
          capture_filtered_packet(*filtered, *filter->get(), prepared);
      av_packet_unref(filtered.get());
      if (!captured) return captured.error();
    }
  };

  for (const auto& encoded : audio.state.packets) {
    auto stored = prepare_stored_packet(encoded, audio_delta,
                                        presentation_duration);
    if (!stored) return stored.error();
    auto packet = packet_from_prepared(*stored);
    if (!packet) return packet.error();
    const auto sent = av_bsf_send_packet(filter->get(), packet->get());
    if (sent < 0) {
      return ffmpeg_failure<PreparedAudioStream>(
          ErrorCode::model_incompatible,
          "AAC ADTS-to-ASC conversion rejected a stored packet", sent);
    }
    auto received = receive_available();
    if (!received) return received.error();
  }

  const auto flushed = av_bsf_send_packet(filter->get(), nullptr);
  if (flushed < 0 && flushed != AVERROR_EOF) {
    return ffmpeg_failure<PreparedAudioStream>(
        ErrorCode::model_incompatible,
        "cannot flush AAC ADTS-to-ASC conversion", flushed);
  }
  auto received = receive_available();
  if (!received) return received.error();

  if (prepared.decoder_config.empty() &&
      filter->get()->par_out->extradata != nullptr &&
      filter->get()->par_out->extradata_size > 0) {
    const auto* first = reinterpret_cast<const std::byte*>(
        filter->get()->par_out->extradata);
    prepared.decoder_config.assign(
        first, first + filter->get()->par_out->extradata_size);
  }
  if (prepared.decoder_config.empty() || prepared.packets.empty()) {
    return fail<PreparedAudioStream>(
        ErrorCode::model_incompatible,
        "AAC ADTS packets do not expose recoverable AudioSpecificConfig");
  }
  return prepared;
}

Result<PreparedAudioStream> prepare_audio(
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t audio_delta,
    std::int64_t presentation_duration) {
  if (audio.state.decoder_config.empty()) {
    return prepare_adts_audio(audio, audio_delta, presentation_duration);
  }

  PreparedAudioStream prepared{
      .decoder_config = audio.state.decoder_config,
      .packets = {},
  };
  prepared.packets.reserve(audio.state.packets.size());
  for (const auto& encoded : audio.state.packets) {
    auto packet = prepare_stored_packet(encoded, audio_delta,
                                        presentation_duration);
    if (!packet) return packet.error();
    prepared.packets.push_back(std::move(*packet));
  }
  return prepared;
}

Result<void> validate_trimmed_audio(const VerifiedVideoEncodedAudio& audio) {
  const auto& state = audio.state;
  if (state.codec != EncodedAudioCodec::aac || state.channels == 0U ||
      state.channels > 2U || state.sample_rate == 0U ||
      state.sample_rate >
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
      state.packets.empty() || state.trim_start_frames == 0U ||
      state.presentation_frames == 0U) {
    return fail(ErrorCode::archive_corrupt,
                "verified leading-trim AAC is invalid before MP4 mux");
  }
  return {};
}

Result<void> write_video_packets(AVFormatContext& input,
                                 int video_index,
                                 AVStream& input_video,
                                 AVFormatContext& output,
                                 AVStream& output_video,
                                 DynamicOutput& dynamic,
                                 std::uint64_t maximum_output_bytes) {
  PacketPtr packet{av_packet_alloc()};
  if (!packet) {
    return fail(ErrorCode::resource_exhausted,
                "cannot allocate trim-aware MP4 video packet");
  }
  for (;;) {
    const auto read = av_read_frame(&input, packet.get());
    if (read == AVERROR_EOF) return {};
    if (read < 0) {
      return ffmpeg_failure<void>(ErrorCode::decode,
                                  "cannot read generated MP4 video packet",
                                  read);
    }
    if (packet->stream_index == video_index) {
      av_packet_rescale_ts(packet.get(), input_video.time_base,
                           output_video.time_base);
      packet->stream_index = output_video.index;
      packet->pos = -1;
      const auto written = av_write_frame(&output, packet.get());
      if (written < 0) {
        av_packet_unref(packet.get());
        return ffmpeg_failure<void>(
            ErrorCode::internal,
            "cannot write video packet while muxing leading-trim AAC",
            written);
      }
      auto bound = dynamic.require_within(maximum_output_bytes);
      if (!bound) {
        av_packet_unref(packet.get());
        return bound.error();
      }
    }
    av_packet_unref(packet.get());
  }
}

Result<void> write_audio_packets(const PreparedAudioStream& prepared,
                                 AVFormatContext& output,
                                 AVStream& output_audio,
                                 DynamicOutput& dynamic,
                                 std::uint64_t maximum_output_bytes) {
  for (const auto& stored : prepared.packets) {
    auto packet = packet_from_prepared(stored);
    if (!packet) return packet.error();
    av_packet_rescale_ts(packet->get(), kNanoseconds, output_audio.time_base);
    if (packet->get()->duration <= 0) packet->get()->duration = 1;
    packet->get()->stream_index = output_audio.index;
    packet->get()->pos = -1;
    const auto written = av_write_frame(&output, packet->get());
    if (written < 0) {
      return ffmpeg_failure<void>(
          ErrorCode::model_incompatible,
          "cannot write leading-trim AAC packet to MP4", written);
    }
    auto bound = dynamic.require_within(maximum_output_bytes);
    if (!bound) return bound.error();
  }
  return {};
}

Result<std::vector<std::byte>> mux_trimmed_aac(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes) {
  if (maximum_output_bytes == 0U) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "video MP4 export output limit must be non-zero");
  }
  auto valid = validate_trimmed_audio(audio);
  if (!valid) return valid.error();

  auto presentation_duration = nonnegative_delta(
      audio.state_record.end_ns, audio.state_record.start_ns,
      "leading-trim AAC presentation duration");
  if (!presentation_duration) return presentation_duration.error();
  if (*presentation_duration <= 0) {
    return fail<std::vector<std::byte>>(
        ErrorCode::archive_corrupt,
        "leading-trim AAC presentation duration is empty");
  }
  auto audio_delta = nonnegative_delta(audio.state_record.start_ns,
                                       video_origin_ns,
                                       "video MP4 encoded-audio timestamp");
  if (!audio_delta) return audio_delta.error();

  auto prepared = prepare_audio(audio, *audio_delta, *presentation_duration);
  if (!prepared) return prepared.error();

  MemoryInput memory{};
  AvioPtr input_avio{};
  auto input = open_memory_mp4(video_mp4, memory, input_avio);
  if (!input) return input.error();
  const auto video_index = av_find_best_stream(
      input->get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0 ||
      static_cast<unsigned>(video_index) >= input->get()->nb_streams ||
      input->get()->streams[video_index] == nullptr ||
      input->get()->streams[video_index]->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::decode,
        "generated MP4 has no video stream for leading-trim AAC mux");
  }
  AVStream* input_video = input->get()->streams[video_index];

  AVFormatContext* raw = nullptr;
  const auto allocated =
      avformat_alloc_output_context2(&raw, nullptr, "mp4", nullptr);
  if (allocated < 0 || raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for leading-trim AAC",
        allocated < 0 ? allocated : AVERROR(ENOMEM));
  }
  OutputFormatPtr output{raw};

  AVStream* output_video = avformat_new_stream(output.get(), nullptr);
  if (output_video == nullptr || output_video->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate leading-trim MP4 video stream");
  }
  const auto copied_video =
      avcodec_parameters_copy(output_video->codecpar, input_video->codecpar);
  if (copied_video < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot copy leading-trim MP4 video parameters", copied_video);
  }
  output_video->codecpar->codec_tag = 0;
  output_video->time_base = input_video->time_base;

  AVStream* output_audio = avformat_new_stream(output.get(), nullptr);
  if (output_audio == nullptr || output_audio->codecpar == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot allocate leading-trim MP4 audio stream");
  }
  output_audio->time_base =
      AVRational{1, static_cast<int>(audio.state.sample_rate)};
  output_audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  output_audio->codecpar->codec_id = AV_CODEC_ID_AAC;
  output_audio->codecpar->codec_tag = 0;
  output_audio->codecpar->profile = audio.state.codec_profile;
  output_audio->codecpar->sample_rate =
      static_cast<int>(audio.state.sample_rate);
  output_audio->codecpar->frame_size = 1024;
  av_channel_layout_default(&output_audio->codecpar->ch_layout,
                            static_cast<int>(audio.state.channels));
  auto configured = replace_extradata(
      *output_audio->codecpar,
      std::span<const std::byte>{prepared->decoder_config.data(),
                                 prepared->decoder_config.size()});
  if (!configured) return configured.error();

  DynamicOutput dynamic{output.get()};
  auto opened = dynamic.open();
  if (!opened) return opened.error();

  AVDictionary* options = nullptr;
  const auto edit_list_set = av_dict_set(&options, "use_editlist", "1", 0);
  if (edit_list_set < 0) {
    av_dict_free(&options);
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "cannot configure MP4 edit-list handling", edit_list_set);
  }
  const auto header = avformat_write_header(output.get(), &options);
  av_dict_free(&options);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "cannot write MP4 header for leading-trim AAC", header);
  }
  auto header_bound = dynamic.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();

  auto video_written = write_video_packets(
      *input->get(), video_index, *input_video, *output, *output_video,
      dynamic, maximum_output_bytes);
  if (!video_written) return video_written.error();

  auto audio_written = write_audio_packets(
      *prepared, *output, *output_audio, dynamic, maximum_output_bytes);
  if (!audio_written) return audio_written.error();

  const auto trailer = av_write_trailer(output.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal,
        "cannot finalize MP4 with leading-trim AAC", trailer);
  }
  auto trailer_bound = dynamic.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic.close(maximum_output_bytes);
}

#endif

}  // namespace

Result<std::vector<std::byte>> mux_verified_encoded_audio_trim_aware(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoEncodedAudio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes) {
  if (audio.state.trim_start_frames == 0U) {
    return mux_verified_encoded_audio_packets(
        video_mp4, audio, video_origin_ns, maximum_output_bytes);
  }
#ifndef CODEC_HAS_FFMPEG_VIDEO
  (void)video_mp4;
  (void)audio;
  (void)video_origin_ns;
  (void)maximum_output_bytes;
  return fail<std::vector<std::byte>>(
      ErrorCode::model_incompatible,
      "FFmpeg video export backend is unavailable");
#else
  return mux_trimmed_aac(video_mp4, audio, video_origin_ns,
                         maximum_output_bytes);
#endif
}

}  // namespace codec::profiles::video
