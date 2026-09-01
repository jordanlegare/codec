#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/archive.hpp>
#include <codec/profiles/video.hpp>
#include <codec/profiles/video_export.hpp>

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

std::filesystem::path export_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-export-" + std::string{name});
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
  std::uint32_t accumulator = 0U;
  int bits = 0;
  for (const char ch : encoded) {
    if (ch == '=') break;
    const auto digit = value(ch);
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

HlsHttpFixture hls_av_server() {
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

video::VideoFrameQuery video_query(const codec::StreamId& stream) {
  return video::VideoFrameQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 4U * 1024U * 1024U,
  };
}

video::VideoAudioQuery audio_query(const codec::StreamId& stream) {
  return video::VideoAudioQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 4U * 1024U * 1024U,
  };
}

bool has_mp4_ftyp(const std::vector<std::byte>& payload) {
  return payload.size() >= 8U && payload[4] == std::byte{'f'} &&
         payload[5] == std::byte{'t'} && payload[6] == std::byte{'y'} &&
         payload[7] == std::byte{'p'};
}

}  // namespace

TEST(video_export_remuxes_hls_annex_b_h264_and_adts_aac) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_av_server();
  const auto archive_path = export_path("hls.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-export-hls-passthrough");
  const video::FfmpegVideoIngestRequest request{
      .source_uri = server.url("/master.m3u8"),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "HLS export passthrough",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 1'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
      .deny_private_network = false,
  };

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_TRUE(report->audio_state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto encoded_video =
      video::query_verified_video_encoded_video(*archive, video_query(stream));
  EXPECT_TRUE(encoded_video);
  if (encoded_video) EXPECT_EQ(encoded_video->size(), std::size_t{1});
  if (encoded_video && encoded_video->size() == 1U) {
    EXPECT_EQ(encoded_video->front().state.framing,
              video::EncodedVideoPacketFraming::annex_b);
  }
  auto encoded_audio =
      video::query_verified_video_encoded_audio(*archive, audio_query(stream));
  EXPECT_TRUE(encoded_audio);
  if (encoded_audio) EXPECT_EQ(encoded_audio->size(), std::size_t{1});
  if (encoded_audio && encoded_audio->size() == 1U) {
    EXPECT_TRUE(encoded_audio->front().state.decoder_config.empty());
    EXPECT_TRUE(!encoded_audio->front().state.packets.empty());
  }

  auto exported = video::export_verified_video_mp4(
      *archive, video_query(stream),
      video::VideoMp4ExportLimits{.maximum_output_bytes = 4U * 1024U * 1024U});
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_TRUE(has_mp4_ftyp(exported->output.payload));
    EXPECT_TRUE(exported->video_packet_passthrough);
    EXPECT_TRUE(exported->audio_packet_passthrough);
    EXPECT_TRUE(!exported->state_records.empty());
    EXPECT_TRUE(exported->audio_state_record.has_value());
  }

  std::filesystem::remove(archive_path);
}
