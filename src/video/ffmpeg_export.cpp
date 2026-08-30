#include <codec/profiles/video_export.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
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
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace codec::profiles::video {

#ifdef CODEC_HAS_FFMPEG_VIDEO
namespace {

constexpr AVRational kNanoseconds{1, 1'000'000'000};
constexpr AVRational kVideoTimeBase{1, 60'000};

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

Result<std::int64_t> nonnegative_delta(std::int64_t value,
                                       std::int64_t origin,
                                       std::string_view label) {
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

AVPixelFormat source_pixel_format(PixelLayout layout) {
  switch (layout) {
    case PixelLayout::gray8: return AV_PIX_FMT_GRAY8;
    case PixelLayout::rgb24: return AV_PIX_FMT_RGB24;
    case PixelLayout::rgba32: return AV_PIX_FMT_RGBA;
    case PixelLayout::yuv420p8: return AV_PIX_FMT_YUV420P;
  }
  return AV_PIX_FMT_NONE;
}

void source_planes(const RawVideoFrameState& state,
                   const std::uint8_t* (&data)[4], int (&linesize)[4]) {
  const auto* base = reinterpret_cast<const std::uint8_t*>(
      state.pixels.data());
  const auto width = static_cast<std::size_t>(state.descriptor.coded_width);
  const auto height = static_cast<std::size_t>(state.descriptor.coded_height);
  data[0] = base;
  switch (state.descriptor.pixel_layout) {
    case PixelLayout::gray8:
      linesize[0] = static_cast<int>(width);
      return;
    case PixelLayout::rgb24:
      linesize[0] = static_cast<int>(width * 3U);
      return;
    case PixelLayout::rgba32:
      linesize[0] = static_cast<int>(width * 4U);
      return;
    case PixelLayout::yuv420p8: {
      const auto y_bytes = width * height;
      const auto chroma_bytes = (width / 2U) * (height / 2U);
      data[1] = base + y_bytes;
      data[2] = base + y_bytes + chroma_bytes;
      linesize[0] = static_cast<int>(width);
      linesize[1] = static_cast<int>(width / 2U);
      linesize[2] = static_cast<int>(width / 2U);
      return;
    }
  }
}

struct FormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    avformat_free_context(value);
  }
};

struct CodecDeleter {
  void operator()(AVCodecContext* value) const noexcept {
    avcodec_free_context(&value);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* value) const noexcept { av_frame_free(&value); }
};

struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept { av_packet_free(&value); }
};

struct SwsDeleter {
  void operator()(SwsContext* value) const noexcept { sws_freeContext(value); }
};

class DynamicAvio final {
 public:
  explicit DynamicAvio(AVFormatContext* format) : format_(format) {}
  DynamicAvio(const DynamicAvio&) = delete;
  DynamicAvio& operator=(const DynamicAvio&) = delete;

  ~DynamicAvio() {
    if (!active_ || format_ == nullptr || format_->pb == nullptr) return;
    std::uint8_t* buffer = nullptr;
    (void)avio_close_dyn_buf(format_->pb, &buffer);
    format_->pb = nullptr;
    av_free(buffer);
  }

  Result<void> open() {
    const auto result = avio_open_dyn_buf(&format_->pb);
    if (result < 0) {
      return ffmpeg_failure<void>(ErrorCode::internal,
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
    const auto byte_count = static_cast<std::uint64_t>(size);
    if (byte_count > maximum_bytes) {
      av_free(buffer);
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "verified video MP4 export exceeds the configured output limit");
    }
    std::vector<std::byte> output(static_cast<std::size_t>(size));
    if (size != 0) {
      std::memcpy(output.data(), buffer, static_cast<std::size_t>(size));
    }
    av_free(buffer);
    return output;
  }

 private:
  AVFormatContext* format_{};
  bool active_{false};
};

class FfmpegMp4Exporter final : public StreamExporter {
 public:
  explicit FfmpegMp4Exporter(std::uint64_t maximum_output_bytes)
      : maximum_output_bytes_(maximum_output_bytes) {}

  std::string name() const override { return "video-vfr1-mp4"; }

  Result<ExporterOutput> export_records(
      std::span<const ExtractedRecord> inputs) override {
    if (inputs.empty()) {
      return fail<ExporterOutput>(ErrorCode::invalid_argument,
                                  "video MP4 export requires verified VFR1 frames");
    }

    std::vector<RawVideoFrameState> states;
    states.reserve(inputs.size());
    for (const auto& input : inputs) {
      if (input.record.type_code() != raw_video_frame_state_record_type) {
        return fail<ExporterOutput>(
            ErrorCode::invalid_argument,
            "video MP4 export accepts only VFR1 state records");
      }
      auto state = decode_raw_video_frame_state(input.payload);
      if (!state) return state.error();
      states.push_back(std::move(*state));
    }

    const auto descriptor = states.front().descriptor;
    for (const auto& state : states) {
      if (!(state.descriptor == descriptor)) {
        return fail<ExporterOutput>(
            ErrorCode::invalid_argument,
            "video MP4 export requires one consistent frame descriptor");
      }
    }

    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const auto& record = inputs[index].record;
      if (record.end_ns <= record.start_ns) {
        return fail<ExporterOutput>(ErrorCode::invalid_argument,
                                    "video MP4 frame interval must be positive");
      }
      if (index != 0 &&
          record.start_ns <= inputs[index - 1].record.start_ns) {
        return fail<ExporterOutput>(
            ErrorCode::invalid_argument,
            "video MP4 frame timestamps must be strictly increasing");
      }
    }

    const auto source_format = source_pixel_format(descriptor.pixel_layout);
    if (source_format == AV_PIX_FMT_NONE) {
      return fail<ExporterOutput>(ErrorCode::model_incompatible,
                                  "video MP4 export pixel layout is unsupported");
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (codec == nullptr) {
      return fail<ExporterOutput>(ErrorCode::model_incompatible,
                                  "FFmpeg MPEG-4 video encoder is unavailable");
    }

    AVFormatContext* format_raw = nullptr;
    const auto format_result =
        avformat_alloc_output_context2(&format_raw, nullptr, "mp4", nullptr);
    if (format_result < 0 || format_raw == nullptr) {
      return ffmpeg_failure<ExporterOutput>(
          ErrorCode::model_incompatible,
          "FFmpeg MP4 muxer is unavailable", format_result);
    }
    std::unique_ptr<AVFormatContext, FormatDeleter> format{format_raw};

    AVStream* stream = avformat_new_stream(format.get(), nullptr);
    if (stream == nullptr) {
      return fail<ExporterOutput>(ErrorCode::internal,
                                  "cannot allocate MP4 video stream");
    }

    std::unique_ptr<AVCodecContext, CodecDeleter> encoder{
        avcodec_alloc_context3(codec)};
    if (!encoder) {
      return fail<ExporterOutput>(ErrorCode::internal,
                                  "cannot allocate FFmpeg video encoder");
    }
    encoder->codec_type = AVMEDIA_TYPE_VIDEO;
    encoder->codec_id = codec->id;
    encoder->width = static_cast<int>(descriptor.coded_width);
    encoder->height = static_cast<int>(descriptor.coded_height);
    encoder->pix_fmt = AV_PIX_FMT_YUV420P;
    encoder->time_base = kVideoTimeBase;
    encoder->framerate = descriptor.nominal_frame_rate_numerator == 0
                             ? AVRational{25, 1}
                             : AVRational{
                                   static_cast<int>(
                                       descriptor.nominal_frame_rate_numerator),
                                   static_cast<int>(
                                       descriptor.nominal_frame_rate_denominator)};
    encoder->gop_size = 12;
    encoder->max_b_frames = 0;
    const auto pixels = static_cast<std::int64_t>(descriptor.coded_width) *
                        descriptor.coded_height;
    encoder->bit_rate = std::clamp<std::int64_t>(pixels * 64, 250'000,
                                                  20'000'000);
    if ((format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
      encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    const auto open_result = avcodec_open2(encoder.get(), codec, nullptr);
    if (open_result < 0) {
      return ffmpeg_failure<ExporterOutput>(
          ErrorCode::model_incompatible,
          "cannot open FFmpeg MPEG-4 video encoder", open_result);
    }
    stream->time_base = encoder->time_base;
    const auto parameters_result =
        avcodec_parameters_from_context(stream->codecpar, encoder.get());
    if (parameters_result < 0) {
      return ffmpeg_failure<ExporterOutput>(
          ErrorCode::internal, "cannot configure MP4 stream parameters",
          parameters_result);
    }

    DynamicAvio output{format.get()};
    auto opened_output = output.open();
    if (!opened_output) return opened_output.error();
    const auto header_result = avformat_write_header(format.get(), nullptr);
    if (header_result < 0) {
      return ffmpeg_failure<ExporterOutput>(ErrorCode::internal,
                                            "cannot write MP4 header",
                                            header_result);
    }
    auto header_bound = output.require_within(maximum_output_bytes_);
    if (!header_bound) return header_bound.error();

    std::unique_ptr<SwsContext, SwsDeleter> scaler{sws_getContext(
        encoder->width, encoder->height, source_format, encoder->width,
        encoder->height, encoder->pix_fmt, SWS_BILINEAR, nullptr, nullptr,
        nullptr)};
    if (!scaler) {
      return fail<ExporterOutput>(ErrorCode::model_incompatible,
                                  "cannot create FFmpeg pixel converter");
    }

    std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
    std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
    if (!frame || !packet) {
      return fail<ExporterOutput>(ErrorCode::internal,
                                  "cannot allocate FFmpeg export frame/packet");
    }
    frame->format = encoder->pix_fmt;
    frame->width = encoder->width;
    frame->height = encoder->height;
    const auto buffer_result = av_frame_get_buffer(frame.get(), 32);
    if (buffer_result < 0) {
      return ffmpeg_failure<ExporterOutput>(
          ErrorCode::internal, "cannot allocate FFmpeg export frame buffer",
          buffer_result);
    }

    std::deque<std::int64_t> pending_durations;
    std::size_t packets_written = 0;

    auto receive_packets = [&]() -> Result<void> {
      while (true) {
        const auto receive_result =
            avcodec_receive_packet(encoder.get(), packet.get());
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
          return {};
        }
        if (receive_result < 0) {
          return ffmpeg_failure<void>(ErrorCode::internal,
                                      "cannot receive encoded video packet",
                                      receive_result);
        }
        if (pending_durations.empty()) {
          av_packet_unref(packet.get());
          return fail(ErrorCode::internal,
                      "FFmpeg emitted more video packets than input frames");
        }
        av_packet_rescale_ts(packet.get(), encoder->time_base,
                             stream->time_base);
        packet->duration = pending_durations.front();
        pending_durations.pop_front();
        packet->stream_index = stream->index;
        const auto write_result =
            av_interleaved_write_frame(format.get(), packet.get());
        av_packet_unref(packet.get());
        if (write_result < 0) {
          return ffmpeg_failure<void>(ErrorCode::internal,
                                      "cannot write encoded MP4 packet",
                                      write_result);
        }
        ++packets_written;
        auto bound = output.require_within(maximum_output_bytes_);
        if (!bound) return bound.error();
      }
    };

    const auto origin_ns = inputs.front().record.start_ns;
    std::int64_t previous_pts = -1;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
      const auto& state = states[index];
      const auto& record = inputs[index].record;
      auto relative_start = nonnegative_delta(record.start_ns, origin_ns,
                                              "video MP4 frame timestamp");
      if (!relative_start) return relative_start.error();
      auto frame_duration = nonnegative_delta(record.end_ns, record.start_ns,
                                              "video MP4 frame duration");
      if (!frame_duration) return frame_duration.error();
      const auto pts =
          av_rescale_q(*relative_start, kNanoseconds, encoder->time_base);
      const auto duration = std::max<std::int64_t>(
          1, av_rescale_q(*frame_duration, kNanoseconds, stream->time_base));
      if (index != 0 && pts <= previous_pts) {
        return fail<ExporterOutput>(
            ErrorCode::invalid_argument,
            "video MP4 frame timestamps collapse at the export time base");
      }
      previous_pts = pts;

      const auto writable_result = av_frame_make_writable(frame.get());
      if (writable_result < 0) {
        return ffmpeg_failure<ExporterOutput>(
            ErrorCode::internal, "cannot make FFmpeg export frame writable",
            writable_result);
      }
      const std::uint8_t* source_data[4]{};
      int source_linesize[4]{};
      source_planes(state, source_data, source_linesize);
      const auto scaled = sws_scale(
          scaler.get(), source_data, source_linesize, 0, encoder->height,
          frame->data, frame->linesize);
      if (scaled != encoder->height) {
        return fail<ExporterOutput>(ErrorCode::internal,
                                    "FFmpeg pixel conversion was incomplete");
      }
      frame->pts = pts;
      pending_durations.push_back(duration);
      const auto send_result = avcodec_send_frame(encoder.get(), frame.get());
      if (send_result < 0) {
        pending_durations.pop_back();
        return ffmpeg_failure<ExporterOutput>(ErrorCode::internal,
                                              "cannot encode video frame",
                                              send_result);
      }
      auto received = receive_packets();
      if (!received) return received.error();
    }

    const auto flush_result = avcodec_send_frame(encoder.get(), nullptr);
    if (flush_result < 0 && flush_result != AVERROR_EOF) {
      return ffmpeg_failure<ExporterOutput>(ErrorCode::internal,
                                            "cannot flush video encoder",
                                            flush_result);
    }
    auto flushed = receive_packets();
    if (!flushed) return flushed.error();
    if (!pending_durations.empty() || packets_written != inputs.size()) {
      return fail<ExporterOutput>(
          ErrorCode::internal,
          "FFmpeg video packet count does not match verified frame count");
    }

    const auto trailer_result = av_write_trailer(format.get());
    if (trailer_result < 0) {
      return ffmpeg_failure<ExporterOutput>(ErrorCode::internal,
                                            "cannot write MP4 trailer",
                                            trailer_result);
    }
    auto payload = output.close(maximum_output_bytes_);
    if (!payload) return payload.error();
    return ExporterOutput{
        .payload_type = "video/mp4",
        .payload = std::move(*payload),
    };
  }

 private:
  std::uint64_t maximum_output_bytes_{};
};

}  // namespace
#endif

bool ffmpeg_video_export_available() noexcept {
#ifdef CODEC_HAS_FFMPEG_VIDEO
  return avcodec_find_encoder(AV_CODEC_ID_MPEG4) != nullptr &&
         av_guess_format("mp4", nullptr, nullptr) != nullptr;
#else
  return false;
#endif
}

Result<VerifiedVideoMp4Export> export_verified_video_mp4(
    const CodaArchive& archive, const VideoFrameQuery& query,
    VideoMp4ExportLimits limits) {
  if (limits.maximum_output_bytes == 0) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::invalid_argument,
        "video MP4 export output limit must be non-zero");
  }

  auto verified = query_verified_raw_video_frames(archive, query);
  if (!verified) return verified.error();
  if (verified->empty()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::invalid_argument,
        "video MP4 export found no provenance-verified VFR1 frames");
  }

#ifndef CODEC_HAS_FFMPEG_VIDEO
  return fail<VerifiedVideoMp4Export>(
      ErrorCode::model_incompatible,
      "FFmpeg video export backend is unavailable");
#else
  if (!ffmpeg_video_export_available()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::model_incompatible,
        "FFmpeg MPEG-4/MP4 export backend is unavailable");
  }

  std::vector<ExtractedRecord> inputs;
  std::vector<RecordInfo> state_records;
  std::vector<StreamProvenance> provenance;
  inputs.reserve(verified->size());
  state_records.reserve(verified->size());
  provenance.reserve(verified->size());
  for (const auto& frame : *verified) {
    auto payload = archive.read_payload(frame.state_record);
    if (!payload) return payload.error();
    inputs.push_back(ExtractedRecord{
        .record = frame.state_record,
        .payload = std::move(*payload),
    });
    state_records.push_back(frame.state_record);
    provenance.push_back(frame.provenance);
  }

  FfmpegMp4Exporter exporter{limits.maximum_output_bytes};
  auto output = invoke_exporter(
      exporter, inputs,
      ExporterRunLimits{
          .maximum_inputs = query.maximum_results,
          .maximum_input_bytes = query.maximum_encoded_bytes,
          .maximum_output_bytes = limits.maximum_output_bytes,
          .maximum_payload_type_bytes = 32,
      });
  if (!output) return output.error();

  return VerifiedVideoMp4Export{
      .output = std::move(*output),
      .state_records = std::move(state_records),
      .provenance = std::move(provenance),
  };
#endif
}

}  // namespace codec::profiles::video
