#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d4-" + std::string{name});
}

codec::StreamId stream_id(std::string_view seed) {
  return codec::derive_stream_id(seed);
}

codec::Pcm16State sample_state() {
  return codec::Pcm16State{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-32768, 32767, -1234, 1234, 0, 42},
  };
}

codec::WavPcm16 sample_wav() {
  const auto state = sample_state();
  return codec::WavPcm16{
      .sample_rate = state.sample_rate,
      .channels = state.channels,
      .samples = state.samples,
  };
}

std::vector<std::byte> raw_bytes(std::string_view value) {
  std::vector<std::byte> bytes;
  bytes.reserve(value.size());
  for (const unsigned char ch : value) {
    bytes.push_back(static_cast<std::byte>(ch));
  }
  return bytes;
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(static_cast<bool>(input));
  const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  bytes.reserve(chars.size());
  for (const unsigned char ch : chars) {
    bytes.push_back(static_cast<std::byte>(ch));
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(static_cast<bool>(output));
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  EXPECT_TRUE(static_cast<bool>(output));
}

codec::ProvenanceProcess canonicalize_process(std::int64_t created_ns = 200) {
  return codec::ProvenanceProcess{
      .operation = "audio.pcm16.canonicalize",
      .implementation_id = "codec-audio-profile",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = created_ns,
      .details_type = {},
      .details = {},
  };
}

struct VerifiedArchiveFixture {
  std::filesystem::path path;
  codec::RecordInfo source;
  std::vector<codec::RecordInfo> states;
};

VerifiedArchiveFixture make_verified_archive(std::string_view name,
                                             const codec::StreamId& stream,
                                             std::size_t state_count = 1) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto source = writer.append(codec::RecordType::source_bytes, stream,
                                    100, 200, raw_bytes("exact wav source"));
  EXPECT_TRUE(source);
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);

  std::vector<codec::RecordInfo> states;
  states.reserve(state_count);
  for (std::size_t index = 0; index < state_count; ++index) {
    const auto state = writer.append(codec::RecordType::pcm16, stream, 100,
                                     200, *encoded);
    EXPECT_TRUE(state);
    const std::array inputs{*source};
    EXPECT_TRUE(writer.append_stream_provenance(
        *state, codec::TruthClass::state_exact, inputs,
        canonicalize_process(200 + static_cast<std::int64_t>(index))));
    states.push_back(*state);
  }
  EXPECT_TRUE(writer.finalize());
  return VerifiedArchiveFixture{path, *source, std::move(states)};
}

}  // namespace

TEST(audio_export_consumes_actual_d2_d3_state_and_returns_wav_evidence) {
  const auto source_path = test_path("d2-source.wav");
  const auto archive_path = test_path("d2-output.coda");
  const auto decoded_path = test_path("d4-decoded.wav");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);
  std::filesystem::remove(decoded_path);

  const auto stream = stream_id("d4-d2-d3-integration");
  EXPECT_TRUE(sample_wav().write(source_path));
  const codec::profiles::audio::Pcm16WavIngestRequest request{
      .source_uri = source_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::audio,
          .label = "D.4 verified export integration",
          .source_id = "fixture",
          .payload_type = "audio/wav",
      },
      .start_ns = 100,
      .end_ns = 200,
  };
  auto report = codec::profiles::audio::ingest_pcm16_wav(request);
  EXPECT_TRUE(report);
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  const codec::profiles::audio::Pcm16StateQuery query{
      .stream = stream,
      .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
      .maximum_results = 4,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  const codec::profiles::audio::Pcm16WavExportLimits limits{
      .maximum_output_bytes = 1024 * 1024,
  };
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(
      *archive, query, limits);
  EXPECT_TRUE(exported);
  EXPECT_EQ(exported->size(), std::size_t{1});
  EXPECT_EQ(exported->front().output.payload_type, std::string{"audio/wav"});
  EXPECT_EQ(exported->front().output.supporting_records.size(),
            std::size_t{1});
  EXPECT_EQ(exported->front().state_record.sequence, report->state->sequence);
  EXPECT_EQ(exported->front().state_record.hash, report->state->hash);
  EXPECT_EQ(exported->front().source_record.sequence, report->source.sequence);
  EXPECT_EQ(exported->front().source_record.hash, report->source.hash);
  EXPECT_EQ(exported->front().provenance.subject_truth,
            codec::TruthClass::state_exact);
  EXPECT_EQ(exported->front().output.supporting_records.front().sequence,
            report->state->sequence);
  EXPECT_EQ(exported->front().output.supporting_records.front().hash,
            report->state->hash);

  write_bytes(decoded_path, exported->front().output.payload);
  auto decoded = codec::WavPcm16::read(decoded_path);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->sample_rate, sample_state().sample_rate);
  EXPECT_EQ(decoded->channels, sample_state().channels);
  EXPECT_EQ(decoded->samples, sample_state().samples);

  std::filesystem::remove(decoded_path);
  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(audio_export_wav_bytes_match_existing_wav_writer_exactly) {
  const auto fixture =
      make_verified_archive("byte-identity.coda", stream_id("d4-byte-id"));
  const auto expected_path = test_path("expected.wav");
  std::filesystem::remove(expected_path);
  EXPECT_TRUE(sample_wav().write(expected_path));
  const auto expected = read_bytes(expected_path);

  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_TRUE(exported);
  EXPECT_EQ(exported->size(), std::size_t{1});
  EXPECT_EQ(exported->front().output.payload, expected);

  std::filesystem::remove(expected_path);
  std::filesystem::remove(fixture.path);
}

TEST(audio_export_does_not_promote_unprovenanced_pcm16) {
  const auto path = test_path("unprovenanced.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d4-unprovenanced");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                            raw_bytes("source")));
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  EXPECT_TRUE(writer.append(codec::RecordType::pcm16, stream, 1, 2, *encoded));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_TRUE(exported);
  EXPECT_TRUE(exported->empty());
  std::filesystem::remove(path);
}

TEST(audio_export_rejects_non_source_lineage) {
  const auto path = test_path("non-source-lineage.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d4-non-source");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  const auto input = writer.append(codec::RecordType::pcm16, stream, 1, 2,
                                   *encoded);
  const auto state = writer.append(codec::RecordType::pcm16, stream, 1, 2,
                                   *encoded);
  EXPECT_TRUE(input);
  EXPECT_TRUE(state);
  const std::array inputs{*input};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, canonicalize_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_export_rejects_cross_stream_lineage) {
  const auto path = test_path("cross-stream-lineage.coda");
  std::filesystem::remove(path);
  const auto state_stream = stream_id("d4-state-stream");
  const auto source_stream = stream_id("d4-source-stream");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto source = writer.append(codec::RecordType::source_bytes,
                                    source_stream, 10, 20,
                                    raw_bytes("source"));
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  const auto state = writer.append(codec::RecordType::pcm16, state_stream,
                                   10, 20, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, canonicalize_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_export_rejects_mismatched_interval_lineage) {
  const auto path = test_path("mismatched-interval.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d4-interval");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto source = writer.append(codec::RecordType::source_bytes, stream,
                                    1, 2, raw_bytes("source"));
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  const auto state = writer.append(codec::RecordType::pcm16, stream, 3, 4,
                                   *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, canonicalize_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_export_propagates_malformed_aps1_decode_error) {
  const auto path = test_path("malformed-aps1.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d4-malformed");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const auto source = writer.append(codec::RecordType::source_bytes, stream,
                                    1, 2, raw_bytes("source"));
  const auto state = writer.append(codec::RecordType::pcm16, stream, 1, 2,
                                   raw_bytes("not APS1"));
  EXPECT_TRUE(source);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, canonicalize_process()));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::decode);
  std::filesystem::remove(path);
}

TEST(audio_export_enforces_output_limits_before_returning_partial_results) {
  const auto fixture = make_verified_archive(
      "output-limits.coda", stream_id("d4-output-limits"), 2);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  codec::profiles::audio::Pcm16WavExportLimits zero;
  zero.maximum_output_bytes = 0;
  auto invalid =
      codec::profiles::audio::export_verified_pcm16_wav(*archive, {}, zero);
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  const auto one_wav_bytes =
      std::uint64_t{44} + sample_state().samples.size() * sizeof(std::int16_t);
  codec::profiles::audio::Pcm16WavExportLimits too_small;
  too_small.maximum_output_bytes = one_wav_bytes - 1;
  auto exhausted_one = codec::profiles::audio::export_verified_pcm16_wav(
      *archive, {}, too_small);
  EXPECT_FALSE(exhausted_one);
  EXPECT_EQ(exhausted_one.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::profiles::audio::Pcm16WavExportLimits fits_one_not_two;
  fits_one_not_two.maximum_output_bytes = one_wav_bytes * 2 - 1;
  auto exhausted_all = codec::profiles::audio::export_verified_pcm16_wav(
      *archive, {}, fits_one_not_two);
  EXPECT_FALSE(exhausted_all);
  EXPECT_EQ(exhausted_all.error().code,
            codec::ErrorCode::resource_exhausted);

  std::filesystem::remove(fixture.path);
}

TEST(audio_export_preserves_verified_state_order) {
  const auto fixture = make_verified_archive(
      "ordered.coda", stream_id("d4-ordered"), 2);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  auto exported = codec::profiles::audio::export_verified_pcm16_wav(*archive);
  EXPECT_TRUE(exported);
  EXPECT_EQ(exported->size(), std::size_t{2});
  EXPECT_EQ(exported->at(0).state_record.sequence,
            fixture.states.at(0).sequence);
  EXPECT_EQ(exported->at(1).state_record.sequence,
            fixture.states.at(1).sequence);
  EXPECT_EQ(exported->at(0).output.supporting_records.front().sequence,
            fixture.states.at(0).sequence);
  EXPECT_EQ(exported->at(1).output.supporting_records.front().sequence,
            fixture.states.at(1).sequence);

  std::filesystem::remove(fixture.path);
}
