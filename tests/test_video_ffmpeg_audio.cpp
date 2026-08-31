#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path audio_ingest_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-audio-" + std::string{name});
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
          .label = "camera with sound",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 1'000'000'000,
      .end_ns = 1'250'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 8,
  };
}

void cleanup(const std::filesystem::path& source,
             const std::filesystem::path& archive) {
  std::filesystem::remove(archive);
  std::filesystem::remove(source);
}

}  // namespace

TEST(video_ffmpeg_audio_direct_ingest_writes_verified_pcm16) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("mono.mp4");
  const auto archive_path = audio_ingest_path("mono.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-mono");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->audio_present);
  EXPECT_TRUE(report->audio_state.has_value());
  EXPECT_TRUE(report->audio_provenance.has_value());
  EXPECT_TRUE(report->audio_state_exact());
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto audio = video::query_verified_video_pcm16_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1024,
          .maximum_encoded_bytes = 1024ULL * 1024ULL * 1024ULL,
      });
  EXPECT_TRUE(audio);
  if (audio && !audio->empty()) {
    EXPECT_EQ(audio->size(), std::size_t{1});
    EXPECT_EQ(audio->front().state.sample_rate, std::uint32_t{8000});
    EXPECT_EQ(audio->front().state.channels, std::uint16_t{1});
    EXPECT_TRUE(!audio->front().state.samples.empty());
    EXPECT_EQ(audio->front().source_records.size(), std::size_t{1});
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_ingest_reuses_existing_pcm16_encoding) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("pcm-reuse.mp4");
  const auto archive_path = audio_ingest_path("pcm-reuse.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-pcm-reuse");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (!report || !report->audio_state.has_value()) return;
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto payload = archive->read_payload(*report->audio_state);
  EXPECT_TRUE(payload);
  auto decoded = codec::decode_pcm16_state(*payload);
  EXPECT_TRUE(decoded);
  if (decoded) {
    EXPECT_EQ(decoded->sample_rate, std::uint32_t{8000});
    EXPECT_EQ(decoded->channels, std::uint16_t{1});
    EXPECT_TRUE(!decoded->samples.empty());
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_no_audio_remains_video_only_exact) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("video-only.mp4");
  const auto archive_path = audio_ingest_path("video-only.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_h264.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-video-only");
  auto request = request_for(source, archive_path, stream);
  request.end_ns = 2'000'000'000;
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_FALSE(report->audio_present);
    EXPECT_FALSE(report->audio_state.has_value());
    EXPECT_FALSE(report->audio_provenance.has_value());
    EXPECT_TRUE(report->audio_state_exact());
    EXPECT_TRUE(report->state_exact());
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_multichannel_is_profile_error_but_keeps_video_s1) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("multichannel.mp4");
  const auto archive_path = audio_ingest_path("multichannel.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_multichannel.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-multichannel");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->audio_present);
    EXPECT_FALSE(report->audio_state.has_value());
    EXPECT_FALSE(report->audio_provenance.has_value());
    EXPECT_TRUE(report->profile_error.has_value());
    EXPECT_FALSE(report->state_exact());
    EXPECT_TRUE(!report->states.empty());
    EXPECT_EQ(report->states.size(), report->provenance.size());
  }
  cleanup(source, archive_path);
}

TEST(video_ffmpeg_audio_direct_audio_limit_is_enforced) {
  if (!video::ffmpeg_video_ingest_available()) return;
  const auto source = audio_ingest_path("limit.mp4");
  const auto archive_path = audio_ingest_path("limit.coda");
  cleanup(source, archive_path);
  EXPECT_TRUE(write_bytes(source, fixture("video_audio_mono.mp4.b64")));
  const auto stream = codec::derive_stream_id("video-audio-direct-limit");
  auto request = request_for(source, archive_path, stream);
  request.maximum_decoded_audio_bytes = 2;
  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (report) {
    EXPECT_TRUE(report->audio_present);
    EXPECT_TRUE(report->profile_error.has_value());
    if (report->profile_error.has_value()) {
      EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
    }
    EXPECT_TRUE(!report->states.empty());
    EXPECT_FALSE(report->state_exact());
  }
  cleanup(source, archive_path);
}
