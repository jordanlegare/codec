#include "test.hpp"

#include <codec/profiles/video.hpp>

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
         ("codec-h1-video-" + std::string{name});
}

std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> output;
  output.reserve(text.size());
  for (const unsigned char ch : text) {
    output.push_back(static_cast<std::byte>(ch));
  }
  return output;
}

video::RawVideoFrameState exact_frame() {
  return video::RawVideoFrameState{
      .descriptor = video::VideoProfileDescriptor{
          .coded_width = 2,
          .coded_height = 2,
          .pixel_layout = video::PixelLayout::rgb24,
          .sample_aspect_ratio_numerator = 1,
          .sample_aspect_ratio_denominator = 1,
          .nominal_frame_rate_numerator = 24,
          .nominal_frame_rate_denominator = 1,
          .color_range = video::ColorRange::full,
          .color_primaries = video::ColorPrimaries::bt709,
          .transfer = video::TransferCharacteristics::bt709,
          .matrix = video::MatrixCoefficients::bt709,
      },
      .pixels = {
          std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
          std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
          std::byte{0x07}, std::byte{0x08}, std::byte{0x09},
          std::byte{0x0a}, std::byte{0x0b}, std::byte{0x0c},
      },
  };
}

codec::ProvenanceProcess video_process(std::string operation =
                                           "codec.video.raw-frame.canonicalize") {
  return codec::ProvenanceProcess{
      .operation = std::move(operation),
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 500,
      .details_type = "application/vnd.codec.video.canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

codec::ProvenanceProcess hls_video_process() {
  return codec::ProvenanceProcess{
      .operation = "codec.video.raw-frame.canonicalize.hls",
      .implementation_id = "codec.video",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 500,
      .details_type =
          "application/vnd.codec.video.hls-canonicalization.v1",
      .details = {std::byte{0x01}},
  };
}

struct HlsReaderFixtureOptions {
  bool include_primary_input{true};
  bool include_child_descriptor{true};
  codec::StreamType child_stream_type{codec::StreamType::opaque};
  std::string child_source_id{"codec.video.hls-resource"};
  bool duplicate_child_stream{false};
  bool non_source_child{false};
  bool direct_process{false};
};

struct HlsReaderFixture {
  std::filesystem::path path;
  codec::StreamId parent_stream;
  codec::StreamId child_stream;
  codec::RecordInfo primary_source;
  codec::RecordInfo child_source;
  codec::RecordInfo state;
};

HlsReaderFixture make_hls_reader_archive(
    std::string_view name, HlsReaderFixtureOptions options = {}) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto parent_stream = codec::derive_stream_id(
      std::string{name} + "/parent");
  const auto child_stream = codec::derive_stream_id(
      std::string{name} + "/child");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = parent_stream,
          .type = codec::StreamType::video,
          .label = "HLS parent",
          .source_id = "fixture",
          .payload_type = "application/vnd.apple.mpegurl",
      },
      10));
  if (options.include_child_descriptor) {
    EXPECT_TRUE(writer.append_stream_descriptor(
        codec::StreamDescriptor{
            .id = child_stream,
            .type = options.child_stream_type,
            .label = "HLS parent:hls-resource-0000",
            .source_id = options.child_source_id,
            .payload_type = "application/octet-stream",
        },
        10));
  }

  auto primary_source = writer.append(
      codec::RecordType::source_bytes, parent_stream, 10, 20,
      bytes("#EXTM3U\n#EXT-X-VERSION:3\n"));
  EXPECT_TRUE(primary_source);

  codec::Result<codec::RecordInfo> child_source =
      options.non_source_child
          ? writer.append_raw(video::video_profile_descriptor_record_type,
                              child_stream, 10, 20,
                              bytes("not source bytes"))
          : writer.append(codec::RecordType::source_bytes, child_stream, 10,
                          20, bytes("segment zero"));
  EXPECT_TRUE(child_source);

  std::optional<codec::RecordInfo> duplicate_child;
  if (options.duplicate_child_stream) {
    auto appended = writer.append(codec::RecordType::source_bytes,
                                  child_stream, 10, 20,
                                  bytes("segment duplicate"));
    EXPECT_TRUE(appended);
    if (appended) duplicate_child = *appended;
  }

  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 parent_stream, 10, 20, *encoded);
  EXPECT_TRUE(state);

  std::vector<codec::RecordInfo> inputs;
  if (options.include_primary_input) inputs.push_back(*primary_source);
  inputs.push_back(*child_source);
  if (duplicate_child.has_value()) inputs.push_back(*duplicate_child);
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      options.direct_process ? video_process() : hls_video_process()));
  EXPECT_TRUE(writer.finalize());
  return HlsReaderFixture{
      .path = path,
      .parent_stream = parent_stream,
      .child_stream = child_stream,
      .primary_source = *primary_source,
      .child_source = *child_source,
      .state = *state,
  };
}

struct VerifiedFixture {
  std::filesystem::path path;
  codec::StreamId stream;
  std::vector<codec::RecordInfo> sources;
  codec::RecordInfo state;
};

VerifiedFixture make_verified_archive(std::string_view name,
                                      bool multiple_sources = false) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(name);
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "H.1 video",
          .source_id = "fixture",
          .payload_type = "application/vnd.codec.video.raw-frame.v1",
      },
      10));

  std::vector<codec::RecordInfo> sources;
  auto first = writer.append(codec::RecordType::source_bytes, stream, 10,
                             multiple_sources ? 15 : 20, bytes("source-a"));
  EXPECT_TRUE(first);
  sources.push_back(*first);
  if (multiple_sources) {
    auto second = writer.append(codec::RecordType::source_bytes, stream, 15,
                                20, bytes("source-b"));
    EXPECT_TRUE(second);
    sources.push_back(*second);
  }

  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 10, 20, *encoded);
  EXPECT_TRUE(state);
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, sources, video_process()));
  EXPECT_TRUE(writer.finalize());
  return VerifiedFixture{path, stream, std::move(sources), *state};
}

}  // namespace

TEST(video_state_reader_returns_exact_verified_s1) {
  const auto fixture = make_verified_archive("verified.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  const video::VideoFrameQuery query{
      .stream = fixture.stream,
      .time = codec::RecordTimeRange{.begin_ns = 10, .end_ns = 20},
      .maximum_results = 4,
      .maximum_encoded_bytes = 1024,
  };
  auto frames = video::query_verified_raw_video_frames(*archive, query);
  EXPECT_TRUE(frames);
  if (!frames) return;
  EXPECT_EQ(frames->size(), std::size_t{1});
  EXPECT_EQ(frames->front().state, exact_frame());
  EXPECT_EQ(frames->front().state_record.sequence, fixture.state.sequence);
  EXPECT_EQ(frames->front().state_record.hash, fixture.state.hash);
  EXPECT_EQ(frames->front().source_records.size(), std::size_t{1});
  EXPECT_EQ(frames->front().source_records.front().hash,
            fixture.sources.front().hash);
  EXPECT_EQ(frames->front().provenance.subject_truth,
            codec::TruthClass::state_exact);
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_ignores_unprovenanced_vfr1) {
  const auto path = test_path("unprovenanced.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-unprovenanced");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(encoded);
  EXPECT_TRUE(writer.append_raw(video::raw_video_frame_state_record_type,
                                stream, 1, 2, *encoded));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) EXPECT_TRUE(frames->empty());
  std::filesystem::remove(path);
}

TEST(video_state_reader_ignores_non_s1_vfr1) {
  const auto path = test_path("derived.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-derived");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("source"));
  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 1, 2, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::derived, inputs, video_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) EXPECT_TRUE(frames->empty());
  std::filesystem::remove(path);
}

TEST(video_state_reader_rejects_wrong_process_contract) {
  const auto path = test_path("wrong-process.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-wrong-process");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("source"));
  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 1, 2, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      video_process("codec.video.resize")));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(path);
}

TEST(video_state_reader_rejects_wrong_process_details_version) {
  const auto path = test_path("wrong-process-details.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-wrong-details");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("source"));
  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 1, 2, *encoded);
  EXPECT_TRUE(state);
  auto process = video_process();
  process.details = {std::byte{0x02}};
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, process));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(path);
}

TEST(video_state_reader_rejects_cross_stream_or_non_source_inputs) {
  const auto path = test_path("wrong-input.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-state-stream");
  const auto other = codec::derive_stream_id("video-source-stream");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto input = writer.append(codec::RecordType::source_bytes, other, 10, 20,
                             bytes("source"));
  auto encoded = video::encode_raw_video_frame_state(exact_frame());
  EXPECT_TRUE(input);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 10, 20, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*input};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, video_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(path);

  const auto non_source_path = test_path("non-source-input.coda");
  std::filesystem::remove(non_source_path);
  writer = std::move(*codec::CodaWriter::create(non_source_path));
  auto descriptor = video::encode_video_profile_descriptor(
      exact_frame().descriptor);
  EXPECT_TRUE(descriptor);
  input = writer.append_raw(video::video_profile_descriptor_record_type,
                            stream, 10, 20, *descriptor);
  state = writer.append_raw(video::raw_video_frame_state_record_type, stream,
                            10, 20, *encoded);
  EXPECT_TRUE(input);
  EXPECT_TRUE(state);
  const std::array non_source_inputs{*input};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, non_source_inputs,
      video_process()));
  EXPECT_TRUE(writer.finalize());
  archive = codec::CodaArchive::open(non_source_path);
  EXPECT_TRUE(archive);
  frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(non_source_path);
}

TEST(video_state_reader_accepts_multiple_exact_s0_inputs) {
  const auto fixture = make_verified_archive("multiple-sources.coda", true);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) {
    EXPECT_EQ(frames->size(), std::size_t{1});
    EXPECT_EQ(frames->front().source_records.size(), std::size_t{2});
    EXPECT_EQ(frames->front().source_records[0].hash,
              fixture.sources[0].hash);
    EXPECT_EQ(frames->front().source_records[1].hash,
              fixture.sources[1].hash);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_rejects_malformed_vfr1_as_archive_corrupt) {
  const auto path = test_path("malformed.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("video-malformed");
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              bytes("source"));
  auto state = writer.append_raw(video::raw_video_frame_state_record_type,
                                 stream, 1, 2, bytes("not VFR1"));
  EXPECT_TRUE(source);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, video_process()));
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(path);
}

TEST(video_state_reader_enforces_result_byte_and_decode_limits) {
  const auto fixture = make_verified_archive("limits.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  video::VideoFrameQuery query;
  query.maximum_results = 0;
  auto frames = video::query_verified_raw_video_frames(*archive, query);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::invalid_argument);
  }

  query = {};
  query.maximum_encoded_bytes = 1;
  frames = video::query_verified_raw_video_frames(*archive, query);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::resource_exhausted);
  }

  query = {};
  query.decode_limits.maximum_width = 1;
  frames = video::query_verified_raw_video_frames(*archive, query);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::resource_exhausted);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_preserves_unknown_future_profile_records) {
  constexpr codec::RecordTypeCode future_type = 0x0102;
  const auto source_path = test_path("future.coda");
  const auto repaired_path = test_path("future-repaired.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(repaired_path);
  const auto stream = codec::derive_stream_id("video-future");
  const auto payload = bytes("future video profile payload");
  auto writer = std::move(*codec::CodaWriter::create(source_path));
  EXPECT_TRUE(writer.append_raw(future_type, stream, 1, 2, payload));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(source_path);
  EXPECT_TRUE(archive);
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream_raw(stream, future_type);
  EXPECT_TRUE(extracted);
  if (extracted) EXPECT_EQ(*extracted, payload);

  auto repaired = codec::CodaArchive::repair(source_path, repaired_path);
  EXPECT_TRUE(repaired);
  auto repaired_archive = codec::CodaArchive::open(repaired_path);
  EXPECT_TRUE(repaired_archive);
  EXPECT_TRUE(repaired_archive->verify().ok);
  extracted = repaired_archive->extract_stream_raw(stream, future_type);
  EXPECT_TRUE(extracted);
  if (extracted) EXPECT_EQ(*extracted, payload);
  std::filesystem::remove(repaired_path);
  std::filesystem::remove(source_path);
}

TEST(video_state_reader_accepts_hls_resource_frontier) {
  const auto fixture = make_hls_reader_archive("hls-frontier.coda");
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_TRUE(frames);
  if (frames) {
    EXPECT_EQ(frames->size(), std::size_t{1});
    EXPECT_EQ(frames->front().state, exact_frame());
    EXPECT_EQ(frames->front().source_records.size(), std::size_t{2});
    EXPECT_EQ(frames->front().source_records[0].hash,
              fixture.primary_source.hash);
    EXPECT_EQ(frames->front().source_records[1].hash,
              fixture.child_source.hash);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_rejects_hls_missing_primary_input) {
  const auto fixture = make_hls_reader_archive(
      "hls-missing-primary.coda",
      HlsReaderFixtureOptions{.include_primary_input = false});
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_rejects_hls_child_without_matching_descriptor) {
  const auto fixture = make_hls_reader_archive(
      "hls-missing-child-descriptor.coda",
      HlsReaderFixtureOptions{.include_child_descriptor = false});
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(fixture.path);
}

TEST(video_state_reader_rejects_hls_child_wrong_source_id_or_stream_type) {
  const auto wrong_source = make_hls_reader_archive(
      "hls-wrong-child-source.coda",
      HlsReaderFixtureOptions{.child_source_id = "fixture"});
  auto archive = codec::CodaArchive::open(wrong_source.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(wrong_source.path);

  const auto wrong_type = make_hls_reader_archive(
      "hls-wrong-child-type.coda",
      HlsReaderFixtureOptions{
          .child_stream_type = codec::StreamType::video});
  archive = codec::CodaArchive::open(wrong_type.path);
  EXPECT_TRUE(archive);
  frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(wrong_type.path);
}

TEST(video_state_reader_rejects_hls_duplicate_or_non_source_child) {
  const auto duplicate_stream = make_hls_reader_archive(
      "hls-duplicate-child-stream.coda",
      HlsReaderFixtureOptions{.duplicate_child_stream = true});
  auto archive = codec::CodaArchive::open(duplicate_stream.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(duplicate_stream.path);

  const auto non_source = make_hls_reader_archive(
      "hls-non-source-child.coda",
      HlsReaderFixtureOptions{.non_source_child = true});
  archive = codec::CodaArchive::open(non_source.path);
  EXPECT_TRUE(archive);
  frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(non_source.path);
}

TEST(video_state_reader_keeps_direct_provenance_rules_unchanged) {
  const auto fixture = make_hls_reader_archive(
      "direct-cross-stream.coda",
      HlsReaderFixtureOptions{.direct_process = true});
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto frames = video::query_verified_raw_video_frames(*archive);
  EXPECT_FALSE(frames);
  if (!frames) {
    EXPECT_EQ(frames.error().code, codec::ErrorCode::archive_corrupt);
  }
  std::filesystem::remove(fixture.path);
}
