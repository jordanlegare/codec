#define ingest_video_ffmpeg ingest_video_ffmpeg_direct
#include "ffmpeg_ingest.cpp"
#undef ingest_video_ffmpeg

#include "hls_policy.hpp"

#include <new>

namespace codec::profiles::video {
namespace {

Result<void> validate_hls_request_limits(
    const FfmpegVideoIngestRequest& request) {
  if (request.maximum_hls_resources == 0U ||
      request.maximum_hls_resource_bytes == 0U ||
      request.maximum_hls_total_bytes == 0U) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest HLS limits must all be non-zero");
  }
  if (request.maximum_hls_resources > 999999U) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest HLS resource limit exceeds child identity bounds");
  }
  if (request.maximum_hls_resource_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
      request.maximum_hls_total_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail(ErrorCode::invalid_argument,
                "FFmpeg video ingest HLS byte limits exceed process bounds");
  }
  return {};
}

#ifdef CODEC_HAS_FFMPEG_VIDEO

struct HlsCapturedResource {
  std::string requested_uri;
  bool manifest{};
  StreamId stream;
  std::vector<std::byte> bytes;
  RecordInfo descriptor_record;
  RecordInfo source_record;
};

struct HlsCaptureSession {
  detail::HlsOrigin primary_origin;
  const FfmpegVideoIngestRequest* request{};
  CodaWriter* writer{};
  std::span<const std::byte> primary_bytes;
  std::vector<std::unique_ptr<HlsCapturedResource>> resources;
  std::uint64_t total_bytes{};
  std::optional<Error> callback_error;
  std::vector<RecordInfo>* report_descriptors{};
  std::vector<RecordInfo>* report_sources{};
};

void remember_hls_callback_error(HlsCaptureSession& session, Error error) {
  if (!session.callback_error.has_value()) {
    session.callback_error = std::move(error);
  }
}

Result<AVIOContext*> make_hls_memory_avio(std::span<const std::byte> bytes) {
  auto* input = new (std::nothrow) MemoryInput{bytes, 0U};
  if (input == nullptr) {
    return fail<AVIOContext*>(ErrorCode::resource_exhausted,
                              "cannot allocate HLS memory input state");
  }

  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (buffer == nullptr) {
    delete input;
    return fail<AVIOContext*>(ErrorCode::resource_exhausted,
                              "cannot allocate HLS FFmpeg input buffer");
  }

  auto* context = avio_alloc_context(buffer, avio_buffer_bytes, 0, input,
                                     &memory_read, nullptr, &memory_seek);
  if (context == nullptr) {
    av_free(buffer);
    delete input;
    return fail<AVIOContext*>(ErrorCode::resource_exhausted,
                              "cannot create HLS FFmpeg input context");
  }
  return context;
}

int hls_io_close2(AVFormatContext*, AVIOContext* context) {
  if (context == nullptr) return 0;
  delete static_cast<MemoryInput*>(context->opaque);
  context->opaque = nullptr;
  av_freep(&context->buffer);
  avio_context_free(&context);
  return 0;
}

int hls_io_open(AVFormatContext* context, AVIOContext** output,
                const char* url, int flags, AVDictionary**) noexcept {
  if (context == nullptr || output == nullptr || url == nullptr) {
    return AVERROR(EINVAL);
  }
  auto* session = static_cast<HlsCaptureSession*>(context->opaque);
  if (session == nullptr || session->request == nullptr ||
      session->writer == nullptr || session->report_descriptors == nullptr ||
      session->report_sources == nullptr) {
    return AVERROR(EINVAL);
  }

  try {
    if ((flags & AVIO_FLAG_WRITE) != 0 || (flags & AVIO_FLAG_READ) == 0) {
      remember_hls_callback_error(
          *session,
          Error{ErrorCode::unauthorized_source,
                "HLS secondary resources are read-only", false});
      return AVERROR(EACCES);
    }

    const std::string requested_uri{url};
    auto authorized =
        detail::require_same_hls_origin(session->primary_origin, requested_uri);
    if (!authorized) {
      remember_hls_callback_error(*session, authorized.error());
      return AVERROR(EACCES);
    }

    if (requested_uri == session->request->source_uri) {
      auto primary = make_hls_memory_avio(session->primary_bytes);
      if (!primary) {
        remember_hls_callback_error(*session, primary.error());
        return AVERROR(ENOMEM);
      }
      *output = *primary;
      return 0;
    }

    for (const auto& resource : session->resources) {
      if (resource->requested_uri == requested_uri && !resource->manifest) {
        auto cached = make_hls_memory_avio(resource->bytes);
        if (!cached) {
          remember_hls_callback_error(*session, cached.error());
          return AVERROR(ENOMEM);
        }
        *output = *cached;
        return 0;
      }
    }

    if (session->resources.size() >= session->request->maximum_hls_resources) {
      remember_hls_callback_error(
          *session,
          Error{ErrorCode::resource_exhausted,
                "HLS resource count exceeds the configured limit", false});
      return AVERROR(ENOMEM);
    }

    auto prepared = ::codec::detail::PreparedCapture::prepare(
        requested_uri,
        ::codec::detail::CaptureOptions{
            .chunk_bytes = session->request->capture_chunk_bytes,
            .maximum_bytes = session->request->maximum_hls_resource_bytes,
            .maximum_redirects = session->request->maximum_redirects,
            .deny_private_network = session->request->deny_private_network,
        });
    if (!prepared) {
      remember_hls_callback_error(*session, prepared.error());
      return AVERROR(EIO);
    }

    std::vector<std::byte> resource_bytes;
    resource_bytes.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
        session->request->maximum_hls_resource_bytes, 4U * 1024U * 1024U)));
    auto captured = prepared->run(
        [&resource_bytes, session](std::span<const std::byte> chunk)
            -> Result<void> {
          const auto chunk_bytes = static_cast<std::uint64_t>(chunk.size());
          const auto resource_size =
              static_cast<std::uint64_t>(resource_bytes.size());
          if (resource_size > session->request->maximum_hls_resource_bytes ||
              chunk_bytes >
                  session->request->maximum_hls_resource_bytes - resource_size) {
            return fail(ErrorCode::resource_exhausted,
                        "HLS resource exceeds the configured byte limit");
          }
          if (session->total_bytes > session->request->maximum_hls_total_bytes ||
              resource_size > session->request->maximum_hls_total_bytes -
                                  session->total_bytes ||
              chunk_bytes > session->request->maximum_hls_total_bytes -
                                session->total_bytes - resource_size) {
            return fail(ErrorCode::resource_exhausted,
                        "HLS resources exceed the configured aggregate byte limit");
          }
          resource_bytes.insert(resource_bytes.end(), chunk.begin(), chunk.end());
          return {};
        });
    if (!captured) {
      remember_hls_callback_error(*session, captured.error());
      return AVERROR(EIO);
    }

    const bool manifest = detail::looks_like_hls_manifest(resource_bytes);
    if (manifest) {
      auto secure = detail::validate_hls_manifest_security(resource_bytes);
      if (!secure) {
        remember_hls_callback_error(*session, secure.error());
        return AVERROR(EACCES);
      }
    }

    const auto ordinal = session->resources.size();
    const auto child_stream =
        detail::derive_hls_child_stream_id(session->request->descriptor.id, ordinal);
    const StreamDescriptor child_descriptor{
        .id = child_stream,
        .type = StreamType::opaque,
        .label = detail::hls_child_label(session->request->descriptor.label, ordinal),
        .source_id = "codec.video.hls-resource",
        .payload_type = manifest ? "application/vnd.apple.mpegurl"
                                 : "application/octet-stream",
    };
    auto descriptor_record =
        session->writer->append_stream_descriptor(child_descriptor,
                                                  session->request->start_ns);
    if (!descriptor_record) {
      remember_hls_callback_error(*session, descriptor_record.error());
      return AVERROR(EIO);
    }
    auto source_record = session->writer->append(
        RecordType::source_bytes, child_stream, session->request->start_ns,
        session->request->end_ns, resource_bytes);
    if (!source_record) {
      remember_hls_callback_error(*session, source_record.error());
      return AVERROR(EIO);
    }

    auto resource = std::make_unique<HlsCapturedResource>(HlsCapturedResource{
        .requested_uri = requested_uri,
        .manifest = manifest,
        .stream = child_stream,
        .bytes = std::move(resource_bytes),
        .descriptor_record = *descriptor_record,
        .source_record = *source_record,
    });
    session->total_bytes +=
        static_cast<std::uint64_t>(resource->bytes.size());
    session->report_descriptors->push_back(resource->descriptor_record);
    session->report_sources->push_back(resource->source_record);
    session->resources.push_back(std::move(resource));

    auto opened = make_hls_memory_avio(session->resources.back()->bytes);
    if (!opened) {
      remember_hls_callback_error(*session, opened.error());
      return AVERROR(ENOMEM);
    }
    *output = *opened;
    return 0;
  } catch (const std::bad_alloc&) {
    remember_hls_callback_error(
        *session,
        Error{ErrorCode::resource_exhausted,
              "HLS ingest exhausted process memory", false});
    return AVERROR(ENOMEM);
  } catch (...) {
    remember_hls_callback_error(
        *session,
        Error{ErrorCode::internal,
              "HLS secondary resource callback failed unexpectedly", false});
    return AVERROR(EIO);
  }
}

Error hls_decode_error(const HlsCaptureSession& session,
                       std::string message, int code) {
  if (session.callback_error.has_value()) return *session.callback_error;
  return ffmpeg_decode_error(std::move(message), code);
}

Result<DecodedVideo> decode_hls_video_bytes(
    std::span<const std::byte> source_bytes,
    const FfmpegVideoIngestRequest& request, HlsCaptureSession& session) {
  MemoryInput input{source_bytes, 0U};
  constexpr int avio_buffer_bytes = 32 * 1024;
  auto* avio_buffer = static_cast<std::uint8_t*>(av_malloc(avio_buffer_bytes));
  if (avio_buffer == nullptr) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg HLS input buffer");
  }
  AvioPtr avio{avio_alloc_context(avio_buffer, avio_buffer_bytes, 0, &input,
                                  &memory_read, nullptr, &memory_seek)};
  if (!avio) {
    av_free(avio_buffer);
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot create FFmpeg HLS input context");
  }

  const AVInputFormat* hls_format = av_find_input_format("hls");
  if (hls_format == nullptr) {
    return fail<DecodedVideo>(ErrorCode::model_incompatible,
                              "FFmpeg HLS demuxer is unavailable");
  }

  AVFormatContext* format_raw = avformat_alloc_context();
  if (format_raw == nullptr) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg HLS format context");
  }
  format_raw->pb = avio.get();
  format_raw->flags |= AVFMT_FLAG_CUSTOM_IO;
  format_raw->opaque = &session;
  format_raw->io_open = &hls_io_open;
  format_raw->io_close2 = &hls_io_close2;
  format_raw->protocol_whitelist = av_strdup("codec-memory-only");
  if (format_raw->protocol_whitelist == nullptr) {
    avformat_free_context(format_raw);
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg HLS protocol policy");
  }

  const auto opened = avformat_open_input(&format_raw, request.source_uri.c_str(),
                                          hls_format, nullptr);
  if (opened < 0) {
    if (format_raw != nullptr) avformat_free_context(format_raw);
    return hls_decode_error(session, "FFmpeg cannot open captured HLS", opened);
  }
  FormatPtr format{format_raw};

  const auto stream_info = avformat_find_stream_info(format.get(), nullptr);
  if (stream_info < 0) {
    return hls_decode_error(session, "FFmpeg cannot inspect captured HLS",
                            stream_info);
  }

  const AVCodec* decoder = nullptr;
  const auto stream_index = av_find_best_stream(
      format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if (stream_index < 0 || decoder == nullptr) {
    return hls_decode_error(
        session, "captured HLS has no decodable video stream",
        stream_index < 0 ? stream_index : AVERROR_DECODER_NOT_FOUND);
  }
  AVStream* stream = format->streams[stream_index];
  if (stream == nullptr || stream->codecpar == nullptr ||
      stream->time_base.num <= 0 || stream->time_base.den <= 0) {
    return fail<DecodedVideo>(ErrorCode::decode,
                              "selected HLS video stream has invalid metadata");
  }

  CodecPtr codec{avcodec_alloc_context3(decoder)};
  if (!codec) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg HLS decoder context");
  }
  const auto copied = avcodec_parameters_to_context(codec.get(), stream->codecpar);
  if (copied < 0) {
    return hls_decode_error(session, "FFmpeg cannot copy HLS codec parameters",
                            copied);
  }
  codec->thread_count = 1;
  codec->max_pixels =
      static_cast<std::int64_t>(VideoDecodeLimits{}.maximum_pixels);
  const auto decoder_opened = avcodec_open2(codec.get(), decoder, nullptr);
  if (decoder_opened < 0) {
    return hls_decode_error(session, "FFmpeg cannot open HLS video decoder",
                            decoder_opened);
  }

  PacketPtr packet{av_packet_alloc()};
  FramePtr frame{av_frame_alloc()};
  if (!packet || !frame) {
    return fail<DecodedVideo>(ErrorCode::resource_exhausted,
                              "cannot allocate FFmpeg HLS decode buffers");
  }

  DecodedVideo decoded;
  decoded.frames.reserve(std::min<std::size_t>(request.maximum_frames, 64U));
  decoded.time_base = stream->time_base;
  decoded.frame_rate = usable_frame_rate(*stream);
  std::uint64_t decoded_bytes = 0U;

  const auto accept_frame = [&]() -> Result<void> {
    if (decoded.frames.size() >= request.maximum_frames) {
      return fail(ErrorCode::resource_exhausted,
                  "decoded HLS exceeds the configured frame limit");
    }
    auto state = canonicalize_frame(*frame, *stream, request.output_layout);
    if (!state) return state.error();
    const auto frame_bytes = static_cast<std::uint64_t>(state->pixels.size());
    if (frame_bytes > request.maximum_decoded_bytes - decoded_bytes) {
      return fail(ErrorCode::resource_exhausted,
                  "decoded HLS exceeds the configured aggregate byte limit");
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
        return hls_decode_error(session, "FFmpeg HLS frame decode failed",
                                received);
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
      return hls_decode_error(session, "FFmpeg HLS packet demux failed", read);
    }
    if (packet->stream_index == stream_index) {
      const auto sent = avcodec_send_packet(codec.get(), packet.get());
      av_packet_unref(packet.get());
      if (sent < 0) {
        return hls_decode_error(session, "FFmpeg HLS packet decode failed", sent);
      }
      auto received = receive_available();
      if (!received) return received.error();
    } else {
      av_packet_unref(packet.get());
    }
  }

  const auto flushed = avcodec_send_packet(codec.get(), nullptr);
  if (flushed < 0 && flushed != AVERROR_EOF) {
    return hls_decode_error(session, "FFmpeg HLS decoder flush failed", flushed);
  }
  auto received = receive_available();
  if (!received) return received.error();
  if (decoded.frames.empty()) {
    return fail<DecodedVideo>(ErrorCode::decode,
                              "captured HLS produced no decoded video frames");
  }
  return decoded;
}

#endif

}  // namespace

Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request) {
  auto hls_limits = validate_hls_request_limits(request);
  if (!hls_limits) return hls_limits.error();
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
      [&source_bytes, &request](std::span<const std::byte> chunk) -> Result<void> {
        if (chunk.size() > request.maximum_source_bytes - source_bytes.size()) {
          return fail(ErrorCode::resource_exhausted,
                      "FFmpeg video ingest exceeded its source byte limit");
        }
        source_bytes.insert(source_bytes.end(), chunk.begin(), chunk.end());
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
      .secondary_descriptors = {},
      .secondary_sources = {},
      .profile_error = std::nullopt,
  };
  const auto finish_profile_error =
      [&writer, &report](Error error) -> Result<FfmpegVideoIngestReport> {
    auto finalized = writer.finalize();
    if (!finalized) return finalized.error();
    report.profile_error = std::move(error);
    return report;
  };

  const bool hls = detail::looks_like_hls_manifest(source_bytes);
  auto decoded = [&]() -> Result<DecodedVideo> {
    if (!hls) return decode_video_bytes(source_bytes, request);

    auto secure = detail::validate_hls_manifest_security(source_bytes);
    if (!secure) return secure.error();
    auto origin = detail::parse_hls_http_origin(request.source_uri);
    if (!origin) return origin.error();

    HlsCaptureSession session{
        .primary_origin = std::move(*origin),
        .request = &request,
        .writer = &writer,
        .primary_bytes = source_bytes,
        .resources = {},
        .total_bytes = 0U,
        .callback_error = std::nullopt,
        .report_descriptors = &report.secondary_descriptors,
        .report_sources = &report.secondary_sources,
    };
    return decode_hls_video_bytes(source_bytes, request, session);
  }();
  if (!decoded) return finish_profile_error(decoded.error());

  auto intervals = map_frame_times(*decoded, request);
  if (!intervals) return finish_profile_error(intervals.error());
  if (intervals->size() != decoded->frames.size()) {
    return finish_profile_error(
        Error{ErrorCode::internal, "video frame timing count mismatch", false});
  }

  const auto configuration_hash = ffmpeg_configuration_hash(request.output_layout);
  const auto created_utc_ns = provenance_created_utc_ns();
  const std::array inputs{*source};
  report.states.reserve(decoded->frames.size());
  report.provenance.reserve(decoded->frames.size());
  for (std::size_t index = 0; index < decoded->frames.size(); ++index) {
    auto encoded = encode_raw_video_frame_state(decoded->frames[index].state);
    if (!encoded) return finish_profile_error(encoded.error());
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
