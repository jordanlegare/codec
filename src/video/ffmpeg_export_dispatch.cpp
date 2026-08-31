#ifdef CODEC_HAS_FFMPEG_VIDEO
#include <cerrno>
#include <cstdlib>

extern "C" {
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

#define export_verified_video_mp4 export_verified_video_mp4_video_only
#include "ffmpeg_export.cpp"
#undef export_verified_video_mp4

namespace codec::profiles::video {
namespace {

#ifdef CODEC_HAS_FFMPEG_VIDEO

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
  if (target < 0 || static_cast<std::uint64_t>(target) > input.bytes.size()) {
    return AVERROR(EINVAL);
  }
  input.offset = static_cast<std::size_t>(target);
  return target;
}

struct AvioDeleter {
  void operator()(AVIOContext* value) const noexcept {
    if (value == nullptr) return;
    av_freep(&value->buffer);
    avio_context_free(&value);
  }
};

struct InputFormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    if (value == nullptr) return;
    avformat_close_input(&value);
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

using AvioPtr = std::unique_ptr<AVIOContext, AvioDeleter>;
using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;
using AudioFifoPtr = std::unique_ptr<AVAudioFifo, AudioFifoDeleter>;

int choose_aac_sample_rate(const AVCodec& codec, std::uint32_t requested) {
  if (requested == 0U || requested > static_cast<std::uint32_t>(INT_MAX)) {
    return 0;
  }
  const auto requested_int = static_cast<int>(requested);
  if (codec.supported_samplerates == nullptr) return requested_int;

  int best = 0;
  std::uint64_t best_distance = std::numeric_limits<std::uint64_t>::max();
  for (const int* rate = codec.supported_samplerates; *rate != 0; ++rate) {
    if (*rate <= 0) continue;
    if (*rate == requested_int) return requested_int;
    const auto distance = static_cast<std::uint64_t>(
        *rate > requested_int ? *rate - requested_int : requested_int - *rate);
    if (best == 0 || distance < best_distance ||
        (distance == best_distance && *rate < best)) {
      best = *rate;
      best_distance = distance;
    }
  }
  return best;
}

AVSampleFormat choose_aac_sample_format(const AVCodec& codec) {
  if (codec.sample_fmts == nullptr) return AV_SAMPLE_FMT_NONE;
  for (const AVSampleFormat* format = codec.sample_fmts;
       *format != AV_SAMPLE_FMT_NONE; ++format) {
    if (*format == AV_SAMPLE_FMT_FLTP) return *format;
  }
  return codec.sample_fmts[0];
}

Result<std::vector<std::byte>> mux_verified_audio(
    std::span<const std::byte> video_mp4,
    const VerifiedVideoPcm16Audio& audio,
    std::int64_t video_origin_ns,
    std::uint64_t maximum_output_bytes) {
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

  MemoryInput memory{video_mp4, 0U};
  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* avio_buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (avio_buffer == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate MP4 remux input buffer");
  }
  AvioPtr avio{avio_alloc_context(avio_buffer, avio_buffer_bytes, 0, &memory,
                                  &memory_read, nullptr, &memory_seek)};
  if (!avio) {
    av_free(avio_buffer);
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot create MP4 remux input context");
  }

  AVFormatContext* input_raw = avformat_alloc_context();
  if (input_raw == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate MP4 remux format context");
  }
  input_raw->pb = avio.get();
  input_raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  input_raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (input_raw->protocol_whitelist == nullptr) {
    avformat_free_context(input_raw);
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate MP4 remux protocol policy");
  }
  const auto input_opened =
      avformat_open_input(&input_raw, nullptr, nullptr, nullptr);
  if (input_opened < 0) {
    if (input_raw != nullptr) avformat_free_context(input_raw);
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::decode, "cannot reopen generated MP4 for audio mux",
        input_opened);
  }
  InputFormatPtr input{input_raw};
  const auto stream_info = avformat_find_stream_info(input.get(), nullptr);
  if (stream_info < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::decode, "cannot inspect generated MP4 for audio mux",
        stream_info);
  }
  const auto video_index =
      av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_index < 0 || static_cast<unsigned>(video_index) >= input->nb_streams ||
      input->streams[video_index] == nullptr) {
    return fail<std::vector<std::byte>>(
        ErrorCode::decode, "generated MP4 has no video stream for audio mux");
  }
  AVStream* input_video = input->streams[video_index];

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

  AVFormatContext* output_raw = nullptr;
  const auto format_result =
      avformat_alloc_output_context2(&output_raw, nullptr, "mp4", nullptr);
  if (format_result < 0 || output_raw == nullptr) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible,
        "FFmpeg MP4 muxer is unavailable for audiovisual export", format_result);
  }
  std::unique_ptr<AVFormatContext, FormatDeleter> output_format_context{output_raw};

  AVStream* output_video = avformat_new_stream(output_format_context.get(), nullptr);
  if (output_video == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate remuxed MP4 video stream");
  }
  const auto copied_video =
      avcodec_parameters_copy(output_video->codecpar, input_video->codecpar);
  if (copied_video < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot copy MP4 video stream parameters",
        copied_video);
  }
  output_video->codecpar->codec_tag = 0;
  output_video->time_base = input_video->time_base;

  AVStream* output_audio = avformat_new_stream(output_format_context.get(), nullptr);
  if (output_audio == nullptr) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate MP4 audio stream");
  }
  std::unique_ptr<AVCodecContext, CodecDeleter> audio_encoder{
      avcodec_alloc_context3(aac)};
  if (!audio_encoder) {
    return fail<std::vector<std::byte>>(ErrorCode::internal,
                                        "cannot allocate FFmpeg AAC encoder");
  }
  audio_encoder->codec_type = AVMEDIA_TYPE_AUDIO;
  audio_encoder->codec_id = aac->id;
  audio_encoder->sample_rate = output_rate;
  audio_encoder->sample_fmt = output_format;
  audio_encoder->time_base = AVRational{1, output_rate};
  audio_encoder->bit_rate = channels == 1 ? 64'000 : 128'000;
  av_channel_layout_default(&audio_encoder->ch_layout, channels);
  if ((output_format_context->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
    audio_encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  const auto audio_opened = avcodec_open2(audio_encoder.get(), aac, nullptr);
  if (audio_opened < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible, "cannot open FFmpeg AAC encoder",
        audio_opened);
  }
  output_audio->time_base = audio_encoder->time_base;
  const auto copied_audio =
      avcodec_parameters_from_context(output_audio->codecpar, audio_encoder.get());
  if (copied_audio < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot configure MP4 AAC stream parameters",
        copied_audio);
  }

  AVChannelLayout input_layout{};
  av_channel_layout_default(&input_layout, channels);
  SwrContext* swr_raw = nullptr;
  const auto swr_allocated = swr_alloc_set_opts2(
      &swr_raw, &audio_encoder->ch_layout, audio_encoder->sample_fmt,
      audio_encoder->sample_rate, &input_layout, AV_SAMPLE_FMT_S16,
      static_cast<int>(audio.state.sample_rate), 0, nullptr);
  av_channel_layout_uninit(&input_layout);
  if (swr_allocated < 0 || swr_raw == nullptr) {
    if (swr_raw != nullptr) swr_free(&swr_raw);
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible, "cannot create AAC sample converter",
        swr_allocated < 0 ? swr_allocated : AVERROR(ENOMEM));
  }
  SwrPtr swr{swr_raw};
  const auto swr_initialized = swr_init(swr.get());
  if (swr_initialized < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::model_incompatible, "cannot initialize AAC sample converter",
        swr_initialized);
  }

  const auto frame_size = audio_encoder->frame_size > 0
                              ? audio_encoder->frame_size
                              : 1024;
  AudioFifoPtr fifo{av_audio_fifo_alloc(audio_encoder->sample_fmt, channels,
                                        std::max(frame_size, 1))};
  if (!fifo) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate AAC audio FIFO");
  }

  DynamicAvio dynamic_output{output_format_context.get()};
  auto opened_output = dynamic_output.open();
  if (!opened_output) return opened_output.error();
  const auto header = avformat_write_header(output_format_context.get(), nullptr);
  if (header < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot write audiovisual MP4 header", header);
  }
  auto header_bound = dynamic_output.require_within(maximum_output_bytes);
  if (!header_bound) return header_bound.error();

  std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
  if (!packet) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "cannot allocate MP4 remux packet");
  }
  for (;;) {
    const auto read = av_read_frame(input.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      return ffmpeg_failure<std::vector<std::byte>>(
          ErrorCode::decode, "cannot read generated MP4 video packet", read);
    }
    if (packet->stream_index == video_index) {
      av_packet_rescale_ts(packet.get(), input_video->time_base,
                           output_video->time_base);
      packet->stream_index = output_video->index;
      packet->pos = -1;
      const auto written = av_write_frame(output_format_context.get(), packet.get());
      av_packet_unref(packet.get());
      if (written < 0) {
        return ffmpeg_failure<std::vector<std::byte>>(
            ErrorCode::internal, "cannot write remuxed MP4 video packet",
            written);
      }
      auto bound = dynamic_output.require_within(maximum_output_bytes);
      if (!bound) return bound.error();
    } else {
      av_packet_unref(packet.get());
    }
  }

  auto audio_delta = nonnegative_delta(audio.state_record.start_ns,
                                       video_origin_ns,
                                       "video MP4 audio timestamp");
  if (!audio_delta) return audio_delta.error();
  const auto audio_start_pts =
      av_rescale_q(*audio_delta, kNanoseconds, audio_encoder->time_base);
  std::int64_t samples_submitted = 0;

  auto receive_audio_packets = [&]() -> Result<void> {
    for (;;) {
      const auto received =
          avcodec_receive_packet(audio_encoder.get(), packet.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return {};
      if (received < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "AAC packet encode failed", received);
      }
      av_packet_rescale_ts(packet.get(), audio_encoder->time_base,
                           output_audio->time_base);
      packet->stream_index = output_audio->index;
      packet->pos = -1;
      const auto written = av_write_frame(output_format_context.get(), packet.get());
      av_packet_unref(packet.get());
      if (written < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot write MP4 AAC packet", written);
      }
      auto bound = dynamic_output.require_within(maximum_output_bytes);
      if (!bound) return bound.error();
    }
  };

  auto encode_fifo = [&](bool final) -> Result<void> {
    while (av_audio_fifo_size(fifo.get()) >= frame_size ||
           (final && av_audio_fifo_size(fifo.get()) > 0)) {
      const auto available = av_audio_fifo_size(fifo.get());
      const auto take = std::min(available, frame_size);
      std::unique_ptr<AVFrame, FrameDeleter> frame{av_frame_alloc()};
      if (!frame) {
        return fail(ErrorCode::resource_exhausted,
                    "cannot allocate AAC export frame");
      }
      frame->nb_samples = frame_size;
      frame->format = audio_encoder->sample_fmt;
      frame->sample_rate = audio_encoder->sample_rate;
      const auto layout_copied =
          av_channel_layout_copy(&frame->ch_layout, &audio_encoder->ch_layout);
      if (layout_copied < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot copy AAC channel layout",
                                    layout_copied);
      }
      const auto frame_buffer = av_frame_get_buffer(frame.get(), 0);
      if (frame_buffer < 0) {
        return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                    "cannot allocate AAC frame samples",
                                    frame_buffer);
      }
      const auto read = av_audio_fifo_read(
          fifo.get(), reinterpret_cast<void**>(frame->data), take);
      if (read != take) {
        return fail(ErrorCode::internal, "cannot drain AAC audio FIFO");
      }
      if (take < frame_size) {
        const auto silenced = av_samples_set_silence(
            frame->data, take, frame_size - take, channels,
            audio_encoder->sample_fmt);
        if (silenced < 0) {
          return ffmpeg_failure<void>(ErrorCode::internal,
                                      "cannot pad final AAC frame", silenced);
        }
      }
      frame->pts = audio_start_pts + samples_submitted;
      samples_submitted += frame_size;
      const auto sent = avcodec_send_frame(audio_encoder.get(), frame.get());
      if (sent < 0) {
        return ffmpeg_failure<void>(ErrorCode::internal,
                                    "cannot send AAC export frame", sent);
      }
      auto received = receive_audio_packets();
      if (!received) return received.error();
    }
    return {};
  };

  auto append_converted = [&](const std::uint8_t* input_data,
                              int input_frames) -> Result<void> {
    const auto output_capacity = swr_get_out_samples(swr.get(), input_frames);
    if (output_capacity < 0) {
      return ffmpeg_failure<void>(ErrorCode::internal,
                                  "cannot size AAC sample conversion",
                                  output_capacity);
    }
    if (output_capacity == 0) return {};
    std::uint8_t** converted = nullptr;
    int converted_linesize = 0;
    const auto allocated = av_samples_alloc_array_and_samples(
        &converted, &converted_linesize, channels, output_capacity,
        audio_encoder->sample_fmt, 0);
    if (allocated < 0) {
      return ffmpeg_failure<void>(ErrorCode::resource_exhausted,
                                  "cannot allocate converted AAC samples",
                                  allocated);
    }
    const std::uint8_t* inputs[1]{input_data};
    const auto converted_frames = swr_convert(
        swr.get(), converted, output_capacity,
        input_data == nullptr ? nullptr : inputs, input_frames);
    if (converted_frames < 0) {
      av_freep(&converted[0]);
      av_freep(&converted);
      return ffmpeg_failure<void>(ErrorCode::internal,
                                  "AAC sample conversion failed",
                                  converted_frames);
    }
    if (converted_frames != 0) {
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
        return fail(ErrorCode::internal, "cannot fill AAC audio FIFO");
      }
    }
    av_freep(&converted[0]);
    av_freep(&converted);
    return encode_fifo(false);
  };

  const auto total_input_frames = audio.state.frames();
  std::uint64_t input_offset = 0U;
  constexpr std::uint64_t conversion_chunk_frames = 4096U;
  while (input_offset < total_input_frames) {
    const auto count = std::min(conversion_chunk_frames,
                                total_input_frames - input_offset);
    if (count > static_cast<std::uint64_t>(INT_MAX)) {
      return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                          "AAC conversion chunk exceeds process bounds");
    }
    const auto sample_offset = input_offset * audio.state.channels;
    const auto* input_bytes = reinterpret_cast<const std::uint8_t*>(
        audio.state.samples.data() + static_cast<std::size_t>(sample_offset));
    auto converted = append_converted(input_bytes, static_cast<int>(count));
    if (!converted) return converted.error();
    input_offset += count;
  }

  for (;;) {
    const auto output_capacity = swr_get_out_samples(swr.get(), 0);
    if (output_capacity <= 0) break;
    const auto before = av_audio_fifo_size(fifo.get());
    auto flushed = append_converted(nullptr, 0);
    if (!flushed) return flushed.error();
    if (av_audio_fifo_size(fifo.get()) == before) break;
  }
  auto final_audio = encode_fifo(true);
  if (!final_audio) return final_audio.error();

  const auto encoder_flushed = avcodec_send_frame(audio_encoder.get(), nullptr);
  if (encoder_flushed < 0 && encoder_flushed != AVERROR_EOF) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot flush AAC encoder", encoder_flushed);
  }
  auto final_packets = receive_audio_packets();
  if (!final_packets) return final_packets.error();

  const auto trailer = av_write_trailer(output_format_context.get());
  if (trailer < 0) {
    return ffmpeg_failure<std::vector<std::byte>>(
        ErrorCode::internal, "cannot finalize audiovisual MP4", trailer);
  }
  auto trailer_bound = dynamic_output.require_within(maximum_output_bytes);
  if (!trailer_bound) return trailer_bound.error();
  return dynamic_output.close(maximum_output_bytes);
}

#endif

}  // namespace

Result<VerifiedVideoMp4Export> export_verified_video_mp4(
    const CodaArchive& archive, const VideoFrameQuery& query,
    VideoMp4ExportLimits limits) {
  auto video_only = export_verified_video_mp4_video_only(archive, query, limits);
  if (!video_only) return video_only.error();

  if (video_only->state_records.empty()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::internal,
        "verified MP4 export returned no video state evidence");
  }
  const auto stream = video_only->state_records.front().stream;
  auto verified_audio = query_verified_video_pcm16_audio(
      archive,
      VideoAudioQuery{
          .stream = stream,
          .time = query.time,
          .maximum_results = 1,
          .maximum_encoded_bytes = query.maximum_encoded_bytes,
      });
  if (!verified_audio) return verified_audio.error();
  if (verified_audio->empty()) return video_only;
  if (verified_audio->size() != 1U) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::archive_corrupt,
        "H.1 v1 MP4 export requires at most one verified PCM16 audio state");
  }

#ifndef CODEC_HAS_FFMPEG_VIDEO
  return fail<VerifiedVideoMp4Export>(
      ErrorCode::model_incompatible,
      "FFmpeg video export backend is unavailable");
#else
  const auto& audio = verified_audio->front();
  auto muxed = mux_verified_audio(
      video_only->output.payload, audio,
      video_only->state_records.front().start_ns, limits.maximum_output_bytes);
  if (!muxed) return muxed.error();

  video_only->output.payload = std::move(*muxed);
  video_only->output.supporting_records.push_back(ProvenanceRecordLink{
      .stream = audio.state_record.stream,
      .type = audio.state_record.type_code(),
      .sequence = audio.state_record.sequence,
      .hash = audio.state_record.hash,
  });
  video_only->audio_state_record = audio.state_record;
  video_only->audio_provenance = audio.provenance;
  return video_only;
#endif
}

}  // namespace codec::profiles::video