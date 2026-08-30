#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/archive.hpp>
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

std::vector<std::byte> hls_bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
}

std::vector<std::byte> decode_hls_base64(std::string_view encoded) {
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

std::vector<std::byte> hls_fixture(std::string_view name) {
  const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" /
                    std::string{name};
  std::ifstream input(path, std::ios::binary);
  std::string encoded((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
  return decode_hls_base64(encoded);
}

std::filesystem::path hls_test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-hls-failure-" + std::string{name});
}

video::FfmpegVideoIngestRequest hls_request(
    std::string source_uri, const std::filesystem::path& archive,
    std::string_view identity, std::int64_t end_ns = 2'000'000'000) {
  return video::FfmpegVideoIngestRequest{
      .source_uri = std::move(source_uri),
      .archive_path = archive,
      .descriptor = codec::StreamDescriptor{
          .id = codec::derive_stream_id(identity),
          .type = codec::StreamType::video,
          .label = "HLS failure fixture",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = end_ns,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 8,
      .deny_private_network = false,
  };
}

std::vector<std::byte> two_segment_manifest(bool end_list = true) {
  std::string text =
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\n"
      "seg0.ts\n"
      "#EXTINF:1.000000,\n"
      "seg1.ts\n";
  if (end_list) text += "#EXT-X-ENDLIST\n";
  return hls_bytes(text);
}

}  // namespace

TEST(video_hls_ingest_rejects_encrypted_manifest_before_key_fetch) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto manifest = hls_bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"secret.key\"\n"
      "#EXTINF:1.000000,\n"
      "seg0.ts\n"
      "#EXT-X-ENDLIST\n");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = manifest}},
      {"/live/secret.key",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/octet-stream",
                       .body = hls_bytes("secret")}},
  });
  const auto archive = hls_test_path("encrypted.coda");
  std::filesystem::remove(archive);

  auto report = video::ingest_video_ffmpeg(
      hls_request(server.url("/live/playlist.m3u8"), archive,
                  "video-hls-encrypted"));
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  if (report->profile_error) {
    EXPECT_EQ(report->profile_error->code, codec::ErrorCode::model_incompatible);
  }
  EXPECT_TRUE(report->states.empty());
  EXPECT_TRUE(report->secondary_sources.empty());
  EXPECT_EQ(server.requests("/live/secret.key"), std::size_t{0});

  auto opened = codec::CodaArchive::open(archive);
  EXPECT_TRUE(opened);
  if (opened) EXPECT_TRUE(opened->verify().ok);
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_rejects_cross_origin_child_before_capture) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment = hls_fixture("hls_4x4_seg0.ts.b64");
  HlsHttpFixture child({
      {"/other/seg0.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment}},
  });
  const auto manifest = hls_bytes(
      "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.000000,\n" +
      child.url("/other/seg0.ts") + "\n#EXT-X-ENDLIST\n");
  HlsHttpFixture parent({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = manifest}},
  });
  const auto archive = hls_test_path("cross-origin.coda");
  std::filesystem::remove(archive);

  auto report = video::ingest_video_ffmpeg(
      hls_request(parent.url("/live/playlist.m3u8"), archive,
                  "video-hls-cross-origin"));
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  if (report->profile_error) {
    EXPECT_EQ(report->profile_error->code, codec::ErrorCode::unauthorized_source);
  }
  EXPECT_TRUE(report->secondary_sources.empty());
  EXPECT_EQ(child.requests("/other/seg0.ts"), std::size_t{0});
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_rejects_file_and_crypto_child_protocols) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto run_case = [](std::string child_uri, std::string_view suffix) {
    const auto manifest = hls_bytes(
        "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.000000,\n" +
        child_uri + "\n#EXT-X-ENDLIST\n");
    HlsHttpFixture server({
        {"/live/playlist.m3u8",
         HlsHttpResponse{.status = 200,
                         .content_type = "application/vnd.apple.mpegurl",
                         .body = manifest}},
    });
    const auto archive = hls_test_path(std::string{suffix} + ".coda");
    std::filesystem::remove(archive);
    auto report = video::ingest_video_ffmpeg(
        hls_request(server.url("/live/playlist.m3u8"), archive,
                    std::string{"video-hls-protocol-"} + std::string{suffix}));
    EXPECT_TRUE(report);
    if (report) {
      EXPECT_TRUE(report->profile_error.has_value());
      if (report->profile_error) {
        EXPECT_EQ(report->profile_error->code, codec::ErrorCode::protocol);
      }
      EXPECT_TRUE(report->secondary_sources.empty());
      EXPECT_TRUE(report->states.empty());
    }
    std::filesystem::remove(archive);
  };

  const auto local_path = hls_test_path("secret.ts");
  {
    std::ofstream output(local_path, std::ios::binary | std::ios::trunc);
    output << "must-not-be-read";
  }
  run_case("file://" + local_path.string(), "file");
  run_case("crypto:http://127.0.0.1/secret.ts", "crypto");
  std::filesystem::remove(local_path);
}

TEST(video_hls_ingest_honors_private_network_policy) {
  if (!video::ffmpeg_video_ingest_available()) return;
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = two_segment_manifest()}},
  });
  const auto archive = hls_test_path("private-network.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-private-network");
  request.deny_private_network = true;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_FALSE(report);
  if (!report) {
    EXPECT_EQ(report.error().code, codec::ErrorCode::unauthorized_source);
  }
  EXPECT_FALSE(std::filesystem::exists(archive));
  EXPECT_EQ(server.requests("/live/playlist.m3u8"), std::size_t{0});
}

TEST(video_hls_ingest_enforces_per_resource_limit_with_preserved_manifest) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment0 = hls_fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = hls_fixture("hls_4x4_seg1.ts.b64");
  const auto manifest = two_segment_manifest();
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = manifest}},
      {"/live/seg0.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment0}},
      {"/live/seg1.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment1}},
  });
  const auto archive = hls_test_path("per-resource-limit.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-per-resource-limit");
  request.maximum_hls_resource_bytes = 1000;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  if (report->profile_error) {
    EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
  }
  EXPECT_TRUE(report->secondary_sources.empty());
  EXPECT_TRUE(report->states.empty());
  auto opened = codec::CodaArchive::open(archive);
  EXPECT_TRUE(opened);
  if (opened) {
    EXPECT_TRUE(opened->verify().ok);
    auto parent = opened->extract_stream(request.descriptor.id);
    EXPECT_TRUE(parent);
    if (parent) EXPECT_EQ(*parent, manifest);
  }
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_enforces_total_resource_limit_with_accepted_prefix) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment0 = hls_fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = hls_fixture("hls_4x4_seg1.ts.b64");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = two_segment_manifest()}},
      {"/live/seg0.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment0}},
      {"/live/seg1.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment1}},
  });
  const auto archive = hls_test_path("total-limit.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-total-limit");
  request.maximum_hls_resource_bytes = 2000;
  request.maximum_hls_total_bytes = 1600;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  if (report->profile_error) {
    EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
  }
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{1});
  EXPECT_TRUE(report->states.empty());
  EXPECT_EQ(server.requests("/live/seg0.ts"), std::size_t{1});
  EXPECT_EQ(server.requests("/live/seg1.ts"), std::size_t{1});
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_enforces_resource_count_limit_with_accepted_prefix) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment0 = hls_fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = hls_fixture("hls_4x4_seg1.ts.b64");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = two_segment_manifest()}},
      {"/live/seg0.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment0}},
      {"/live/seg1.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment1}},
  });
  const auto archive = hls_test_path("count-limit.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-count-limit");
  request.maximum_hls_resources = 1;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  if (report->profile_error) {
    EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
  }
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{1});
  EXPECT_TRUE(report->states.empty());
  EXPECT_EQ(server.requests("/live/seg0.ts"), std::size_t{1});
  EXPECT_EQ(server.requests("/live/seg1.ts"), std::size_t{0});
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_preserves_captured_children_when_segment_decode_fails) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto malformed = hls_bytes("this is not an MPEG transport stream");
  const auto manifest = hls_bytes(
      "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n#EXTINF:1.000000,\n"
      "bad.ts\n#EXT-X-ENDLIST\n");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = manifest}},
      {"/live/bad.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = malformed}},
  });
  const auto archive = hls_test_path("malformed-child.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-malformed-child");

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{1});
  EXPECT_TRUE(report->states.empty());
  EXPECT_TRUE(report->provenance.empty());
  auto opened = codec::CodaArchive::open(archive);
  EXPECT_TRUE(opened);
  if (opened) {
    EXPECT_TRUE(opened->verify().ok);
    auto child = opened->read_payload(report->secondary_sources.front());
    EXPECT_TRUE(child);
    if (child) EXPECT_EQ(*child, malformed);
  }
  std::filesystem::remove(archive);
}

TEST(video_hls_ingest_live_playlist_stops_at_requested_media_duration) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment0 = hls_fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = hls_fixture("hls_4x4_seg1.ts.b64");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = two_segment_manifest(false)}},
      {"/live/seg0.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment0}},
      {"/live/seg1.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment1}},
  });
  const auto archive = hls_test_path("live-duration.coda");
  std::filesystem::remove(archive);
  auto request = hls_request(server.url("/live/playlist.m3u8"), archive,
                             "video-hls-live-duration", 1'000'000'000);
  request.maximum_hls_resources = 8;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{2});
  EXPECT_EQ(server.requests("/live/playlist.m3u8"), std::size_t{1});
  EXPECT_EQ(server.requests("/live/seg0.ts"), std::size_t{1});
  EXPECT_EQ(server.requests("/live/seg1.ts"), std::size_t{1});
  std::filesystem::remove(archive);
}
