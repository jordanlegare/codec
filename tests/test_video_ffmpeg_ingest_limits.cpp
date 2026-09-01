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
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0xff},
      std::byte{0xff}, std::byte{0xff}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0xff}, std::byte{0x00},
      std::byte{0xff}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
  };
}

std::vector<std::byte> concat_secondary_open_fixture(
    std::string_view nested_name) {
  std::string manifest = "ffconcat version 1.0\nfile ";
  manifest += nested_name;
  manifest += '\n';
  const auto view = std::as_bytes(
      std::span<const char>{manifest.data(), manifest.size()});
  return {view.begin(), view.end()};
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
  const video::VideoFrameQuery query{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024U * 1024U,
  };
  auto raw = video::query_verified_raw_video_frames(*archive, query);
  EXPECT_TRUE(raw);
  if (raw) EXPECT_TRUE(raw->empty());
  auto encoded = video::query_verified_video_encoded_video(*archive, query);
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_TRUE(encoded->empty());
}

}  // namespace

TEST(video_ffmpeg_ingest_decoded_byte_limit_preserves_exact_s0_only) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = limit_test_path("decoded-limit.mp4");
  const auto archive_path = limit_test_path("decoded-limit.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto source_bytes = fixture("video_4x4_h264.mp4.b64");
  EXPECT_TRUE(write_limit_bytes(source_path, source_bytes));
  const auto stream = codec::derive_stream_id("video-ffmpeg-decoded-limit");
  auto request = limit_request(source_path, archive_path, stream, "video/mp4");
  request.maximum_decoded_bytes = 15;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
    }
    expect_source_only_archive(*report, archive_path, stream, source_bytes);
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_ffmpeg_ingest_denies_nested_demuxer_resource_open) {
  if (!video::ffmpeg_video_ingest_available()) return;
  constexpr std::string_view nested_name = "codec-video-ffmpeg-nested.bmp";
  const auto nested_path = std::filesystem::current_path() / nested_name;
  const auto source_path = limit_test_path("secondary-open.ffconcat");
  const auto archive_path = limit_test_path("secondary-open.coda");
  std::filesystem::remove(nested_path);
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);

  const auto nested_fixture = bmp_2x2_fixture();
  EXPECT_TRUE(write_limit_bytes(nested_path, nested_fixture));
  const auto source_bytes = concat_secondary_open_fixture(nested_name);
  EXPECT_TRUE(write_limit_bytes(source_path, source_bytes));
  const auto stream = codec::derive_stream_id("video-ffmpeg-secondary-open");
  auto request = limit_request(source_path, archive_path, stream, "text/plain");

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->profile_error.has_value());
    expect_source_only_archive(*report, archive_path, stream, source_bytes);
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
  std::filesystem::remove(nested_path);
}
