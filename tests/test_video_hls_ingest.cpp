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

std::vector<std::byte> bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
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

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-hls-" + std::string{name});
}

video::VideoFrameQuery query_for(const codec::StreamId& stream) {
  return video::VideoFrameQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024U * 1024U,
  };
}

}  // namespace

TEST(video_hls_ingest_preserves_manifest_segments_and_emits_one_evp1) {
  if (!video::ffmpeg_video_ingest_available()) return;

  const auto segment0 = fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = fixture("hls_4x4_seg1.ts.b64");
  EXPECT_EQ(segment0.size(), std::size_t{1504});
  EXPECT_EQ(segment1.size(), std::size_t{752});

  const auto manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\n"
      "seg0.ts\n"
      "#EXTINF:1.000000,\n"
      "seg1.ts\n"
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

  const auto archive_path = test_path("two-segment.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-two-segment");
  const video::FfmpegVideoIngestRequest request{
      .source_uri = server.url("/live/playlist.m3u8"),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "HLS fixture",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 2'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
      .deny_private_network = false,
  };

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});
  EXPECT_EQ(report->states.front().type_code(),
            video::video_encoded_video_state_record_type);
  EXPECT_EQ(report->secondary_descriptors.size(), std::size_t{2});
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{2});
  EXPECT_EQ(server.requests("/live/seg0.ts"), std::size_t{1});
  EXPECT_EQ(server.requests("/live/seg1.ts"), std::size_t{1});

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  EXPECT_TRUE(archive->verify().ok);

  auto parent = archive->extract_stream(stream);
  EXPECT_TRUE(parent);
  if (parent) EXPECT_EQ(*parent, manifest);

  EXPECT_EQ(report->secondary_sources[0].payload_size,
            static_cast<std::uint64_t>(segment0.size()));
  EXPECT_EQ(report->secondary_sources[1].payload_size,
            static_cast<std::uint64_t>(segment1.size()));
  auto child0 = archive->read_payload(report->secondary_sources[0]);
  auto child1 = archive->read_payload(report->secondary_sources[1]);
  EXPECT_TRUE(child0);
  EXPECT_TRUE(child1);
  if (child0) EXPECT_EQ(*child0, segment0);
  if (child1) EXPECT_EQ(*child1, segment1);

  auto descriptors = archive->streams();
  EXPECT_TRUE(descriptors);
  if (descriptors) {
    std::size_t opaque_children = 0U;
    for (const auto& descriptor : *descriptors) {
      if (descriptor.id == stream) continue;
      ++opaque_children;
      EXPECT_EQ(descriptor.type, codec::StreamType::opaque);
      EXPECT_EQ(descriptor.source_id,
                std::string{"codec.video.hls-resource"});
      EXPECT_TRUE(descriptor.label.find("127.0.0.1") == std::string::npos);
      EXPECT_TRUE(descriptor.label.find("http://") == std::string::npos);
    }
    EXPECT_EQ(opaque_children, std::size_t{2});
  }

  auto raw = video::query_verified_raw_video_frames(*archive, query_for(stream));
  EXPECT_TRUE(raw);
  if (raw) EXPECT_TRUE(raw->empty());
  auto encoded = video::query_verified_video_encoded_video(*archive, query_for(stream));
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
      EXPECT_EQ(encoded->front().source_records[0].hash, report->source.hash);
      EXPECT_EQ(encoded->front().source_records[1].hash,
                report->secondary_sources[0].hash);
      EXPECT_EQ(encoded->front().source_records[2].hash,
                report->secondary_sources[1].hash);
    }
  }

  std::filesystem::remove(archive_path);
}

TEST(video_hls_ingest_reopens_same_url_as_distinct_snapshots) {
  if (!video::ffmpeg_video_ingest_available()) return;

  const auto segment0 = fixture("hls_4x4_seg0.ts.b64");
  const auto segment1 = fixture("hls_4x4_seg1.ts.b64");
  const auto manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\n"
      "same.ts\n"
      "#EXTINF:1.000000,\n"
      "same.ts\n"
      "#EXT-X-ENDLIST\n");
  HlsHttpFixture server({
      {"/live/playlist.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = manifest}},
      {"/live/same.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = segment0,
                       .subsequent_bodies = {segment1}}},
  });

  const auto archive_path = test_path("reopened-segment.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-hls-reopened-segment");
  const video::FfmpegVideoIngestRequest request{
      .source_uri = server.url("/live/playlist.m3u8"),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "HLS reopened segment fixture",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 2'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
      .deny_private_network = false,
  };

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_EQ(report->secondary_descriptors.size(), std::size_t{2});
  EXPECT_EQ(report->secondary_sources.size(), std::size_t{2});
  EXPECT_EQ(server.requests("/live/same.ts"), std::size_t{2});
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive && report->secondary_sources.size() == 2U) {
    const auto first = archive->read_payload(report->secondary_sources[0]);
    const auto second = archive->read_payload(report->secondary_sources[1]);
    EXPECT_TRUE(first);
    EXPECT_TRUE(second);
    if (first) EXPECT_EQ(*first, segment0);
    if (second) EXPECT_EQ(*second, segment1);
    EXPECT_TRUE(report->secondary_sources[0].hash !=
                report->secondary_sources[1].hash);
    EXPECT_TRUE(report->secondary_sources[0].stream !=
                report->secondary_sources[1].stream);
  }
  std::filesystem::remove(archive_path);
}
