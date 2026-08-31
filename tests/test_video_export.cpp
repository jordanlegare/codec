#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video_export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO
#include <cerrno>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}
#endif

namespace video = codec::profiles::video;

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-export-" + std::string{name});
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> output;
  output.reserve(text.size());
  for (const unsigned char ch : text) {
    output.push_back(static_cast<std::byte>(ch));
  }
  return output;
}

video::RawVideoFrameState frame(std::uint8_t luma) {
  video::RawVideoFrameState output{
      .descriptor = video::VideoProfileDescriptor{
          .coded_width = 16,
          .coded_height = 16,
          .pixel_layout = video::PixelLayout::yuv420p8,
          .sample_aspect_ratio_numerator = 1,
          .sample_aspect_ratio_denominator = 1,
          .nominal_frame_rate_numerator = 25,
          .nominal_frame_rate_denominator = 1,
          .color_range = video::ColorRange::limited,
          .color_primaries = video::ColorPrimaries::bt709,
          .transfer = video::TransferCharacteristics::bt709,
          .matrix = video::MatrixCoefficients::bt709,
      },
      .pixels = {},
  };
  output.pixels.assign(16U * 16U, static_cast<std::byte>(luma));
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  return output;
}

codec::ProvenanceProcess video_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.raw-frame.canonicalize",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess pcm_audio_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.pcm16.canonicalize",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.audio-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess encoded_audio_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.encoded-audio.preserve",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.encoded-audio.v1",
      .details = {std::byte{0x01}},
  };
}

struct ExportFixture {
  std::filesystem::path path;
  codec::StreamId stream;
  std::optional<codec::RecordInfo> audio_state;
  std::optional<codec::RecordInfo> encoded_audio_state;
};

ExportFixture make_export_archive(
    std::string_view name, bool provenanced = true, bool with_audio = false,
    std::int64_t audio_start_ns = 20'000'000,
    std::int64_t audio_duration_ns = 100'000'000,
    bool with_encoded_audio = false,
    std::uint64_t encoded_trim_start_frames = 0) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(name);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "export fixture",
          .source_id = "fixture",
          .payload_type = "application/octet-stream",
      },
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              140'000'000, bytes("encoded source"));
  EXPECT_TRUE(source);

  const std::array starts{0LL, 40'000'000LL, 100'000'000LL};
  const std::array ends{40'000'000LL, 100'000'000LL, 140'000'000LL};
  const std::array lumas{std::uint8_t{0x20}, std::uint8_t{0x80},
                         std::uint8_t{0xd0}};
  for (std::size_t index = 0; index < starts.size(); ++index) {
    auto encoded = video::encode_raw_video_frame_state(frame(lumas[index]));
    EXPECT_TRUE(encoded);
    auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                   stream, starts[index], ends[index], *encoded);
    EXPECT_TRUE(state);
    if (provenanced) {
      const std::array inputs{*source};
      EXPECT_TRUE(writer.append_stream_provenance(
          *state, codec::TruthClass::state_exact, inputs, video_process()));
    }
  }

  std::optional<codec::RecordInfo> audio_state_record;
  if (with_audio) {
    codec::Pcm16State pcm{
        .sample_rate = 8000,
        .channels = 1,
        .samples = std::vector<std::int16_t>(800U, std::int16_t{1000}),
    };
    auto encoded_audio = codec::encode_pcm16_state(pcm);
    EXPECT_TRUE(encoded_audio);
    auto state = writer.append_raw(
        video::video_pcm16_audio_state_record_type, stream, audio_start_ns,
        audio_start_ns + audio_duration_ns, *encoded_audio);
    EXPECT_TRUE(state);
    if (state) audio_state_record = *state;
    if (state && provenanced) {
      const std::array inputs{*source};
      EXPECT_TRUE(writer.append_stream_provenance(
          *state, codec::TruthClass::state_exact, inputs,
          pcm_audio_process()));
    }
  }

  std::optional<codec::RecordInfo> encoded_audio_state_record;
  if (with_encoded_audio) {
    const video::EncodedAudioState encoded_state{
        .codec = video::EncodedAudioCodec::aac,
        .codec_profile = 1,
        .sample_rate = 8'000,
        .channels = 1,
        .decoded_frames = 1'024,
        .trim_start_frames = encoded_trim_start_frames,
        .presentation_frames = 800,
        .decoder_config = {std::byte{0x15}, std::byte{0x88}},
        .packets = {video::EncodedAudioPacket{
            .pts_offset_ns = 0,
            .dts_offset_ns = 0,
            .duration_ns = 128'000'000,
            .flags = 1,
            .payload = {std::byte{0x01}},
        }},
    };
    auto encoded_audio = video::encode_encoded_audio_state(encoded_state);
    EXPECT_TRUE(encoded_audio);
    auto state = writer.append_raw(
        video::video_encoded_audio_state_record_type, stream, audio_start_ns,
        audio_start_ns + audio_duration_ns, *encoded_audio);
    EXPECT_TRUE(state);
    if (state) encoded_audio_state_record = *state;
    if (state && provenanced) {
      const std::array inputs{*source};
      EXPECT_TRUE(writer.append_stream_provenance(
          *state, codec::TruthClass::state_exact, inputs,
          encoded_audio_process()));
    }
  }

  EXPECT_TRUE(writer.finalize());
  return ExportFixture{path, stream, audio_state_record,
                       encoded_audio_state_record};
}

#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO

std::vector<std::byte> decode_base64(std::string_view encoded) {
  const auto value = [](char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
  };
  std::vector<std::byte> output;
  output.reserve((encoded.size() * 3U) / 4U);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const char ch : encoded) {
    if (ch == '=') break;
    const int digit = value(ch);
    if (digit < 0) continue;
    accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(digit);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      output.push_back(static_cast<std::byte>(
          (accumulator >> static_cast<unsigned>(bits)) & 0xffU));
    }
  }
  return output;
}

std::vector<std::byte> encoded_media_fixture(std::string_view name) {
  const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" /
                    std::string{name};
  std::ifstream input(path, std::ios::binary);
  const std::string encoded((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  return decode_base64(encoded);
}

bool write_fixture(const std::filesystem::path& path,
                   std::span<const std::byte> payload) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
  return static_cast<bool>(output);
}

#endif

bool has_ftyp(std::span<const std::byte> payload) {
  return payload.size() >= 8 && payload[4] == std::byte{'f'} &&
         payload[5] == std::byte{'t'} && payload[6] == std::byte{'y'} &&
         payload[7] == std::byte{'p'};
}

bool contains_ascii(std::span<const std::byte> payload,
                    std::string_view needle) {
  if (needle.empty() || needle.size() > payload.size()) return false;
  for (std::size_t offset = 0; offset + needle.size() <= payload.size(); ++offset) {
    bool match = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      if (payload[offset + index] !=
          static_cast<std::byte>(static_cast<unsigned char>(needle[index]))) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO

struct TempOutputFile {
  std::filesystem::path path;
  ~TempOutputFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

struct InputFormatDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    if (value != nullptr) avformat_close_input(&value);
  }
};

struct DecoderDeleter {
  void operator()(AVCodecContext* value) const noexcept {
    if (value != nullptr) avcodec_free_context(&value);
  }
};

struct PacketDeleter {
  void operator()(AVPacket* value) const noexcept {
    if (value != nullptr) av_packet_free(&value);
  }
};

struct FrameDeleter {
  void operator()(AVFrame* value) const noexcept {
    if (value != nullptr) av_frame_free(&value);
  }
};

struct Mp4Inspection {
  std::size_t video_streams{};
  std::size_t audio_streams{};
  std::size_t audio_packets{};
  std::uint64_t decoded_audio_samples{};
  std::optional<std::int64_t> first_audio_pts_ns;
  int audio_sample_rate{};
  std::vector<std::vector<std::byte>> audio_payloads;
};

Mp4Inspection inspect_mp4(std::span<const std::byte> payload,
                          std::string_view name) {
  TempOutputFile output_file{test_path(std::string{name} + ".mp4")};
  {
    std::ofstream output(output_file.path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create MP4 inspection file");
    output.write(reinterpret_cast<const char*>(payload.data()),
                 static_cast<std::streamsize>(payload.size()));
    if (!output) throw std::runtime_error("cannot write MP4 inspection file");
  }

  AVFormatContext* raw_format = nullptr;
  const auto path_text = output_file.path.string();
  const auto opened =
      avformat_open_input(&raw_format, path_text.c_str(), nullptr, nullptr);
  if (opened < 0 || raw_format == nullptr) {
    if (raw_format != nullptr) avformat_close_input(&raw_format);
    throw std::runtime_error("cannot open exported MP4 for inspection");
  }
  std::unique_ptr<AVFormatContext, InputFormatDeleter> format{raw_format};
  if (avformat_find_stream_info(format.get(), nullptr) < 0) {
    throw std::runtime_error("cannot read exported MP4 stream info");
  }

  Mp4Inspection inspection;
  int audio_index = -1;
  for (unsigned index = 0; index < format->nb_streams; ++index) {
    const auto* stream = format->streams[index];
    if (stream == nullptr || stream->codecpar == nullptr) continue;
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      ++inspection.video_streams;
    } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      ++inspection.audio_streams;
      if (audio_index < 0) audio_index = static_cast<int>(index);
    }
  }
  if (audio_index < 0) return inspection;

  AVStream* audio_stream = format->streams[audio_index];
  inspection.audio_sample_rate = audio_stream->codecpar->sample_rate;
  const AVCodec* decoder = avcodec_find_decoder(audio_stream->codecpar->codec_id);
  if (decoder == nullptr) {
    throw std::runtime_error("cannot find decoder for exported MP4 audio");
  }
  std::unique_ptr<AVCodecContext, DecoderDeleter> decoder_context{
      avcodec_alloc_context3(decoder)};
  if (!decoder_context) {
    throw std::runtime_error("cannot allocate exported MP4 audio decoder");
  }
  if (avcodec_parameters_to_context(decoder_context.get(),
                                    audio_stream->codecpar) < 0 ||
      avcodec_open2(decoder_context.get(), decoder, nullptr) < 0) {
    throw std::runtime_error("cannot open exported MP4 audio decoder");
  }

  std::unique_ptr<AVPacket, PacketDeleter> packet{av_packet_alloc()};
  std::unique_ptr<AVFrame, FrameDeleter> decoded{av_frame_alloc()};
  if (!packet || !decoded) {
    throw std::runtime_error("cannot allocate exported MP4 decode buffers");
  }

  auto receive_frames = [&]() {
    for (;;) {
      const auto received =
          avcodec_receive_frame(decoder_context.get(), decoded.get());
      if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return;
      if (received < 0) {
        throw std::runtime_error("cannot decode exported MP4 audio frame");
      }
      if (decoded->nb_samples > 0) {
        inspection.decoded_audio_samples +=
            static_cast<std::uint64_t>(decoded->nb_samples);
      }
      av_frame_unref(decoded.get());
    }
  };

  for (;;) {
    const auto read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) {
      throw std::runtime_error("cannot read exported MP4 packet");
    }
    if (packet->stream_index != audio_index) {
      av_packet_unref(packet.get());
      continue;
    }

    ++inspection.audio_packets;
    if (packet->data != nullptr && packet->size > 0) {
      const auto* packet_bytes =
          reinterpret_cast<const std::byte*>(packet->data);
      inspection.audio_payloads.emplace_back(packet_bytes,
                                             packet_bytes + packet->size);
    } else {
      inspection.audio_payloads.emplace_back();
    }
    if (!inspection.first_audio_pts_ns.has_value() &&
        packet->pts != AV_NOPTS_VALUE) {
      inspection.first_audio_pts_ns =
          av_rescale_q(packet->pts, audio_stream->time_base,
                       AVRational{1, 1'000'000'000});
    }

    auto sent = avcodec_send_packet(decoder_context.get(), packet.get());
    if (sent == AVERROR(EAGAIN)) {
      receive_frames();
      sent = avcodec_send_packet(decoder_context.get(), packet.get());
    }
    av_packet_unref(packet.get());
    if (sent < 0) {
      throw std::runtime_error("cannot submit exported MP4 audio packet");
    }
    receive_frames();
  }

  const auto flushed = avcodec_send_packet(decoder_context.get(), nullptr);
  if (flushed < 0 && flushed != AVERROR_EOF) {
    throw std::runtime_error("cannot flush exported MP4 audio decoder");
  }
  receive_frames();
  return inspection;
}

std::uint64_t absolute_difference(std::int64_t left, std::int64_t right) {
  return left >= right ? static_cast<std::uint64_t>(left - right)
                       : static_cast<std::uint64_t>(right - left);
}

#endif

}  // namespace

TEST(video_export_verified_vfr1_to_mp4) {
  const auto fixture = make_export_archive("verified.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  const video::VideoFrameQuery query{
      .stream = fixture.stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  auto exported = video::export_verified_video_mp4(
      *archive, query,
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  if (!video::ffmpeg_video_export_available()) {
    EXPECT_FALSE(exported);
    if (!exported) {
      EXPECT_EQ(exported.error().code, codec::ErrorCode::model_incompatible);
    }
    std::filesystem::remove(fixture.path);
    return;
  }
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_EQ(exported->output.payload_type, std::string{"video/mp4"});
    EXPECT_TRUE(has_ftyp(exported->output.payload));
    EXPECT_TRUE(contains_ascii(exported->output.payload, "vide"));
    EXPECT_EQ(exported->state_records.size(), std::size_t{3});
    EXPECT_EQ(exported->provenance.size(), std::size_t{3});
    EXPECT_EQ(exported->output.supporting_records.size(), std::size_t{3});
    EXPECT_FALSE(exported->audio_state_record.has_value());
    EXPECT_FALSE(exported->audio_provenance.has_value());
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_muxes_verified_pcm16_as_aac) {
  const auto fixture = make_export_archive("verified-audio.coda", true, true);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = fixture.stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  if (!video::ffmpeg_video_export_available()) {
    EXPECT_FALSE(exported);
    std::filesystem::remove(fixture.path);
    return;
  }
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_TRUE(contains_ascii(exported->output.payload, "vide"));
    EXPECT_TRUE(contains_ascii(exported->output.payload, "soun"));
    EXPECT_TRUE(exported->audio_state_record.has_value());
    EXPECT_TRUE(exported->audio_provenance.has_value());
    EXPECT_FALSE(exported->audio_packet_passthrough);
    if (exported->audio_state_record.has_value() && fixture.audio_state.has_value()) {
      EXPECT_EQ(exported->audio_state_record->hash, fixture.audio_state->hash);
    }
#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO
    const auto inspected = inspect_mp4(exported->output.payload, "muxed-audio");
    EXPECT_EQ(inspected.video_streams, std::size_t{1});
    EXPECT_EQ(inspected.audio_streams, std::size_t{1});
#endif
  }
  std::filesystem::remove(fixture.path);
}

#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO

TEST(video_export_audio_track_decodes_nonempty) {
  const auto fixture = make_export_archive("decoded-audio.coda", true, true);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = fixture.stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_TRUE(exported);
  if (exported) {
    const auto inspected = inspect_mp4(exported->output.payload, "decoded-audio");
    EXPECT_TRUE(inspected.audio_packets > 0U);
    EXPECT_TRUE(inspected.decoded_audio_samples > 0U);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_passthrough_keeps_ingested_aac_packets_byte_exact) {
  const auto source_path = test_path("encoded-passthrough-input.mp4");
  const auto archive_path = test_path("encoded-passthrough.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  EXPECT_TRUE(write_fixture(
      source_path, encoded_media_fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("encoded-audio-passthrough");
  auto ingested = video::ingest_video_ffmpeg(video::FfmpegVideoIngestRequest{
      .source_uri = source_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "encoded audio passthrough",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 0,
      .end_ns = 250'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 8,
  });
  EXPECT_TRUE(ingested);
  if (!ingested) {
    std::filesystem::remove(source_path);
    std::filesystem::remove(archive_path);
    return;
  }

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto encoded = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 1,
                                      .maximum_encoded_bytes = 1024 * 1024});
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_EQ(encoded->size(), std::size_t{1});
  if (!encoded || encoded->size() != 1U) {
    std::filesystem::remove(source_path);
    std::filesystem::remove(archive_path);
    return;
  }
  std::vector<std::vector<std::byte>> expected_payloads;
  for (const auto& packet : encoded->front().state.packets) {
    expected_payloads.push_back(packet.payload);
  }

  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_TRUE(exported->audio_state_record.has_value());
    EXPECT_TRUE(exported->audio_packet_passthrough);
    if (exported->audio_state_record.has_value()) {
      EXPECT_EQ(exported->audio_state_record->type_code(),
                video::video_encoded_audio_state_record_type);
    }
    const auto inspected =
        inspect_mp4(exported->output.payload, "encoded-passthrough");
    EXPECT_EQ(inspected.audio_streams, std::size_t{1});
    EXPECT_TRUE(inspected.decoded_audio_samples > 0U);
    EXPECT_EQ(inspected.audio_payloads, expected_payloads);
  }
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
}

TEST(video_export_preserves_positive_audio_start_offset) {
  constexpr std::int64_t expected_offset_ns = 20'000'000;
  const auto fixture = make_export_archive("audio-offset.coda", true, true,
                                           expected_offset_ns);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = fixture.stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_TRUE(exported);
  if (exported) {
    const auto inspected = inspect_mp4(exported->output.payload, "audio-offset");
    EXPECT_TRUE(inspected.first_audio_pts_ns.has_value());
    EXPECT_TRUE(inspected.audio_sample_rate > 0);
    if (inspected.first_audio_pts_ns.has_value() &&
        inspected.audio_sample_rate > 0) {
      const auto sample_rate =
          static_cast<std::uint64_t>(inspected.audio_sample_rate);
      const auto one_aac_frame_ns =
          (1024ULL * 1'000'000'000ULL + sample_rate - 1ULL) / sample_rate;
      EXPECT_TRUE(absolute_difference(*inspected.first_audio_pts_ns,
                                      expected_offset_ns) <= one_aac_frame_ns);
    }
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_old_archive_without_h1_audio_stays_video_only) {
  const auto fixture = make_export_archive("old-video-only.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = fixture.stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_TRUE(exported);
  if (exported) {
    const auto inspected = inspect_mp4(exported->output.payload, "old-video-only");
    EXPECT_EQ(inspected.video_streams, std::size_t{1});
    EXPECT_EQ(inspected.audio_streams, std::size_t{0});
    EXPECT_FALSE(exported->audio_state_record.has_value());
    EXPECT_FALSE(exported->audio_provenance.has_value());
  }
  std::filesystem::remove(fixture.path);
}

#endif

TEST(video_export_rejects_simultaneous_verified_audio_state_forms) {
  const auto fixture = make_export_archive(
      "audio-state-conflict.coda", true, true, 20'000'000, 100'000'000,
      true);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code,
              video::ffmpeg_video_export_available()
                  ? codec::ErrorCode::archive_corrupt
                  : codec::ErrorCode::model_incompatible);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_rejects_encoded_audio_leading_trim) {
  const auto fixture = make_export_archive(
      "encoded-audio-leading-trim.coda", true, false, 20'000'000,
      100'000'000, true, 1);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::model_incompatible);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_invalid_verified_audio_fails_instead_of_muting) {
  const auto fixture = make_export_archive("invalid-audio.coda", true, true,
                                           20'000'000, 90'000'000);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = fixture.stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code,
              video::ffmpeg_video_export_available()
                  ? codec::ErrorCode::archive_corrupt
                  : codec::ErrorCode::model_incompatible);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_refuses_unverified_vfr1) {
  const auto fixture = make_export_archive("unverified.coda", false);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::invalid_argument);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_rejects_zero_output_limit) {
  const auto fixture = make_export_archive("zero-limit.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 0});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::invalid_argument);
  }
  std::filesystem::remove(fixture.path);
}
