#include "test.hpp"

#include <codec/profiles/video.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;

namespace {

std::filesystem::path encoded_reader_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-video-encoded-reader-" + std::string{name});
}

codec::ProvenanceProcess encoded_direct_process() {
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

codec::ProvenanceProcess encoded_hls_process() {
  auto process = encoded_direct_process();
  process.operation = "codec.video.encoded-audio.preserve.hls";
  process.details_type = "application/vnd.codec.video.hls-encoded-audio.v1";
  return process;
}

video::EncodedAudioState encoded_reader_state() {
  return video::EncodedAudioState{
      .codec = video::EncodedAudioCodec::aac,
      .codec_profile = 1,
      .sample_rate = 8'000,
      .channels = 1,
      .decoded_frames = 1'024,
      .trim_start_frames = 0,
      .presentation_frames = 1'024,
      .decoder_config = {std::byte{0x15}, std::byte{0x88}},
      .packets = {video::EncodedAudioPacket{
          .pts_offset_ns = 0,
          .dts_offset_ns = 0,
          .duration_ns = 128'000'000,
          .flags = 1,
          .payload = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}},
      }},
  };
}

void expect_encoded_reader_corrupt(
    const codec::Result<std::vector<video::VerifiedVideoEncodedAudio>>& result) {
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::archive_corrupt);
}

struct DirectReaderArchiveOptions {
  codec::ProvenanceProcess process{encoded_direct_process()};
  codec::TruthClass truth{codec::TruthClass::state_exact};
  codec::StreamType descriptor_type{codec::StreamType::video};
  bool write_provenance{true};
  std::size_t state_count{1};
  std::optional<std::vector<std::byte>> state_payload{};
};

struct ReaderArchiveFixture {
  std::filesystem::path path;
  codec::StreamId stream;
};

ReaderArchiveFixture make_direct_reader_archive(
    std::string_view name, DirectReaderArchiveOptions options = {}) {
  const auto path = encoded_reader_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(std::string{"encoded-audio-"} +
                                               std::string{name});
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = options.descriptor_type,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  EXPECT_TRUE(source);
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(encoded);
  const auto& payload =
      options.state_payload.has_value() ? *options.state_payload : *encoded;
  for (std::size_t index = 0U; index < options.state_count; ++index) {
    auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                   stream, 0, 128'000'000, payload);
    EXPECT_TRUE(state);
    if (options.write_provenance) {
      const std::array inputs{*source};
      EXPECT_TRUE(writer.append_stream_provenance(
          *state, options.truth, inputs, options.process));
    }
  }
  EXPECT_TRUE(writer.finalize());
  return ReaderArchiveFixture{.path = path, .stream = stream};
}

codec::Result<std::vector<video::VerifiedVideoEncodedAudio>> query_fixture(
    const ReaderArchiveFixture& fixture, std::size_t maximum_results = 8U,
    std::uint64_t maximum_encoded_bytes = 4096U) {
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  return video::query_verified_video_encoded_audio(
      *archive,
      video::VideoAudioQuery{.stream = fixture.stream,
                             .time = std::nullopt,
                             .maximum_results = maximum_results,
                             .maximum_encoded_bytes = maximum_encoded_bytes});
}

}  // namespace

TEST(video_encoded_audio_reader_accepts_exact_direct_lineage) {
  const auto path = encoded_reader_path("direct.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-reader-direct");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = "camera",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_audio(
      *archive,
      video::VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 8,
          .maximum_encoded_bytes = 4096,
      });
  EXPECT_TRUE(verified);
  if (verified) {
    EXPECT_EQ(verified->size(), std::size_t{1});
    if (!verified->empty()) {
      EXPECT_EQ(verified->front().state, encoded_reader_state());
      EXPECT_EQ(verified->front().state_record.hash, state->hash);
      EXPECT_EQ(verified->front().source_records.size(), std::size_t{1});
      EXPECT_EQ(verified->front().source_records.front().hash, source->hash);
      EXPECT_EQ(verified->front().provenance.process.operation,
                std::string{"codec.video.encoded-audio.preserve"});
    }
  }
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_accepts_ordered_hls_frontier) {
  const auto path = encoded_reader_path("hls.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-reader-hls");
  const auto child = codec::derive_stream_id("encoded-audio-reader-hls-child");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "application/vnd.apple.mpegurl"},
      0));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = child,
                              .type = codec::StreamType::opaque,
                              .label = "resource",
                              .source_id = "codec.video.hls-resource",
                              .payload_type = "video/mp2t"},
      0));
  auto primary = writer.append(codec::RecordType::source_bytes, stream, 0,
                               128'000'000,
                               std::array{std::byte{0xaa}});
  auto secondary = writer.append(codec::RecordType::source_bytes, child, 0,
                                 128'000'000,
                                 std::array{std::byte{0xbb}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(primary);
  EXPECT_TRUE(secondary);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*primary, *secondary};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_hls_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto verified = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096});
  EXPECT_TRUE(verified);
  if (verified && !verified->empty()) {
    EXPECT_EQ(verified->front().source_records.size(), std::size_t{2});
    EXPECT_EQ(verified->front().source_records[0].hash, primary->hash);
    EXPECT_EQ(verified->front().source_records[1].hash, secondary->hash);
    EXPECT_EQ(verified->front().provenance.process.operation,
              std::string{"codec.video.encoded-audio.preserve.hls"});
  }
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_wrong_process_identity) {
  const auto path = encoded_reader_path("wrong-process.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-wrong-process");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 128'000'000, *encoded);
  EXPECT_TRUE(state);
  auto process = encoded_direct_process();
  process.operation = "codec.video.pcm16.canonicalize";
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, process));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096}));
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_interval_mismatch) {
  const auto path = encoded_reader_path("interval.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("encoded-audio-interval");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      0));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                              128'000'000,
                              std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                 stream, 0, 127'000'000, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096}));
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_full_signed_range_interval) {
  const auto path = encoded_reader_path("full-signed-interval.coda");
  std::filesystem::remove(path);
  const auto stream =
      codec::derive_stream_id("encoded-audio-full-signed-interval");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append_stream_descriptor(
      codec::StreamDescriptor{.id = stream,
                              .type = codec::StreamType::video,
                              .label = "camera",
                              .source_id = "fixture",
                              .payload_type = "video/mp4"},
      std::numeric_limits<std::int64_t>::min()));
  auto source = writer.append(
      codec::RecordType::source_bytes, stream,
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max(),
      std::array{std::byte{0xaa}});
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  auto state = writer.append_raw(
      video::video_encoded_audio_state_record_type, stream,
      std::numeric_limits<std::int64_t>::min(),
      std::numeric_limits<std::int64_t>::max(), *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs,
      encoded_direct_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = stream,
                                      .time = std::nullopt,
                                      .maximum_results = 8,
                                      .maximum_encoded_bytes = 4096}));
  std::filesystem::remove(path);
}

TEST(video_encoded_audio_reader_rejects_wrong_process_details) {
  auto process = encoded_direct_process();
  process.details_type = "application/vnd.codec.video.encoded-audio.v2";
  const auto wrong_type = make_direct_reader_archive(
      "wrong-details-type.coda",
      DirectReaderArchiveOptions{.process = std::move(process)});
  expect_encoded_reader_corrupt(query_fixture(wrong_type));
  std::filesystem::remove(wrong_type.path);

  process = encoded_direct_process();
  process.details = {std::byte{0x02}};
  const auto wrong_value = make_direct_reader_archive(
      "wrong-details-value.coda",
      DirectReaderArchiveOptions{.process = std::move(process)});
  expect_encoded_reader_corrupt(query_fixture(wrong_value));
  std::filesystem::remove(wrong_value.path);
}

TEST(video_encoded_audio_reader_rejects_missing_or_non_exact_provenance) {
  const auto missing = make_direct_reader_archive(
      "missing-provenance.coda",
      DirectReaderArchiveOptions{.write_provenance = false});
  expect_encoded_reader_corrupt(query_fixture(missing));
  std::filesystem::remove(missing.path);

  const auto derived = make_direct_reader_archive(
      "derived-provenance.coda",
      DirectReaderArchiveOptions{.truth = codec::TruthClass::derived});
  expect_encoded_reader_corrupt(query_fixture(derived));
  std::filesystem::remove(derived.path);
}

TEST(video_encoded_audio_reader_rejects_non_video_descriptor) {
  const auto fixture = make_direct_reader_archive(
      "opaque-descriptor.coda",
      DirectReaderArchiveOptions{
          .descriptor_type = codec::StreamType::opaque});
  expect_encoded_reader_corrupt(query_fixture(fixture));
  std::filesystem::remove(fixture.path);
}

TEST(video_encoded_audio_reader_rejects_invalid_or_repeated_hls_child) {
  for (const bool repeat_child : {false, true}) {
    const auto name = repeat_child ? "repeated-hls-child.coda"
                                   : "invalid-hls-child.coda";
    const auto path = encoded_reader_path(name);
    std::filesystem::remove(path);
    const auto stream = codec::derive_stream_id(std::string{name} + "-parent");
    const auto child = codec::derive_stream_id(std::string{name} + "-child");
    auto writer = std::move(*codec::CodaWriter::create(path));
    EXPECT_TRUE(writer.append_stream_descriptor(
        codec::StreamDescriptor{.id = stream,
                                .type = codec::StreamType::video,
                                .label = "camera",
                                .source_id = "fixture",
                                .payload_type =
                                    "application/vnd.apple.mpegurl"},
        0));
    EXPECT_TRUE(writer.append_stream_descriptor(
        codec::StreamDescriptor{
            .id = child,
            .type = codec::StreamType::opaque,
            .label = "resource",
            .source_id = repeat_child ? "codec.video.hls-resource" : "wrong",
            .payload_type = "video/mp2t"},
        0));
    auto primary = writer.append(codec::RecordType::source_bytes, stream, 0,
                                 128'000'000,
                                 std::array{std::byte{0xaa}});
    auto secondary = writer.append(codec::RecordType::source_bytes, child, 0,
                                   128'000'000,
                                   std::array{std::byte{0xbb}});
    auto repeated = writer.append(codec::RecordType::source_bytes, child, 0,
                                  128'000'000,
                                  std::array{std::byte{0xcc}});
    auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
    EXPECT_TRUE(primary);
    EXPECT_TRUE(secondary);
    EXPECT_TRUE(repeated);
    EXPECT_TRUE(encoded);
    auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                   stream, 0, 128'000'000, *encoded);
    EXPECT_TRUE(state);
    const std::vector inputs =
        repeat_child ? std::vector{*primary, *secondary, *repeated}
                     : std::vector{*primary, *secondary};
    EXPECT_TRUE(writer.append_stream_provenance(
        *state, codec::TruthClass::state_exact, inputs,
        encoded_hls_process()));
    EXPECT_TRUE(writer.finalize());

    auto archive = codec::CodaArchive::open(path);
    EXPECT_TRUE(archive);
    expect_encoded_reader_corrupt(video::query_verified_video_encoded_audio(
        *archive, video::VideoAudioQuery{.stream = stream,
                                        .time = std::nullopt,
                                        .maximum_results = 8,
                                        .maximum_encoded_bytes = 4096}));
    std::filesystem::remove(path);
  }
}

TEST(video_encoded_audio_reader_rejects_duplicate_state_or_malformed_eap1) {
  const auto duplicate = make_direct_reader_archive(
      "duplicate-state.coda",
      DirectReaderArchiveOptions{.state_count = 2U});
  expect_encoded_reader_corrupt(query_fixture(duplicate));
  std::filesystem::remove(duplicate.path);

  const auto malformed = make_direct_reader_archive(
      "malformed-eap1.coda",
      DirectReaderArchiveOptions{
          .state_payload = std::vector{std::byte{'n'}, std::byte{'o'},
                                      std::byte{'t'}, std::byte{'!'}}});
  expect_encoded_reader_corrupt(query_fixture(malformed));
  std::filesystem::remove(malformed.path);
}

TEST(video_encoded_audio_reader_enforces_query_bounds) {
  const auto fixture = make_direct_reader_archive("query-bounds.coda");
  auto zero_count = query_fixture(fixture, 0U, 4096U);
  EXPECT_FALSE(zero_count);
  if (!zero_count) {
    EXPECT_EQ(zero_count.error().code, codec::ErrorCode::invalid_argument);
  }
  auto zero_bytes = query_fixture(fixture, 8U, 0U);
  EXPECT_FALSE(zero_bytes);
  if (!zero_bytes) {
    EXPECT_EQ(zero_bytes.error().code, codec::ErrorCode::invalid_argument);
  }
  auto encoded_bytes = query_fixture(fixture, 8U, 1U);
  EXPECT_FALSE(encoded_bytes);
  if (!encoded_bytes) {
    EXPECT_EQ(encoded_bytes.error().code,
              codec::ErrorCode::resource_exhausted);
  }
  std::filesystem::remove(fixture.path);

  const auto path = encoded_reader_path("query-count.coda");
  std::filesystem::remove(path);
  auto writer = std::move(*codec::CodaWriter::create(path));
  auto encoded = video::encode_encoded_audio_state(encoded_reader_state());
  EXPECT_TRUE(encoded);
  for (const auto& name : {"query-count-one", "query-count-two"}) {
    const auto stream = codec::derive_stream_id(name);
    EXPECT_TRUE(writer.append_stream_descriptor(
        codec::StreamDescriptor{.id = stream,
                                .type = codec::StreamType::video,
                                .label = name,
                                .source_id = "fixture",
                                .payload_type = "video/mp4"},
        0));
    auto source = writer.append(codec::RecordType::source_bytes, stream, 0,
                                128'000'000,
                                std::array{std::byte{0xaa}});
    auto state = writer.append_raw(video::video_encoded_audio_state_record_type,
                                   stream, 0, 128'000'000, *encoded);
    EXPECT_TRUE(source);
    EXPECT_TRUE(state);
    const std::array inputs{*source};
    EXPECT_TRUE(writer.append_stream_provenance(
        *state, codec::TruthClass::state_exact, inputs,
        encoded_direct_process()));
  }
  EXPECT_TRUE(writer.finalize());
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto count = video::query_verified_video_encoded_audio(
      *archive, video::VideoAudioQuery{.stream = std::nullopt,
                                      .time = std::nullopt,
                                      .maximum_results = 1,
                                      .maximum_encoded_bytes = 4096});
  EXPECT_FALSE(count);
  if (!count) {
    EXPECT_EQ(count.error().code, codec::ErrorCode::resource_exhausted);
  }
  std::filesystem::remove(path);
}
