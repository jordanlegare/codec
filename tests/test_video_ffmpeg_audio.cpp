#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/profiles/video.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

bool write_bytes(const std::filesystem::path& path,
                 const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

std::vector<std::byte> audiovisual_mono_fixture() {
  constexpr std::string_view encoded =
      "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";
  return decode_base64(encoded);
}

std::vector<std::byte> audiovisual_multichannel_fixture() {
  constexpr std::string_view encoded =
      "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";
  return decode_base64(encoded);
}

std::vector<std::byte> video_only_fixture() {
  constexpr std::string_view encoded =
      "AAAAHGZ0eXBpc29tAAACAGlzb21pc28ybXA0MQAAA2Rtb292AAAAbG12aGQAAAAAAAAAAAAAAAAAAAPoAAAD6AABAAABAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACAAACj3RyYWsAAABcdGtoZAAAAAMAAAAAAAAAAAAAAAEAAAAAAAAD6AAAAAAAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAEAAAAAAAAAAAAAAAAAAEAAAAAAEAAAABAAAAAAACRlZHRzAAAAHGVsc3QAAAAAAAAAAQAAA+gAAAAAAAEAAAAAAgdtZGlhAAAAIG1kaGQAAAAAAAAAAAAAAAAAAEAAAABAAFXEAAAAAAAtaGRscgAAAAAAAAAAdmlkZQAAAAAAAAAAAAAAAFZpZGVvSGFuZGxlcgAAAAGybWluZgAAABR2bWhkAAAAAQAAAAAAAAAAAAAAJGRpbmYAAAAcZHJlZgAAAAAAAAABAAAADHVybCAAAAABAAABcnN0YmwAAADqc3RzZAAAAAAAAAABAAAA2m1wNHYAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAEAAQAEgAAABIAAAAAAAAAAETTGF2YzYxLjE5LjEwMSBtcGVnNAAAAAAAAAAAAAAAGP//AAAAYGVzZHMAAAAAA4CAgE8AAQAEgICAQQghAAAAAAMNQAAAAUgFgICAIwAAAbABAAABtYkTAAABAAAAASAAxI2IACUAhAIUYwAAAaJMYXZjNjEuMTkuMTAxBoCAgAECAAAAEHBhc3AAAAABAAAAAQAAABRidHJ0AAAAAAADDVAAAAVIAA AAGHN0dHMAAAAAAAAAAQAAAAQAAEAAAAAAFHN0c3MAAAAAAAAAAQAAAAEAAAAUc3RzYwAAAAAAAAABAAAAAQAAAAQAAAABAAAAJHN0c3oAAAAAAAAAAAAAAAQAAAAUAAAAAQAAAAcAAAAHAAAAFHN0Y28AAAAAAAAAAQAAA5AAAABhdWR0YQAAAFltZXRhAAAAAAAAACFoZGxyAAAAAAAAAABtZGlyYXBwbAAAAAAAAAAAAAAAA CxpbHN0AAAAJKl0b28AAAAcZGF0YQAAAAEAAAAATGF2ZjYxLjcuMTAzAAAACGZyZWUAAAAxbWRhdAAAA bMAEAcAAAG2EwKMKDbBaBQfAAABtleBGwAAAbZbARsAAAG2X4Eb";
  std::string cleaned;
  cleaned.reserve(encoded.size());
  for (char ch : encoded) if (ch != ' ') cleaned.push_back(ch);
  return decode_base64(cleaned);
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
  EXPECT_TRUE(write_bytes(source, audiovisual_mono_fixture()));
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
  EXPECT_TRUE(write_bytes(source, audiovisual_mono_fixture()));
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
  EXPECT_TRUE(write_bytes(source, video_only_fixture()));
  const auto stream = codec::derive_stream_id("video-audio-direct-video-only");
  auto report = video::ingest_video_ffmpeg(request_for(source, archive_path, stream));
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
  EXPECT_TRUE(write_bytes(source, audiovisual_multichannel_fixture()));
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
  EXPECT_TRUE(write_bytes(source, audiovisual_mono_fixture()));
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
