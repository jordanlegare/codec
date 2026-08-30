#include "test.hpp"

#include <codec/profiles/video_export.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-export-" + std::string{name});
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> output;
  output.reserve(text.size());
  for (const unsigned char ch : text) {
    output.push_back(static_cast<std::byte>(ch));
  }
  return output;
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
  };
  output.pixels.assign(16U * 16U, static_cast<std::byte>(luma));
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  output.pixels.insert(output.pixels.end(), 8U * 8U, std::byte{0x80});
  return output;
}

codec::ProvenanceProcess video_process() {
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

struct ExportFixture {
  std::filesystem::path path;
  codec::StreamId stream;
};

ExportFixture make_export_archive(std::string_view name,
                                  bool provenanced = true) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(name);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "export fixture",
          .source_id = "fixture",
          .payload_type = "application/octet-stream",
      },
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              140'000'000, bytes("encoded source"));
  EXPECT_TRUE(source);

  const std::array starts{0LL, 40'000'000LL, 100'000'000LL};
  const std::array ends{40'000'000LL, 100'000'000LL, 140'000'000LL};
  const std::array lumas{std::uint8_t{0x20}, std::uint8_t{0x80},
                         std::uint8_t{0xd0}};
  for (std::size_t index = 0; index < starts.size(); ++index) {
    auto encoded = video::encode_raw_video_frame_state(frame(lumas[index]));
    EXPECT_TRUE(encoded);
    auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                   stream, starts[index], ends[index], *encoded);
    EXPECT_TRUE(state);
    if (provenanced) {
      const std::array inputs{*source};
      EXPECT_TRUE(writer.append_stream_provenance(
          *state, codec::TruthClass::state_exact, inputs, video_process()));
    }
  }
  EXPECT_TRUE(writer.finalize());
  return ExportFixture{path, stream};
}

bool has_ftyp(std::span<const std::byte> payload) {
  return payload.size() >= 8 && payload[4] == std::byte{'f'} &&
         payload[5] == std::byte{'t'} && payload[6] == std::byte{'y'} &&
         payload[7] == std::byte{'p'};
}

}  // namespace

TEST(video_export_verified_vfr1_to_mp4) {
  const auto fixture = make_export_archive("verified.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  const video::VideoFrameQuery query{
      .stream = fixture.stream,
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  auto exported = video::export_verified_video_mp4(
      *archive, query,
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  if (!video::ffmpeg_video_export_available()) {
    EXPECT_FALSE(exported);
    if (!exported) {
      EXPECT_EQ(exported.error().code, codec::ErrorCode::model_incompatible);
    }
    std::filesystem::remove(fixture.path);
    return;
  }
  EXPECT_TRUE(exported);
  if (exported) {
    EXPECT_EQ(exported->output.payload_type, std::string{"video/mp4"});
    EXPECT_TRUE(has_ftyp(exported->output.payload));
    EXPECT_EQ(exported->state_records.size(), std::size_t{3});
    EXPECT_EQ(exported->provenance.size(), std::size_t{3});
    EXPECT_EQ(exported->output.supporting_records.size(), std::size_t{3});
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_refuses_unverified_vfr1) {
  const auto fixture = make_export_archive("unverified.coda", false);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 1024 * 1024});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::invalid_argument);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_export_rejects_zero_output_limit) {
  const auto fixture = make_export_archive("zero-limit.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 1024 * 1024},
      video::VideoMp4ExportLimits{.maximum_output_bytes = 0});
  EXPECT_FALSE(exported);
  if (!exported) {
    EXPECT_EQ(exported.error().code, codec::ErrorCode::invalid_argument);
  }
  std::filesystem::remove(fixture.path);
}
