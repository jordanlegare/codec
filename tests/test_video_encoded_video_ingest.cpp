#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/archive.hpp>
#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path encoded_ingest_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-ingest-" + std::string{name});
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

bool write_fixture(const std::filesystem::path& path,
                   const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::vector<std::byte> bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
}

}  // namespace

TEST(video_ffmpeg_ingest_writes_evp1_and_no_raw_pixel_states) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = encoded_ingest_path("direct.mp4");
  const auto archive_path = encoded_ingest_path("direct.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto source_bytes = fixture("video_4x4_h264.mp4.b64");
  EXPECT_TRUE(write_fixture(source_path, source_bytes));
  const auto stream = codec::derive_stream_id("encoded-video-direct");

  const video::FfmpegVideoIngestRequest request{
      .source_uri = source_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "encoded direct",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 1'000'000'000,
      .end_ns = 2'000'000'000,
      .maximum_frames = 8,
  };
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});
  EXPECT_EQ(report->states.front().type_code(),
            video::video_encoded_video_state_record_type);

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto raw_frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(raw_frames);
  if (raw_frames) EXPECT_TRUE(raw_frames->empty());

  auto encoded = video::query_verified_video_encoded_video(
      *archive, video::VideoFrameQuery{.stream = stream,
                                       .time = std::nullopt,
                                       .maximum_results = 8,
                                       .maximum_encoded_bytes = 1024U * 1024U});
  EXPECT_TRUE(encoded);
  if (encoded) {
    EXPECT_EQ(encoded->size(), std::size_t{1});
    if (!encoded->empty()) {
      const auto& state = encoded->front().state;
      EXPECT_EQ(state.codec, video::EncodedVideoCodec::h264);
      EXPECT_EQ(state.framing,
                video::EncodedVideoPacketFraming::length_prefixed);
      EXPECT_EQ(state.coded_width, std::uint32_t{4});
      EXPECT_EQ(state.coded_height, std::uint32_t{4});
      EXPECT_TRUE(state.validated_frames > 0U);
      EXPECT_TRUE(!state.decoder_config.empty());
      EXPECT_TRUE(!state.packets.empty());
      EXPECT_EQ(encoded->front().provenance.process.operation,
                std::string{"codec.video.encoded-video.preserve"});
      EXPECT_EQ(encoded->front().source_records.size(), std::size_t{1});
    }
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_hls_ingest_writes_one_evp1_with_complete_resource_frontier) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto segment0 = fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = fixture("hls_4x4_seg1.ts.b64");
  const auto manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\nseg0.ts\n"
      "#EXTINF:1.000000,\nseg1.ts\n"
      "#EXT-X-ENDLIST\n");
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

  const auto archive_path = encoded_ingest_path("hls.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("encoded-video-hls");
  const video::FfmpegVideoIngestRequest request{
      .source_uri = server.url("/live/playlist.m3u8"),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "encoded HLS",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 2'000'000'000,
      .maximum_frames = 8,
      .deny_private_network = false,
  };
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});
  EXPECT_EQ(report->states.front().type_code(),
            video::video_encoded_video_state_record_type);
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{2});

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto raw_frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(raw_frames);
  if (raw_frames) EXPECT_TRUE(raw_frames->empty());
  auto encoded = video::query_verified_video_encoded_video(
      *archive, video::VideoFrameQuery{.stream = stream,
                                       .time = std::nullopt,
                                       .maximum_results = 8,
                                       .maximum_encoded_bytes = 1024U * 1024U});
  EXPECT_TRUE(encoded);
  if (encoded) {
    EXPECT_EQ(encoded->size(), std::size_t{1});
    if (!encoded->empty()) {
      EXPECT_EQ(encoded->front().state.codec, video::EncodedVideoCodec::h264);
      EXPECT_EQ(encoded->front().state.framing,
                video::EncodedVideoPacketFraming::annex_b);
      EXPECT_TRUE(!encoded->front().state.packets.empty());
      EXPECT_EQ(encoded->front().provenance.process.operation,
                std::string{"codec.video.encoded-video.preserve.hls"});
      EXPECT_EQ(encoded->front().source_records.size(), std::size_t{3});
      EXPECT_EQ(encoded->front().source_records.front().hash,
                report->source.hash);
    }
  }
  std::filesystem::remove(archive_path);
}
