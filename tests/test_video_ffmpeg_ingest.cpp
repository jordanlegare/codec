#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-ffmpeg-" + std::string{name});
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

std::vector<std::byte> mp4_h264_fixture() {
  constexpr std::string_view encoded =
      "AAAAIGZ0eXBpc29tAAACAGlzb21pc28yYXZjMW1wNDEAAAMAbW9vdgAAAGxtdmhkAAAAAAAAAAAAAAAAAAAD6AAAA+gAAQAAAQAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgAAAit0cmFrAAAAXHRraGQAAAADAAAAAAAAAAAAAAABAAAAAAAAA+gAAAAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAABAAAAAAAAAAAAAAAAAABAAAAAAAQAAAAEAAAAAAAkZWR0cwAAABxlbHN0AAAAAAAAAAEAAAPoAAAAAAABAAAAAAGjbWRpYQAAACBtZGhkAAAAAAAAAAAAAAAAAABAAAAAQABVxAAAAAAALWhkbHIAAAAAAAAAAHZpZGUAAAAAAAAAAAAAAABWaWRlb0hhbmRsZXIAAAABTm1pbmYAAAAUdm1oZAAAAAEAAAAAAAAAAAAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAQ5zdGJsAAAAqnN0c2QAAAAAAAAAAQAAAJphdmMxAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAQABABIAAAASAAAAAAAAAABFUxhdmM2MS4xOS4xMDEgbGlieDI2NAAAAAAAAAAAAAAAGP//AAAAMGF2Y0MB9BAK/+EAFGf0EAquu/PPCAAAAwAIAAADABAgAQAFaM4BryD9+PgAAAAAFGJ0cnQAAAAAAAAmQAAAAAAAAAAYc3R0cwAAAAAAAAABAAAAAQAAQAAAAAAcc3RzYwAAAAAAAAABAAAAAQAAAAEAAAABAAAAFHN0c3oAAAAAAAAEyAAAAAEAAAAUc3RjbwAAAAAAAAABAAADMAAAAGF1ZHRhAAAAWW1ldGEAAAAAAAAAIWhkbHIAAAAAAAAAAG1kaXJhcHBsAAAAAAAAAAAAAAAALGlsc3QAAAAkqXRvbwAAABxkYXRhAAAAAQAAAABMYXZmNjEuNy4xMDMAAAAIZnJlZQAABNBtZGF0AAACBAYF//8A3EXpvebZSLeWLNgg2SPu73gyNjQgLSBjb3JlIDE2NCByMzEwOCAzMWUxOWY5IC0gSC4yNjQvTVBFRy00IEFWQyBjb2RlYyAtIENvcHlsZWZ0IDIwMDMtMjAyMyAtIGh0dHA6Ly93d3cudmlkZW9sYW4ub3JnL3gyNjQuaHRtbCAtIG9wdGlvbnM6IGNhYmFjPTAgcmVmPTEgZGVibG9jaz0wOjA6MCBhbmFseXNlPTA6MCBtZT1kaWEgc3VibWU9MCBwc3k9MCBtaXhlZF9yZWY9MCBtZV9yYW5nZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTAgOHg4ZGN0PTAgY3FtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0wIGNocm9tYV9xcF9vZmZzZXQ9MCB0aHJlYWRzPTEgbG9va2FoZWFkX3RocmVhZHM9MSBzbGljZWRfdGhyZWFkcz0wIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MSBrZXlpbnRfbWluPTEgc2NlbmVjdXQ9MCBpbnRyYV9yZWZyZXNoPTAgcmM9Y3FwIG1idHJlZT0wIHFwPTAAgAAAArxliISgxgAIAAIUIAAhCgACCSABECEQRHEEQRZFkQRHEcWRfAAcAAQoQABCHAAEFEABECsRRHE0UxZFsVRPFcXeAABChAAEIUAAQUQAEQKRBEESRRFkWRREkURZ4AAEKEAAQhwABBJAAiBGIojiKIYgiGIojiOIvAAAhQgACEKAAIJIAEQIRBEEQRBEEQRBEEQRB4AAEKEAAQhQABBRAARApEEQRJFEWRZFESRRFngAAQoQABCFAAEFEABECkQRBEkURZFkURJFEWeAABChAAEIUAAQSQAIgQiCIIgiCIIgiCIIgiDwAAIUIAAhCgACCSABECEQRBEEQRBEEQRBEEQeAABChAAEIcAAQSQAIgRiKI4iiGIIhiKI4jiLwAAIUIAAhCgACCSABECEQRBEEQRBEEQRBEEQeAABChAAEIcAAQSQAIgRiKI4iiGIIhiKI4jiLwAAIUIAAhCgACCSABECEQRBEEQRBEEQRBEEQeAABChAAEIUAAQSQAIgQiCIIgiCIIgiCIIgiDwAAIUIAAhCgACCSABECEQRBEEQRBEEQRBEEQeAABChAAEIUAAQSQAIgQiCIIgiCIIgiCIIgiDwAAIUIAAhCgACCSABECEQRBEEQRBEEQRBEEQQgABBZAAED8AAQBQHwgABBkAAEEYAAQCAHgAHAAEFkAAQPQABABAZHLy9vLy+vry9vb7wAAILIAAgegACACAyOXl5eXl9fXl5eX3gAAQWQABA9AAEAEBkcvL28vLy8vL29vPAAAgsgACB6AAIAIDI5eXl5eXl5eXl5eQAHAAEGQAAQSAABAMAKBIRCERCIQCARCEQiBwAAIMgAAgkAACAYAUCQiEQiEQgEAiEQiEDgAAQZAABBIAAEAwAoEhEIREIhEIhEIRCInAAAgyAACCQAAIBgBQJCIRCIRCIRCIRCIRI";
  return decode_base64(encoded);
}

std::vector<std::byte> expected_yuv420p8() {
  return {
      std::byte{0x10}, std::byte{0x11}, std::byte{0x12}, std::byte{0x13},
      std::byte{0x14}, std::byte{0x15}, std::byte{0x16}, std::byte{0x17},
      std::byte{0x18}, std::byte{0x19}, std::byte{0x1a}, std::byte{0x1b},
      std::byte{0x1c}, std::byte{0x1d}, std::byte{0x1e}, std::byte{0x1f},
      std::byte{0x40}, std::byte{0x41}, std::byte{0x42}, std::byte{0x43},
      std::byte{0xc0}, std::byte{0xc1}, std::byte{0xc2}, std::byte{0xc3},
  };
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
          .label = "ffmpeg fixture",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 1'000'000'000,
      .end_ns = 2'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
  };
}

}  // namespace

TEST(video_ffmpeg_ingest_preserves_mp4_and_emits_verified_yuv420p8) {
  const auto source_path = test_path("fixture.mp4");
  const auto archive_path = test_path("fixture.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto fixture = mp4_h264_fixture();
  EXPECT_TRUE(write_bytes(source_path, fixture));
  const auto stream = codec::derive_stream_id("video-ffmpeg-fixture");
  const auto request = request_for(source_path, archive_path, stream);

  auto report = video::ingest_video_ffmpeg(request);
  if (!video::ffmpeg_video_ingest_available()) {
    EXPECT_FALSE(report);
    if (!report) {
      EXPECT_EQ(report.error().code, codec::ErrorCode::model_incompatible);
    }
    EXPECT_FALSE(std::filesystem::exists(archive_path));
    std::filesystem::remove(source_path);
    return;
  }

  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_EQ(report->states.size(), std::size_t{1});
  EXPECT_EQ(report->provenance.size(), std::size_t{1});

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  if (extracted) EXPECT_EQ(*extracted, fixture);

  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) {
    EXPECT_EQ(frames->size(), std::size_t{1});
    if (!frames->empty()) {
      const auto& frame = frames->front();
      EXPECT_EQ(frame.state.descriptor.coded_width, std::uint32_t{4});
      EXPECT_EQ(frame.state.descriptor.coded_height, std::uint32_t{4});
      EXPECT_EQ(frame.state.descriptor.pixel_layout,
                video::PixelLayout::yuv420p8);
      EXPECT_EQ(frame.state.pixels, expected_yuv420p8());
      EXPECT_EQ(frame.state_record.start_ns, std::int64_t{1'000'000'000});
      EXPECT_EQ(frame.state_record.end_ns, std::int64_t{2'000'000'000});
      EXPECT_EQ(frame.source_records.size(), std::size_t{1});
      if (!frame.source_records.empty()) {
        EXPECT_EQ(frame.source_records.front().hash, report->source.hash);
      }
    }
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_ffmpeg_ingest_preserves_source_when_decode_fails) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = test_path("malformed.bin");
  const auto archive_path = test_path("malformed.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const std::vector<std::byte> malformed{
      std::byte{'n'}, std::byte{'o'}, std::byte{'t'}, std::byte{' '},
      std::byte{'m'}, std::byte{'e'}, std::byte{'d'}, std::byte{'i'},
      std::byte{'a'},
  };
  EXPECT_TRUE(write_bytes(source_path, malformed));
  const auto stream = codec::derive_stream_id("video-ffmpeg-malformed");
  auto request = request_for(source_path, archive_path, stream);
  request.descriptor.payload_type = "application/octet-stream";

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_FALSE(report->state_exact());
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_TRUE(report->states.empty());
  EXPECT_TRUE(report->provenance.empty());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive) {
    EXPECT_TRUE(archive->verify().ok);
    auto extracted = archive->extract_stream(stream);
    EXPECT_TRUE(extracted);
    if (extracted) EXPECT_EQ(*extracted, malformed);
    auto frames = video::query_verified_raw_video_frames(*archive);
    EXPECT_TRUE(frames);
    if (frames) EXPECT_TRUE(frames->empty());
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_ffmpeg_ingest_validates_interval_before_backend_use) {
  const auto source_path = test_path("invalid.mp4");
  const auto archive_path = test_path("invalid.coda");
  std::filesystem::remove(archive_path);
  const auto stream = codec::derive_stream_id("video-ffmpeg-invalid");
  auto request = request_for(source_path, archive_path, stream);
  request.end_ns = request.start_ns;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_FALSE(report);
  if (!report) {
    EXPECT_EQ(report.error().code, codec::ErrorCode::invalid_argument);
  }
  EXPECT_FALSE(std::filesystem::exists(archive_path));
}
