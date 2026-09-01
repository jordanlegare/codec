#include "test.hpp"
#include "hls_http_fixture.hpp"

#include <codec/archive.hpp>
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

std::filesystem::path export_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-export-" + std::string{name});
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

std::vector<std::byte> bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
}

bool write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> payload) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
  return output.good();
}

HlsHttpFixture hls_av_server() {
  const auto master = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"mono\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=100000,CODECS=\"avc1.42c00a,mp4a.40.2\",AUDIO=\"audio\"\n"
      "video.m3u8\n");
  const auto video_manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:1.000000,\n"
      "video.ts\n"
      "#EXT-X-ENDLIST\n");
  const auto audio_manifest = bytes(
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-MEDIA-SEQUENCE:0\n"
      "#EXTINF:0.250000,\n"
      "audio.ts\n"
      "#EXT-X-ENDLIST\n");
  return HlsHttpFixture({
      {"/master.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = master}},
      {"/video.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = video_manifest}},
      {"/audio.m3u8",
       HlsHttpResponse{.status = 200,
                       .content_type = "application/vnd.apple.mpegurl",
                       .body = audio_manifest}},
      {"/video.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = fixture("hls_4x4_seg0.ts.b64")}},
      {"/audio.ts",
       HlsHttpResponse{.status = 200,
                       .content_type = "video/mp2t",
                       .body = fixture("hls_audio_mono.ts.b64")}},
  });
}

video::VideoFrameQuery video_query(const codec::StreamId& stream) {
  return video::VideoFrameQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 4U * 1024U * 1024U,
  };
}

video::VideoAudioQuery audio_query(const codec::StreamId& stream) {
  return video::VideoAudioQuery{
      .stream = stream,
      .time = std::nullopt,
      .maximum_results = 8,
      .maximum_encoded_bytes = 4U * 1024U * 1024U,
  };
}

bool has_mp4_ftyp(const std::vector<std::byte>& payload) {
  return payload.size() >= 8U && payload[4] == std::byte{'f'} &&
         payload[5] == std::byte{'t'} && payload[6] == std::byte{'y'} &&
         payload[7] == std::byte{'p'};
}

codec::ProvenanceProcess encoded_video_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.encoded-video.preserve",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = "application/vnd.codec.video.encoded-video.v1",
      .details = {std::byte{0x01}},
  };
}

struct EncodedArchiveFixture {
  std::filesystem::path path;
  codec::StreamId stream;
};

EncodedArchiveFixture make_unrecoverable_annex_b_fixture() {
  const auto path = export_path("unrecoverable-annex-b.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("unrecoverable-annex-b");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "unrecoverable",
                              .source_id = "fixture",
                              .payload_type = "video/mp2t"},
      0));
  const std::array source_bytes{std::byte{0xaa}, std::byte{0xbb}};
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              1'000'000'000, source_bytes);
  EXPECT_TRUE(source);
  const video::EncodedVideoState state{
      .codec = video::EncodedVideoCodec::h264,
      .framing = video::EncodedVideoPacketFraming::annex_b,
      .codec_profile = 66,
      .codec_level = 10,
      .coded_width = 4,
      .coded_height = 4,
      .sample_aspect_ratio_numerator = 1,
      .sample_aspect_ratio_denominator = 1,
      .validated_frames = 1,
      .presentation_lead_ns = 0,
      .decoder_config = {},
      .packets = {video::EncodedVideoPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 1'000'000'000,
          .flags = 1,
          .payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                      std::byte{0x01}, std::byte{0x65}, std::byte{0x88}},
      }},
  };
  auto payload = video::encode_encoded_video_state(state);
  EXPECT_TRUE(payload);
  auto state_record = writer.append_raw(
      video::video_encoded_video_state_record_type, stream, 0,
      1'000'000'000, *payload);
  EXPECT_TRUE(state_record);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state_record, codec::TruthClass::state_exact, inputs,
      encoded_video_process()));
  EXPECT_TRUE(writer.finalize());
  return EncodedArchiveFixture{.path = path, .stream = stream};
}

}  // namespace

TEST(video_export_remuxes_hls_annex_b_h264_and_adts_aac) {
  if (!video::ffmpeg_video_ingest_available()) return;
  auto server = hls_av_server();
  const auto archive_path = export_path("hls.coda");
  const auto output_path = export_path("hls-roundtrip.mp4");
  const auto roundtrip_archive_path = export_path("hls-roundtrip.coda");
  std::filesystem::remove(archive_path);
  std::filesystem::remove(output_path);
  std::filesystem::remove(roundtrip_archive_path);
  const auto stream = codec::derive_stream_id("video-export-hls-passthrough");
  const video::FfmpegVideoIngestRequest request{
      .source_uri = server.url("/master.m3u8"),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "HLS export passthrough",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      .start_ns = 0,
      .end_ns = 1'000'000'000,
      .output_layout = video::PixelLayout::yuv420p8,
      .maximum_frames = 4,
      .deny_private_network = false,
  };

  auto report = video::ingest_video_ffmpeg(request);
  EXPECT_TRUE(report);
  if (!report) return;
  EXPECT_TRUE(report->state_exact());
  EXPECT_TRUE(report->audio_state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto encoded_video =
      video::query_verified_video_encoded_video(*archive, video_query(stream));
  EXPECT_TRUE(encoded_video);
  if (encoded_video) EXPECT_EQ(encoded_video->size(), std::size_t{1});
  if (encoded_video && encoded_video->size() == 1U) {
    EXPECT_EQ(encoded_video->front().state.framing,
              video::EncodedVideoPacketFraming::annex_b);
  }
  auto encoded_audio =
      video::query_verified_video_encoded_audio(*archive, audio_query(stream));
  EXPECT_TRUE(encoded_audio);
  if (encoded_audio) EXPECT_EQ(encoded_audio->size(), std::size_t{1});
  if (encoded_audio && encoded_audio->size() == 1U) {
    EXPECT_TRUE(encoded_audio->front().state.decoder_config.empty());
    EXPECT_TRUE(!encoded_audio->front().state.packets.empty());
  }

  auto exported = video::export_verified_video_mp4(
      *archive, video_query(stream),
      video::VideoMp4ExportLimits{.maximum_output_bytes = 4U * 1024U * 1024U});
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_TRUE(has_mp4_ftyp(exported->output.payload));
    EXPECT_TRUE(exported->video_packet_passthrough);
    EXPECT_TRUE(exported->audio_packet_passthrough);
    EXPECT_TRUE(!exported->state_records.empty());
    EXPECT_TRUE(exported->audio_state_record.has_value());
    EXPECT_TRUE(write_bytes(output_path, exported->output.payload));

    const auto roundtrip_stream =
        codec::derive_stream_id("video-export-hls-roundtrip");
    auto roundtrip = video::ingest_video_ffmpeg(video::FfmpegVideoIngestRequest{
        .source_uri = output_path.string(),
        .archive_path = roundtrip_archive_path,
        .descriptor = codec::StreamDescriptor{
            .id = roundtrip_stream,
            .type = codec::StreamType::video,
            .label = "roundtrip",
            .source_id = "fixture",
            .payload_type = "video/mp4",
        },
        .start_ns = 0,
        .end_ns = 1'000'000'000,
        .output_layout = video::PixelLayout::yuv420p8,
        .maximum_frames = 4,
    });
    EXPECT_TRUE(roundtrip);
    if (roundtrip) {
      EXPECT_TRUE(roundtrip->state_exact());
      EXPECT_TRUE(roundtrip->audio_present);
      EXPECT_TRUE(roundtrip->audio_state_exact());
    }
  }

  std::filesystem::remove(roundtrip_archive_path);
  std::filesystem::remove(output_path);
  std::filesystem::remove(archive_path);
}

TEST(video_export_rejects_unrecoverable_annex_b_configuration) {
  const auto fixture = make_unrecoverable_annex_b_fixture();
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  auto exported = video::export_verified_video_mp4(
      *archive, video_query(fixture.stream),
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024U * 1024U});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::model_incompatible);
  }
  std::filesystem::remove(fixture.path);
}
