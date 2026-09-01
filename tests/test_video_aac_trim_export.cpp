#include "test.hpp"

#include <codec/profiles/video.hpp>
#include <codec/profiles/video_export.hpp>

#include <array>
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

std::filesystem::path trim_test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-aac-trim-" + std::string{name});
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

std::vector<std::byte> media_fixture(std::string_view name) {
  const auto path = std::filesystem::path{__FILE__}.parent_path() / "fixtures" /
                    std::string{name};
  std::ifstream input(path, std::ios::binary);
  const std::string encoded((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  return decode_base64(encoded);
}

bool write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

video::RawVideoFrameState frame(std::uint8_t luma) {
  video::RawVideoFrameState output{
      .descriptor = video::VideoProfileDescriptor{
          .coded_width = 16,
          .coded_height = 16,
          .pixel_layout = video::PixelLayout::yuv420p8,
          .sample_aspect_ratio_numerator = 1,
          .sample_aspect_ratio_denominator = 1,
          .nominal_frame_rate_numerator = 25,
          .nominal_frame_rate_denominator = 1,
          .color_range = video::ColorRange::limited,
          .color_primaries = video::ColorPrimaries::bt709,
          .transfer = video::TransferCharacteristics::bt709,
          .matrix = video::MatrixCoefficients::bt709,
      },
      .pixels = {},
  };
  output.pixels.assign(16U * 16U, static_cast<std::byte>(luma));
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  return output;
}

codec::ProvenanceProcess raw_video_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.raw-frame.canonicalize",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess encoded_audio_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.encoded-audio.preserve",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.encoded-audio.v1",
      .details = {std::byte{0x01}},
  };
}

std::uint64_t frames_to_ns(std::uint64_t frames, std::uint32_t sample_rate) {
  return (frames / sample_rate) * 1'000'000'000ULL +
         ((frames % sample_rate) * 1'000'000'000ULL) / sample_rate;
}

struct TrimArchive {
  std::filesystem::path path;
  codec::StreamId stream;
  video::EncodedAudioState audio;
};

TrimArchive make_trim_archive(const video::EncodedAudioState& source_state) {
  const auto path = trim_test_path("leading-trim.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("aac-leading-trim-export");

  auto state = source_state;
  EXPECT_EQ(state.trim_start_frames, std::uint64_t{0});
  const auto trim_frames = static_cast<std::uint64_t>(state.sample_rate / 100U);
  const auto presentation_frames =
      static_cast<std::uint64_t>(state.sample_rate / 10U);
  EXPECT_TRUE(trim_frames > 0U);
  EXPECT_TRUE(presentation_frames > 0U);
  EXPECT_TRUE(state.decoded_frames >= trim_frames + presentation_frames);
  const auto trim_ns = frames_to_ns(trim_frames, state.sample_rate);
  const auto presentation_ns =
      frames_to_ns(presentation_frames, state.sample_rate);
  EXPECT_TRUE(trim_ns > 0U);
  EXPECT_TRUE(presentation_ns > 0U);

  state.trim_start_frames = trim_frames;
  state.presentation_frames = presentation_frames;
  std::vector<video::EncodedAudioPacket> packets;
  for (auto packet : state.packets) {
    packet.pts_offset_ns -= static_cast<std::int64_t>(trim_ns);
    packet.dts_offset_ns -= static_cast<std::int64_t>(trim_ns);
    const auto packet_end =
        packet.pts_offset_ns + static_cast<std::int64_t>(packet.duration_ns);
    if (packet_end <= 0 ||
        packet.pts_offset_ns >= static_cast<std::int64_t>(presentation_ns)) {
      continue;
    }
    packets.push_back(std::move(packet));
  }
  state.packets = std::move(packets);
  EXPECT_TRUE(!state.packets.empty());
  EXPECT_TRUE(state.packets.front().pts_offset_ns < 0);
  EXPECT_TRUE(video::encode_encoded_audio_state(state));

  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "AAC leading trim regression",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      0));
  const std::array source_bytes{std::byte{0xaa}, std::byte{0xbb}};
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              400'000'000, source_bytes);
  EXPECT_TRUE(source);

  const std::array starts{0LL, 40'000'000LL, 100'000'000LL};
  const std::array ends{40'000'000LL, 100'000'000LL, 140'000'000LL};
  const std::array lumas{std::uint8_t{0x20}, std::uint8_t{0x80},
                         std::uint8_t{0xd0}};
  for (std::size_t index = 0U; index < starts.size(); ++index) {
    auto payload = video::encode_raw_video_frame_state(frame(lumas[index]));
    EXPECT_TRUE(payload);
    auto record = writer.append_raw(video::raw_video_frame_state_record_type,
                                    stream, starts[index], ends[index], *payload);
    EXPECT_TRUE(record);
    const std::array inputs{*source};
    EXPECT_TRUE(writer.append_stream_provenance(
        *record, codec::TruthClass::state_exact, inputs, raw_video_process()));
  }

  auto audio_payload = video::encode_encoded_audio_state(state);
  EXPECT_TRUE(audio_payload);
  auto audio_record = writer.append_raw(
      video::video_encoded_audio_state_record_type, stream, 0,
      static_cast<std::int64_t>(presentation_ns), *audio_payload);
  EXPECT_TRUE(audio_record);
  const std::array audio_inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *audio_record, codec::TruthClass::state_exact, audio_inputs,
      encoded_audio_process()));
  EXPECT_TRUE(writer.finalize());
  return TrimArchive{path, stream, std::move(state)};
}

}  // namespace

TEST(video_export_passthrough_represents_aac_leading_trim_with_mp4_edit_list) {
  if (!video::ffmpeg_video_ingest_available() ||
      !video::ffmpeg_video_export_available()) {
    return;
  }

  const auto source_path = trim_test_path("source.mp4");
  const auto source_archive_path = trim_test_path("source.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(source_archive_path);
  EXPECT_TRUE(write_bytes(source_path, media_fixture("video_audio_mono.mp4.b64")));
  const auto source_stream = codec::derive_stream_id("aac-leading-trim-source");
  auto ingested = video::ingest_video_ffmpeg(video::FfmpegVideoIngestRequest{
      .source_uri = source_path.string(),
      .archive_path = source_archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = source_stream,
          .type = codec::StreamType::video,
          .label = "AAC leading trim source",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 0,
      .end_ns = 250'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 8,
  });
  EXPECT_TRUE(ingested);
  if (!ingested) return;

  auto source_archive = codec::CodaArchive::open(source_archive_path);
  EXPECT_TRUE(source_archive);
  if (!source_archive) return;
  auto encoded = video::query_verified_video_encoded_audio(
      *source_archive,
      video::VideoAudioQuery{.stream = source_stream,
                             .time = std::nullopt,
                             .maximum_results = 1,
                             .maximum_encoded_bytes = 1024U * 1024U});
  EXPECT_TRUE(encoded);
  if (!encoded || encoded->size() != 1U) return;
  EXPECT_TRUE(!encoded->front().state.decoder_config.empty());

  auto fixture = make_trim_archive(encoded->front().state);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024U * 1024U},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024U * 1024U});
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_TRUE(exported->audio_packet_passthrough);
    EXPECT_TRUE(exported->audio_state_record.has_value());
  }

  std::filesystem::remove(fixture.path);
  std::filesystem::remove(source_archive_path);
  std::filesystem::remove(source_path);
}
