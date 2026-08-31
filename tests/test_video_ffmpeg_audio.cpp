#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <algorithm>
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

std::filesystem::path audio_ingest_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-audio-" + std::string{name});
}

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

std::vector<std::byte> fixture(std::string_view name) {
  const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" /
                    std::string{name};
  std::ifstream input(path, std::ios::binary);
  std::string encoded((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  return decode_base64(encoded);
}

std::vector<std::byte> bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
}

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO

struct AudioInputDeleter {
  void operator()(AVFormatContext* value) const noexcept {
    if (value != nullptr) avformat_close_input(&value);
  }
};

struct AudioPacketDeleter {
  void operator()(AVPacket* value) const noexcept {
    if (value != nullptr) av_packet_free(&value);
  }
};

struct DemuxedAacPacket {
  std::int64_t pts_ns{};
  std::int64_t dts_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
};

struct DemuxedAacStream {
  std::int32_t codec_profile{};
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::vector<std::byte> decoder_config;
  std::vector<DemuxedAacPacket> packets;
};

DemuxedAacStream demux_aac_source(const std::filesystem::path& path) {
  AVFormatContext* raw_format = nullptr;
  const auto path_text = path.string();
  if (avformat_open_input(&raw_format, path_text.c_str(), nullptr, nullptr) < 0 ||
      raw_format == nullptr) {
    if (raw_format != nullptr) avformat_close_input(&raw_format);
    throw std::runtime_error("cannot open AAC source fixture");
  }
  std::unique_ptr<AVFormatContext, AudioInputDeleter> format{raw_format};
  if (avformat_find_stream_info(format.get(), nullptr) < 0) {
    throw std::runtime_error("cannot inspect AAC source fixture");
  }
  const auto audio_index =
      av_find_best_stream(format.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (audio_index < 0 ||
      static_cast<unsigned>(audio_index) >= format->nb_streams) {
    throw std::runtime_error("AAC source fixture has no selected audio stream");
  }
  const auto* stream = format->streams[audio_index];
  if (stream == nullptr || stream->codecpar == nullptr ||
      stream->codecpar->codec_id != AV_CODEC_ID_AAC ||
      stream->codecpar->sample_rate <= 0 ||
      stream->codecpar->ch_layout.nb_channels < 1 ||
      stream->codecpar->ch_layout.nb_channels > 2 ||
      stream->time_base.num <= 0 || stream->time_base.den <= 0 ||
      stream->codecpar->extradata_size < 0 ||
      (stream->codecpar->extradata_size > 0 &&
       stream->codecpar->extradata == nullptr)) {
    throw std::runtime_error("AAC source fixture has invalid stream metadata");
  }

  DemuxedAacStream source{
      .codec_profile = stream->codecpar->profile,
      .sample_rate = static_cast<std::uint32_t>(stream->codecpar->sample_rate),
      .channels = static_cast<std::uint16_t>(
          stream->codecpar->ch_layout.nb_channels),
      .decoder_config = {},
      .packets = {},
  };
  if (stream->codecpar->extradata_size > 0) {
    const auto* config = reinterpret_cast<const std::byte*>(
        stream->codecpar->extradata);
    source.decoder_config.assign(
        config, config + stream->codecpar->extradata_size);
  }

  constexpr AVRational nanosecond_time_base{1, 1'000'000'000};
  std::unique_ptr<AVPacket, AudioPacketDeleter> packet{av_packet_alloc()};
  if (!packet) throw std::runtime_error("cannot allocate AAC source packet");
  std::optional<std::int64_t> previous_end_ns;
  for (;;) {
    const auto read = av_read_frame(format.get(), packet.get());
    if (read == AVERROR_EOF) break;
    if (read < 0) throw std::runtime_error("cannot demux AAC source packet");
    if (packet->stream_index != audio_index) {
      av_packet_unref(packet.get());
      continue;
    }
    auto pts = packet->pts;
    auto dts = packet->dts;
    if (pts == AV_NOPTS_VALUE) pts = dts;
    if (dts == AV_NOPTS_VALUE) dts = pts;
    if (packet->data == nullptr || packet->size <= 0) {
      throw std::runtime_error("AAC source packet is missing bytes");
    }
    auto duration = packet->duration;
    if (duration <= 0) {
      duration = av_rescale_q(1024, AVRational{1, stream->codecpar->sample_rate},
                              stream->time_base);
    }
    const auto duration_ns =
        av_rescale_q(duration, stream->time_base, nanosecond_time_base);
    if (duration <= 0 || duration_ns <= 0) {
      throw std::runtime_error("AAC source packet has invalid duration");
    }
    std::int64_t pts_ns = 0;
    std::int64_t dts_ns = 0;
    if (pts == AV_NOPTS_VALUE || dts == AV_NOPTS_VALUE) {
      // The raw AAC transport fixture timestamps its first access unit. Derive
      // later contiguous AAC access-unit times from the independently demuxed
      // sample rate when MPEG-TS omits repeated timestamps.
      if (!previous_end_ns.has_value()) {
        throw std::runtime_error("first AAC source packet has no timing");
      }
      pts_ns = *previous_end_ns;
      dts_ns = *previous_end_ns;
    } else {
      pts_ns = av_rescale_q(pts, stream->time_base, nanosecond_time_base);
      dts_ns = av_rescale_q(dts, stream->time_base, nanosecond_time_base);
    }
    const auto* payload = reinterpret_cast<const std::byte*>(packet->data);
    source.packets.push_back(DemuxedAacPacket{
        .pts_ns = pts_ns,
        .dts_ns = dts_ns,
        .duration_ns = static_cast<std::uint64_t>(duration_ns),
        .flags = static_cast<std::uint32_t>(packet->flags) & 0x1fU,
        .payload = std::vector<std::byte>(payload, payload + packet->size),
    });
    previous_end_ns = pts_ns + duration_ns;
    av_packet_unref(packet.get());
  }
  return source;
}

void expect_eap1_matches_source_demux(
    const DemuxedAacStream& source,
    const video::VerifiedVideoEncodedAudio& verified) {
  const auto& state = verified.state;
  EXPECT_EQ(state.codec, video::EncodedAudioCodec::aac);
  EXPECT_EQ(state.codec_profile, source.codec_profile);
  EXPECT_EQ(state.sample_rate, source.sample_rate);
  EXPECT_EQ(state.channels, source.channels);
  EXPECT_EQ(state.decoder_config, source.decoder_config);
  EXPECT_TRUE(!state.packets.empty());
  EXPECT_TRUE(source.packets.size() >= state.packets.size());
  if (state.packets.empty() || source.packets.size() < state.packets.size()) {
    return;
  }

  std::size_t matching_subsequences = 0U;
  for (std::size_t begin = 0U;
       begin + state.packets.size() <= source.packets.size(); ++begin) {
    const auto origin_ns =
        source.packets[begin].pts_ns - state.packets.front().pts_offset_ns;
    bool matches = true;
    for (std::size_t index = 0U; index < state.packets.size(); ++index) {
      const auto& actual = state.packets[index];
      const auto& literal = source.packets[begin + index];
      if (actual.pts_offset_ns != literal.pts_ns - origin_ns ||
          actual.dts_offset_ns != literal.dts_ns - origin_ns ||
          actual.duration_ns != literal.duration_ns ||
          actual.flags != literal.flags || actual.payload != literal.payload) {
        matches = false;
        break;
      }
    }
    if (matches) ++matching_subsequences;
  }
  EXPECT_EQ(matching_subsequences, std::size_t{1});
}

#endif

video::FfmpegVideoIngestRequest request_for(
    const std::filesystem::path& source,
    const std::filesystem::path& archive,
    const codec::StreamId& stream) {
  return video::FfmpegVideoIngestRequest{
      .source_uri = source.string(),
      .archive_path = archive,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "camera with sound",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 1'000'000'000,
      .end_ns = 1'250'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 8,
  };
}

video::FfmpegVideoIngestRequest hls_request_for(
    const HlsHttpFixture& server, const std::filesystem::path& archive,
    const codec::StreamId& stream) {
  return video::FfmpegVideoIngestRequest{
      .source_uri = server.url("/master.m3u8"),
      .archive_path = archive,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "HLS camera with sound",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 1'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
      .deny_private_network = false,
  };
}

HlsHttpFixture hls_audio_server() {
  const auto master = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"mono\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42c00a,mp4a.40.2\",AUDIO=\"audio\"\n"
      "video.m3u8\n");
  const auto video_manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\n"
      "video.ts\n"
      "#EXT-X-ENDLIST\n");
  const auto audio_manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:0.250000,\n"
      "audio.ts\n"
      "#EXT-X-ENDLIST\n");
  return HlsHttpFixture({
      {"/master.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = master}},
      {"/video.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = video_manifest}},
      {"/audio.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = audio_manifest}},
      {"/video.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = fixture("hls_4x4_seg0.ts.b64")}},
      {"/audio.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = fixture("hls_audio_mono.ts.b64")}},
  });
}

void cleanup(const std::filesystem::path& source,
             const std::filesystem::path& archive) {
  std::filesystem::remove(archive);
  std::filesystem::remove(source);
}

}  // namespace

TEST(video_ffmpeg_audio_direct_ingest_writes_verified_encoded_packets) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("mono.mp4");
  const auto archive_path = audio_ingest_path("mono.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-mono");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->audio_present);
  EXPECT_TRUE(report->audio_state.has_value());
  EXPECT_TRUE(report->audio_provenance.has_value());
  EXPECT_TRUE(report->audio_state_exact());
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_encoded_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1024,
          .maximum_encoded_bytes = 1024ULL * 1024ULL * 1024ULL,
      });
  EXPECT_TRUE(audio);
  if (audio) EXPECT_EQ(audio->size(), std::size_t{1});
  if (audio && !audio->empty()) {
    EXPECT_EQ(audio->front().state.codec, video::EncodedAudioCodec::aac);
    EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{8000});
    EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
    EXPECT_TRUE(!audio->front().state.packets.empty());
    EXPECT_EQ(audio->front().source_records.size(), std::size_t{1});
  }
  auto legacy = video::query_verified_video_pcm16_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 1024,
                                      .maximum_encoded_bytes =
                                          1024ULL * 1024ULL * 1024ULL});
  EXPECT_TRUE(legacy);
  if (legacy) EXPECT_TRUE(legacy->empty());
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_state_matches_independent_source_demux) {
#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = audio_ingest_path("direct-exactness.mp4");
  const auto archive_path = audio_ingest_path("direct-exactness.coda");
  cleanup(source_path, archive_path);
  EXPECT_TRUE(
      write_bytes(source_path, fixture("video_audio_mono.mp4.b64")));
  const auto literal_source = demux_aac_source(source_path);
  const auto stream = codec::derive_stream_id("video-audio-direct-exactness");
  auto report = video::ingest_video_ffmpeg(
      request_for(source_path, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = std::nullopt,
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 1024ULL * 1024ULL,
                });
  EXPECT_TRUE(audio);
  if (audio) EXPECT_EQ(audio->size(), std::size_t{1});
  if (audio && audio->size() == 1U) {
    expect_eap1_matches_source_demux(literal_source, audio->front());
  }
  cleanup(source_path, archive_path);
#endif
}

TEST(video_ffmpeg_audio_direct_ingest_state_is_smaller_than_pcm16_equivalent) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("pcm-reuse.mp4");
  const auto archive_path = audio_ingest_path("pcm-reuse.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-pcm-reuse");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report || !report->audio_state.has_value()) return;
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 1024,
                                      .maximum_encoded_bytes =
                                          1024ULL * 1024ULL * 1024ULL});
  EXPECT_TRUE(audio);
  if (audio) EXPECT_EQ(audio->size(), std::size_t{1});
  if (audio && audio->size() == 1U) {
    const auto& verified = audio->front();
    EXPECT_EQ(verified.state_record.type_code(),
              video::video_encoded_audio_state_record_type);
    const auto pcm16_equivalent =
        verified.state.presentation_frames * verified.state.channels *
        sizeof(std::int16_t);
    EXPECT_TRUE(verified.state_record.payload_size < pcm16_equivalent);
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_no_audio_remains_video_only_exact) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("video-only.mp4");
  const auto archive_path = audio_ingest_path("video-only.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_h264.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-video-only");
  auto request = request_for(source, archive_path, stream);
  request.end_ns = 2'000'000'000;
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_FALSE(report->audio_present);
    EXPECT_FALSE(report->audio_state.has_value());
    EXPECT_FALSE(report->audio_provenance.has_value());
    EXPECT_TRUE(report->audio_state_exact());
    EXPECT_TRUE(report->state_exact());
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_multichannel_is_profile_error_but_keeps_video_s1) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("multichannel.mp4");
  const auto archive_path = audio_ingest_path("multichannel.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_multichannel.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-multichannel");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->audio_present);
    EXPECT_FALSE(report->audio_state.has_value());
    EXPECT_FALSE(report->audio_provenance.has_value());
    EXPECT_TRUE(report->profile_error.has_value());
    EXPECT_FALSE(report->state_exact());
    EXPECT_TRUE(!report->states.empty());
    EXPECT_EQ(report->states.size(), report->provenance.size());
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_audio_limit_is_enforced) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("limit.mp4");
  const auto archive_path = audio_ingest_path("limit.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-limit");
  auto request = request_for(source, archive_path, stream);
  request.maximum_decoded_audio_bytes = 2;
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->audio_present);
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error.has_value()) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
    }
    EXPECT_TRUE(!report->states.empty());
    EXPECT_FALSE(report->state_exact());
  }
  cleanup(source, archive_path);
}

TEST(video_hls_audio_ingest_writes_verified_encoded_packets) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_audio_server();
  const auto archive_path = audio_ingest_path("hls-audio.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-audio");
  auto report = video::ingest_video_ffmpeg(hls_request_for(server, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->audio_present);
  EXPECT_TRUE(report->audio_state_exact());
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive) {
    auto audio = video::query_verified_video_encoded_audio(
        *archive,
        video::VideoAudioQuery{
            .stream = stream,
            .time = std::nullopt,
            .maximum_results = 1024,
            .maximum_encoded_bytes = 1024ULL * 1024ULL * 1024ULL,
        });
    EXPECT_TRUE(audio);
    if (audio) {
      EXPECT_EQ(audio->size(), std::size_t{1});
      if (!audio->empty()) {
        EXPECT_EQ(audio->front().state.codec, video::EncodedAudioCodec::aac);
        EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
        EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{8000});
        EXPECT_TRUE(!audio->front().state.packets.empty());
      }
    }
  }
  std::filesystem::remove(archive_path);
}

TEST(video_hls_audio_state_matches_independent_transport_demux) {
#ifdef CODEC_TEST_HAS_FFMPEG_VIDEO
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_audio_server();
  const auto transport_path = audio_ingest_path("hls-audio-exactness.ts");
  const auto archive_path = audio_ingest_path("hls-audio-exactness.coda");
  cleanup(transport_path, archive_path);
  EXPECT_TRUE(
      write_bytes(transport_path, fixture("hls_audio_mono.ts.b64")));
  const auto literal_source = demux_aac_source(transport_path);
  const auto stream = codec::derive_stream_id("video-hls-audio-exactness");
  auto report = video::ingest_video_ffmpeg(
      hls_request_for(server, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{
                    .stream = stream,
                    .time = std::nullopt,
                    .maximum_results = 8,
                    .maximum_encoded_bytes = 1024ULL * 1024ULL,
                });
  EXPECT_TRUE(audio);
  if (audio) EXPECT_EQ(audio->size(), std::size_t{1});
  if (audio && audio->size() == 1U) {
    expect_eap1_matches_source_demux(literal_source, audio->front());
  }
  cleanup(transport_path, archive_path);
#endif
}

TEST(video_hls_audio_provenance_uses_primary_plus_ordered_frontier) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_audio_server();
  const auto archive_path = audio_ingest_path("hls-audio-frontier.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-audio-frontier");
  auto report = video::ingest_video_ffmpeg(hls_request_for(server, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive) {
    auto audio = video::query_verified_video_encoded_audio(
        *archive,
        video::VideoAudioQuery{
            .stream = stream,
            .time = std::nullopt,
            .maximum_results = 1024,
            .maximum_encoded_bytes = 1024ULL * 1024ULL * 1024ULL,
        });
    EXPECT_TRUE(audio);
    if (audio && audio->size() == 1U) {
      const auto& sources = audio->front().source_records;
      EXPECT_EQ(sources.size(), report->secondary_sources.size() + 1U);
      if (!sources.empty()) EXPECT_EQ(sources.front().hash, report->source.hash);
      for (std::size_t index = 0; index < report->secondary_sources.size() &&
                                  index + 1U < sources.size();
           ++index) {
        EXPECT_EQ(sources[index + 1U].hash,
                  report->secondary_sources[index].hash);
      }
      EXPECT_EQ(audio->front().provenance.process.operation,
                std::string{"codec.video.encoded-audio.preserve.hls"});
    }
  }
  std::filesystem::remove(archive_path);
}

TEST(video_hls_audio_uses_same_capture_session_not_second_fetch) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_audio_server();
  const auto archive_path = audio_ingest_path("hls-audio-fetch.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-audio-fetch");
  auto report = video::ingest_video_ffmpeg(hls_request_for(server, archive_path, stream));
  EXPECT_TRUE(report);
  if (report) EXPECT_TRUE(report->audio_state_exact());
  EXPECT_EQ(server.requests("/video.m3u8"), std::size_t{1});
  EXPECT_EQ(server.requests("/audio.m3u8"), std::size_t{1});
  EXPECT_EQ(server.requests("/video.ts"), std::size_t{1});
  EXPECT_EQ(server.requests("/audio.ts"), std::size_t{1});
  std::filesystem::remove(archive_path);
}

TEST(video_hls_audio_failure_keeps_video_s1_and_s0) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_audio_server();
  const auto archive_path = audio_ingest_path("hls-audio-limit.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-audio-limit");
  auto request = hls_request_for(server, archive_path, stream);
  request.maximum_decoded_audio_bytes = 2;
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->audio_present);
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error.has_value()) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
    }
    EXPECT_TRUE(!report->states.empty());
    EXPECT_EQ(report->states.size(), report->provenance.size());
    EXPECT_FALSE(report->audio_state.has_value());
    EXPECT_FALSE(report->audio_provenance.has_value());
    EXPECT_FALSE(report->state_exact());
  }
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive && report) {
    auto primary = archive->read_payload(report->source);
    EXPECT_TRUE(primary);
    EXPECT_TRUE(archive->verify().ok);
  }
  std::filesystem::remove(archive_path);
}
