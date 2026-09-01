#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path reader_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-video-reader-" + std::string{name});
}

video::EncodedVideoState reader_state() {
  return video::EncodedVideoState{
      .codec = video::EncodedVideoCodec::h264,
      .framing = video::EncodedVideoPacketFraming::length_prefixed,
      .codec_profile = 100,
      .codec_level = 40,
      .coded_width = 640,
      .coded_height = 360,
      .sample_aspect_ratio_numerator = 1,
      .sample_aspect_ratio_denominator = 1,
      .validated_frames = 1,
      .presentation_lead_ns = 0,
      .decoder_config = {std::byte{0x01}, std::byte{0x64}, std::byte{0x00},
                         std::byte{0x28}},
      .packets = {video::EncodedVideoPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 100'000'000,
          .flags = 1,
          .payload = {std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
                      std::byte{0x02}, std::byte{0x65}, std::byte{0x88}},
      }},
  };
}

codec::ProvenanceProcess reader_process() {
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

struct ReaderFixture {
  std::filesystem::path path;
  codec::StreamId stream;
  codec::RecordInfo source;
  codec::RecordInfo state;
};

ReaderFixture make_reader_fixture(std::string_view name,
                                  codec::ProvenanceProcess process) {
  const auto path = reader_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(std::string{name});
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              100'000'000,
                              std::array{std::byte{0xaa}, std::byte{0xbb}});
  auto encoded = video::encode_encoded_video_state(reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_video_state_record_type,
                                 stream, 0, 100'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, std::move(process)));
  EXPECT_TRUE(writer.finalize());
  return ReaderFixture{.path = path,
                       .stream = stream,
                       .source = *source,
                       .state = *state};
}

}  // namespace

TEST(video_encoded_video_reader_accepts_exact_direct_lineage) {
  const auto fixture =
      make_reader_fixture("encoded-video-reader-direct", reader_process());
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_video(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 4096});
  EXPECT_TRUE(verified);
  if (verified && !verified->empty()) {
    EXPECT_EQ(verified->size(), std::size_t{1});
    EXPECT_EQ(verified->front().state, reader_state());
    EXPECT_EQ(verified->front().state_record.hash, fixture.state.hash);
    EXPECT_EQ(verified->front().source_records.size(), std::size_t{1});
    EXPECT_EQ(verified->front().source_records.front().hash,
              fixture.source.hash);
    EXPECT_EQ(verified->front().provenance.process.operation,
              std::string{"codec.video.encoded-video.preserve"});
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_encoded_video_reader_rejects_forged_process_contract) {
  auto process = reader_process();
  process.operation = "codec.video.raw-frame.canonicalize";
  const auto fixture =
      make_reader_fixture("encoded-video-reader-forged", std::move(process));
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_video(
      *archive,
      video::VideoFrameQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = 8,
                             .maximum_encoded_bytes = 4096});
  EXPECT_FALSE(verified);
  if (!verified) {
    EXPECT_EQ(verified.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(fixture.path);
}
