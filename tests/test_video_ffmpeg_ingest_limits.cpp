#include "test.hpp"

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

std::filesystem::path limit_test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-ffmpeg-limit-" + std::string{name});
}

bool write_limit_bytes(const std::filesystem::path& path,
                       std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::vector<std::byte> bmp_2x2_fixture() {
  return {
      std::byte{'B'}, std::byte{'M'},
      std::byte{0x46}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x36}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x28}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x18}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x10}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0x00},
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };
}

std::vector<std::byte> hls_secondary_open_fixture() {
  constexpr std::string_view manifest =
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.0,\n"
      "file:///etc/passwd\n"
      "#EXT-X-ENDLIST\n";
  const auto bytes = std::as_bytes(
      std::span<const char>{manifest.data(), manifest.size()});
  return {bytes.begin(), bytes.end()};
}

video::FfmpegVideoIngestRequest limit_request(
    const std::filesystem::path& source,
    const std::filesystem::path& archive,
    const codec::StreamId& stream,
    std::string payload_type) {
  return video::FfmpegVideoIngestRequest{
      .source_uri = source.string(),
      .archive_path = archive,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "ffmpeg bounded fixture",
          .source_id = "fixture",
          .payload_type = std::move(payload_type),
      },
      .start_ns = 0,
      .end_ns = 1'000'000'000,
      .output_layout = video::PixelLayout::gray8,
      .maximum_frames = 4,
  };
}

void expect_source_only_archive(
    const video::FfmpegVideoIngestReport& report,
    const std::filesystem::path& archive_path,
    const codec::StreamId& stream,
    std::span<const std::byte> expected_source) {
  EXPECT_FALSE(report.state_exact());
  EXPECT_TRUE(report.profile_error.has_value());
  EXPECT_TRUE(report.states.empty());
  EXPECT_TRUE(report.provenance.empty());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  if (extracted) {
    EXPECT_EQ(*extracted,
              std::vector<std::byte>(expected_source.begin(),
                                     expected_source.end()));
  }
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) EXPECT_TRUE(frames->empty());
}

}  // namespace

TEST(video_ffmpeg_ingest_decoded_byte_limit_preserves_exact_s0_only) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = limit_test_path("decoded-limit.bmp");
  const auto archive_path = limit_test_path("decoded-limit.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto fixture = bmp_2x2_fixture();
  EXPECT_TRUE(write_limit_bytes(source_path, fixture));
  const auto stream = codec::derive_stream_id("video-ffmpeg-decoded-limit");
  auto request = limit_request(source_path, archive_path, stream, "image/bmp");
  request.maximum_decoded_bytes = 3;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
    }
    expect_source_only_archive(*report, archive_path, stream, fixture);
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_ffmpeg_ingest_denies_secondary_media_resource_open) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = limit_test_path("secondary-open.m3u8");
  const auto archive_path = limit_test_path("secondary-open.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto fixture = hls_secondary_open_fixture();
  EXPECT_TRUE(write_limit_bytes(source_path, fixture));
  const auto stream = codec::derive_stream_id("video-ffmpeg-secondary-open");
  auto request = limit_request(source_path, archive_path, stream,
                               "application/vnd.apple.mpegurl");

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::decode);
      EXPECT_TRUE(report->profile_error->message.find("Operation not permitted") !=
                  std::string::npos);
    }
    expect_source_only_archive(*report, archive_path, stream, fixture);
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}
