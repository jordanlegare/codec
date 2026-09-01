#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path layout_test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-ffmpeg-layout-" + std::string{name});
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

bool write_layout_fixture(const std::filesystem::path& path,
                          const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
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

TEST(video_ffmpeg_ingest_supported_decode_layouts_do_not_change_evp1_bytes) {
  if (!video::ffmpeg_video_ingest_available()) return;

  const auto source_path = layout_test_path("source.mp4");
  std::filesystem::remove(source_path);
  const auto source_bytes = fixture("video_4x4_h264.mp4.b64");
  EXPECT_TRUE(write_layout_fixture(source_path, source_bytes));

  const std::vector<video::PixelLayout> layouts{
      video::PixelLayout::gray8,
      video::PixelLayout::rgb24,
      video::PixelLayout::rgba32,
      video::PixelLayout::yuv420p8,
  };
  std::optional<std::vector<std::byte>> first_evp1;

  for (std::size_t index = 0; index < layouts.size(); ++index) {
    const auto archive_path =
        layout_test_path("layout-" + std::to_string(index) + ".coda");
    std::filesystem::remove(archive_path);
    const auto stream = codec::derive_stream_id(
        "video-ffmpeg-layout-" + std::to_string(index));
    const video::FfmpegVideoIngestRequest request{
        .source_uri = source_path.string(),
        .archive_path = archive_path,
        .descriptor = codec::StreamDescriptor{
            .id = stream,
            .type = codec::StreamType::video,
            .label = "layout regression",
            .source_id = "fixture",
            .payload_type = "video/mp4",
        },
        .start_ns = 0,
        .end_ns = 1'000'000'000,
        .output_layout = layouts[index],
        .maximum_frames = 4,
    };

    auto report = video::ingest_video_ffmpeg(request);
    EXPECT_TRUE(report);
    if (report) {
      EXPECT_TRUE(report->state_exact());
      EXPECT_EQ(report->states.size(), std::size_t{1});
      EXPECT_EQ(report->states.front().type_code(),
                video::video_encoded_video_state_record_type);
      auto archive = codec::CodaArchive::open(archive_path);
      EXPECT_TRUE(archive);
      if (archive) {
        auto raw = video::query_verified_raw_video_frames(*archive, query_for(stream));
        EXPECT_TRUE(raw);
        if (raw) EXPECT_TRUE(raw->empty());
        auto encoded =
            video::query_verified_video_encoded_video(*archive, query_for(stream));
        EXPECT_TRUE(encoded);
        if (encoded && encoded->size() == 1U) {
          auto bytes = video::encode_encoded_video_state(encoded->front().state);
          EXPECT_TRUE(bytes);
          if (bytes) {
            if (!first_evp1.has_value()) {
              first_evp1 = *bytes;
            } else {
              EXPECT_EQ(*bytes, *first_evp1);
            }
          }
        }
      }
    }
    std::filesystem::remove(archive_path);
  }

  std::filesystem::remove(source_path);
}
