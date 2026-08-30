#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path layout_test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-ffmpeg-layout-" + std::string{name});
}

std::vector<std::byte> bmp_2x2_layout_fixture() {
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

bool write_layout_fixture(const std::filesystem::path& path,
                          const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::size_t expected_layout_bytes(video::PixelLayout layout) {
  switch (layout) {
    case video::PixelLayout::gray8: return 4U;
    case video::PixelLayout::rgb24: return 12U;
    case video::PixelLayout::rgba32: return 16U;
    case video::PixelLayout::yuv420p8: return 6U;
  }
  return 0U;
}

}  // namespace

TEST(video_ffmpeg_ingest_canonicalizes_every_supported_layout_safely) {
  if (!video::ffmpeg_video_ingest_available()) return;

  const auto source_path = layout_test_path("source.bmp");
  std::filesystem::remove(source_path);
  const auto fixture = bmp_2x2_layout_fixture();
  EXPECT_TRUE(write_layout_fixture(source_path, fixture));

  const std::vector<video::PixelLayout> layouts{
      video::PixelLayout::gray8,
      video::PixelLayout::rgb24,
      video::PixelLayout::rgba32,
      video::PixelLayout::yuv420p8,
  };

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
            .payload_type = "image/bmp",
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
      auto archive = codec::CodaArchive::open(archive_path);
      EXPECT_TRUE(archive);
      if (archive) {
        auto frames = video::query_verified_raw_video_frames(*archive);
        EXPECT_TRUE(frames);
        if (frames) {
          EXPECT_EQ(frames->size(), std::size_t{1});
          if (!frames->empty()) {
            EXPECT_EQ(frames->front().state.descriptor.pixel_layout,
                      layouts[index]);
            EXPECT_EQ(frames->front().state.pixels.size(),
                      expected_layout_bytes(layouts[index]));
          }
        }
      }
    }
    std::filesystem::remove(archive_path);
  }

  std::filesystem::remove(source_path);
}
