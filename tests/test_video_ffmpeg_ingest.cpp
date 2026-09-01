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

video::VideoFrameQuery query_for(const codec::StreamId& stream) {
  return video::VideoFrameQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024U * 1024U,
  };
}

}  // namespace

TEST(video_ffmpeg_ingest_preserves_mp4_and_emits_verified_encoded_h264) {
  const auto source_path = test_path("fixture.mp4");
  const auto archive_path = test_path("fixture.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  const auto source_bytes = fixture("video_4x4_h264.mp4.b64");
  EXPECT_TRUE(write_bytes(source_path, source_bytes));
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
  EXPECT_EQ(report->states.front().type_code(),
            video::video_encoded_video_state_record_type);

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  if (extracted) EXPECT_EQ(*extracted, source_bytes);

  auto raw_frames = video::query_verified_raw_video_frames(*archive, query_for(stream));
  EXPECT_TRUE(raw_frames);
  if (raw_frames) EXPECT_TRUE(raw_frames->empty());

  auto encoded = video::query_verified_video_encoded_video(*archive, query_for(stream));
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
      EXPECT_EQ(encoded->front().state_record.start_ns,
                std::int64_t{1'000'000'000});
      EXPECT_EQ(encoded->front().state_record.end_ns,
                std::int64_t{2'000'000'000});
      EXPECT_EQ(encoded->front().source_records.size(), std::size_t{1});
      if (!encoded->front().source_records.empty()) {
        EXPECT_EQ(encoded->front().source_records.front().hash,
                  report->source.hash);
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
    auto encoded =
        video::query_verified_video_encoded_video(*archive, query_for(stream));
    EXPECT_TRUE(encoded);
    if (encoded) EXPECT_TRUE(encoded->empty());
  }

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(video_ffmpeg_ingest_accepts_negative_archive_interval) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source_path = test_path("negative-time.mp4");
  const auto archive_path = test_path("negative-time.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  EXPECT_TRUE(write_bytes(source_path, fixture("video_4x4_h264.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-ffmpeg-negative-time");
  auto request = request_for(source_path, archive_path, stream);
  request.start_ns = -1'000'000'000;
  request.end_ns = 0;

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->state_exact());
    auto archive = codec::CodaArchive::open(archive_path);
    EXPECT_TRUE(archive);
    if (archive) {
      auto encoded =
          video::query_verified_video_encoded_video(*archive, query_for(stream));
      EXPECT_TRUE(encoded);
      if (encoded && !encoded->empty()) {
        EXPECT_EQ(encoded->front().state_record.start_ns,
                  std::int64_t{-1'000'000'000});
        EXPECT_EQ(encoded->front().state_record.end_ns, std::int64_t{0});
      }
    }
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
