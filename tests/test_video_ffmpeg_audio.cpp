#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

TEST(video_ffmpeg_audio_direct_ingest_writes_verified_pcm16) {
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
  auto audio = video::query_verified_video_pcm16_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1024,
          .maximum_encoded_bytes = 1024ULL * 1024ULL * 1024ULL,
      });
  EXPECT_TRUE(audio);
  if (audio && !audio->empty()) {
    EXPECT_EQ(audio->size(), std::size_t{1});
    EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{8000});
    EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
    EXPECT_TRUE(!audio->front().state.samples.empty());
    EXPECT_EQ(audio->front().source_records.size(), std::size_t{1});
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_ingest_reuses_existing_pcm16_encoding) {
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
  auto payload = archive->read_payload(*report->audio_state);
  EXPECT_TRUE(payload);
  auto decoded = codec::decode_pcm16_state(*payload);
  EXPECT_TRUE(decoded);
  if (decoded) {
    EXPECT_EQ(decoded->sample_rate, std::uint32_t{8000});
    EXPECT_EQ(decoded->channels, std::uint16_t{1});
    EXPECT_TRUE(!decoded->samples.empty());
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

TEST(video_hls_audio_ingest_writes_verified_pcm16) {
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
    auto audio = video::query_verified_video_pcm16_audio(
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
        EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
        EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{8000});
      }
    }
  }
  std::filesystem::remove(archive_path);
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
    auto audio = video::query_verified_video_pcm16_audio(
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
                std::string{"codec.video.pcm16.canonicalize.hls"});
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
