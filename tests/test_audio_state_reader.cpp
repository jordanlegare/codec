#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d3-" + std::string{name});
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

std::vector<std::byte> raw_bytes(std::string_view value) {
  std::vector<std::byte> bytes;
  bytes.reserve(value.size());
  for (const unsigned char ch : value) {
    bytes.push_back(static_cast<std::byte>(ch));
  }
  return bytes;
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

struct ArchiveFixture {
  std::filesystem::path path;
  codec::RecordInfo source;
  codec::RecordInfo state;
};

ArchiveFixture make_verified_archive(std::string_view name,
                                     const codec::StreamId& stream,
                                     std::int64_t source_start = 100,
                                     std::int64_t source_end = 200,
                                     std::int64_t state_start = 100,
                                     std::int64_t state_end = 200) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  auto writer = std::move(*codec::CodaWriter::create(path));
  const auto source = writer.append(codec::RecordType::source_bytes, stream,
                                    source_start, source_end,
                                    raw_bytes("exact wav source"));
  EXPECT_TRUE(source);
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  const auto state = writer.append(codec::RecordType::pcm16, stream,
                                   state_start, state_end, *encoded);
  EXPECT_TRUE(state);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state, codec::TruthClass::state_exact, inputs, canonicalize_process()));
  EXPECT_TRUE(writer.finalize());
  return ArchiveFixture{path, *source, *state};
}

}  // namespace

TEST(audio_state_reader_returns_only_verified_d1_state) {
  const auto stream = stream_id("d3-success");
  const auto fixture = make_verified_archive("success.coda", stream);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  const codec::profiles::audio::Pcm16StateQuery query{
      .stream = stream,
      .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
      .maximum_results = 8,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive,
                                                                     query);
  EXPECT_TRUE(states);
  EXPECT_EQ(states->size(), std::size_t{1});
  EXPECT_EQ(states->front().state.sample_rate, std::uint32_t{48000});
  EXPECT_EQ(states->front().state.channels, std::uint16_t{2});
  EXPECT_EQ(states->front().state.samples, sample_state().samples);
  EXPECT_EQ(states->front().state_record.sequence, fixture.state.sequence);
  EXPECT_EQ(states->front().state_record.hash, fixture.state.hash);
  EXPECT_EQ(states->front().source_record.sequence, fixture.source.sequence);
  EXPECT_EQ(states->front().source_record.hash, fixture.source.hash);
  EXPECT_EQ(states->front().provenance.subject_truth,
            codec::TruthClass::state_exact);
  EXPECT_EQ(states->front().provenance.inputs.size(), std::size_t{1});
  EXPECT_EQ(states->front().provenance.inputs.front().sequence,
            fixture.source.sequence);
  std::filesystem::remove(fixture.path);
}

TEST(audio_state_reader_consumes_actual_d2_ingest_output) {
  const auto wav_path = test_path("d2-source.wav");
  const auto archive_path = test_path("d2-output.coda");
  std::filesystem::remove(wav_path);
  std::filesystem::remove(archive_path);
  const auto stream = stream_id("d3-d2-integration");

  const codec::WavPcm16 wav{
      .sample_rate = sample_state().sample_rate,
      .channels = sample_state().channels,
      .samples = sample_state().samples,
  };
  EXPECT_TRUE(wav.write(wav_path));

  const codec::profiles::audio::Pcm16WavIngestRequest request{
      .source_uri = wav_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::audio,
          .label = "D.3 D.2 integration",
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
      .maximum_results = 1,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive,
                                                                     query);
  EXPECT_TRUE(states);
  EXPECT_EQ(states->size(), std::size_t{1});
  EXPECT_EQ(states->front().state.samples, sample_state().samples);
  EXPECT_EQ(states->front().source_record.sequence, report->source.sequence);
  EXPECT_EQ(states->front().source_record.hash, report->source.hash);
  EXPECT_EQ(states->front().state_record.sequence, report->state->sequence);
  EXPECT_EQ(states->front().state_record.hash, report->state->hash);
  EXPECT_EQ(states->front().provenance.process.operation,
            std::string{"audio.pcm16.canonicalize"});

  std::filesystem::remove(archive_path);
  std::filesystem::remove(wav_path);
}

TEST(audio_state_reader_does_not_promote_unprovenanced_pcm16) {
  const auto path = test_path("unprovenanced.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d3-unprovenanced");
  auto writer = std::move(*codec::CodaWriter::create(path));
  EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                            raw_bytes("source")));
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  EXPECT_TRUE(writer.append(codec::RecordType::pcm16, stream, 1, 2, *encoded));
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_TRUE(states);
  EXPECT_TRUE(states->empty());
  std::filesystem::remove(path);
}

TEST(audio_state_reader_rejects_non_source_lineage) {
  const auto path = test_path("non-source-lineage.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d3-non-source");
  auto writer = std::move(*codec::CodaWriter::create(path));
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
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_state_reader_rejects_cross_stream_lineage) {
  const auto path = test_path("cross-stream-lineage.coda");
  std::filesystem::remove(path);
  const auto state_stream = stream_id("d3-state-stream");
  const auto source_stream = stream_id("d3-source-stream");
  auto writer = std::move(*codec::CodaWriter::create(path));
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
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_state_reader_rejects_mismatched_interval_lineage) {
  const auto stream = stream_id("d3-interval");
  const auto fixture = make_verified_archive("interval.coda", stream, 1, 2, 3,
                                             4);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(fixture.path);
}

TEST(audio_state_reader_rejects_malformed_aps1_with_valid_lineage) {
  const auto path = test_path("malformed-aps1.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d3-malformed");
  auto writer = std::move(*codec::CodaWriter::create(path));
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
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::decode);
  std::filesystem::remove(path);
}

TEST(audio_state_reader_requires_finalized_archive) {
  const auto path = test_path("unfinalized.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d3-unfinalized");
  {
    auto writer = std::move(*codec::CodaWriter::create(path));
    EXPECT_TRUE(writer.append(codec::RecordType::source_bytes, stream, 1, 2,
                              raw_bytes("source")));
  }
  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_state_reader_enforces_query_limits) {
  const auto stream = stream_id("d3-limits");
  const auto fixture = make_verified_archive("limits.coda", stream);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  codec::profiles::audio::Pcm16StateQuery zero_results;
  zero_results.maximum_results = 0;
  auto invalid_results =
      codec::profiles::audio::query_verified_pcm16_states(*archive,
                                                           zero_results);
  EXPECT_FALSE(invalid_results);
  EXPECT_EQ(invalid_results.error().code, codec::ErrorCode::invalid_argument);

  codec::profiles::audio::Pcm16StateQuery zero_bytes;
  zero_bytes.maximum_encoded_bytes = 0;
  auto invalid_bytes =
      codec::profiles::audio::query_verified_pcm16_states(*archive, zero_bytes);
  EXPECT_FALSE(invalid_bytes);
  EXPECT_EQ(invalid_bytes.error().code, codec::ErrorCode::invalid_argument);

  codec::profiles::audio::Pcm16StateQuery tiny_bytes;
  tiny_bytes.maximum_encoded_bytes = 1;
  auto exhausted_bytes =
      codec::profiles::audio::query_verified_pcm16_states(*archive, tiny_bytes);
  EXPECT_FALSE(exhausted_bytes);
  EXPECT_EQ(exhausted_bytes.error().code,
            codec::ErrorCode::resource_exhausted);
  std::filesystem::remove(fixture.path);
}

TEST(audio_state_reader_enforces_result_count_before_decode) {
  const auto path = test_path("result-limit.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d3-result-limit");
  auto writer = std::move(*codec::CodaWriter::create(path));
  const auto source = writer.append(codec::RecordType::source_bytes, stream,
                                    1, 2, raw_bytes("source"));
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(source);
  EXPECT_TRUE(encoded);
  for (int index = 0; index < 2; ++index) {
    const auto state = writer.append(codec::RecordType::pcm16, stream, 1, 2,
                                     *encoded);
    EXPECT_TRUE(state);
    const std::array inputs{*source};
    EXPECT_TRUE(writer.append_stream_provenance(
        *state, codec::TruthClass::state_exact, inputs,
        canonicalize_process(200 + index)));
  }
  EXPECT_TRUE(writer.finalize());

  auto archive = codec::CodaArchive::open(path);
  EXPECT_TRUE(archive);
  codec::profiles::audio::Pcm16StateQuery query;
  query.maximum_results = 1;
  auto states = codec::profiles::audio::query_verified_pcm16_states(*archive,
                                                                     query);
  EXPECT_FALSE(states);
  EXPECT_EQ(states.error().code, codec::ErrorCode::resource_exhausted);
  std::filesystem::remove(path);
}
