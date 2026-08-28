#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <FLAC/stream_decoder.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d5-" + std::string{name});
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

struct DecodedFlac {
  std::uint32_t sample_rate{};
  std::uint32_t channels{};
  std::uint32_t bits_per_sample{};
  std::vector<std::int16_t> samples;
};

struct DecoderState {
  std::span<const std::byte> input;
  std::size_t cursor{};
  DecodedFlac decoded;
  bool ok{true};
};

FLAC__StreamDecoderReadStatus decoder_read(
    const FLAC__StreamDecoder*, FLAC__byte buffer[], std::size_t* bytes,
    void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (bytes == nullptr || *bytes == 0) {
    state.ok = false;
    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  }
  if (state.cursor >= state.input.size()) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  const auto remaining = state.input.size() - state.cursor;
  const auto count = std::min(*bytes, remaining);
  std::memcpy(buffer, state.input.data() + state.cursor, count);
  state.cursor += count;
  *bytes = count;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus decoder_write(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (frame == nullptr || buffer == nullptr) {
    state.ok = false;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (state.decoded.channels != 0 &&
      frame->header.channels != state.decoded.channels) {
    state.ok = false;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (state.decoded.sample_rate != 0 &&
      frame->header.sample_rate != state.decoded.sample_rate) {
    state.ok = false;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  for (std::uint32_t sample = 0; sample < frame->header.blocksize; ++sample) {
    for (std::uint32_t channel = 0; channel < frame->header.channels;
         ++channel) {
      const auto value = buffer[channel][sample];
      if (value < std::numeric_limits<std::int16_t>::min() ||
          value > std::numeric_limits<std::int16_t>::max()) {
        state.ok = false;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
      state.decoded.samples.push_back(static_cast<std::int16_t>(value));
    }
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void decoder_metadata(const FLAC__StreamDecoder*,
                      const FLAC__StreamMetadata* metadata,
                      void* client_data) {
  auto& state = *static_cast<DecoderState*>(client_data);
  if (metadata == nullptr || metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
    return;
  }
  state.decoded.sample_rate = metadata->data.stream_info.sample_rate;
  state.decoded.channels = metadata->data.stream_info.channels;
  state.decoded.bits_per_sample = metadata->data.stream_info.bits_per_sample;
}

void decoder_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus,
                   void* client_data) {
  static_cast<DecoderState*>(client_data)->ok = false;
}

std::optional<DecodedFlac> decode_flac(std::span<const std::byte> input) {
  using DecoderPtr =
      std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>;
  DecoderPtr decoder{FLAC__stream_decoder_new(), &FLAC__stream_decoder_delete};
  if (!decoder) return std::nullopt;

  DecoderState state{.input = input};
  const auto init = FLAC__stream_decoder_init_stream(
      decoder.get(), decoder_read, nullptr, nullptr, nullptr, nullptr,
      decoder_write, decoder_metadata, decoder_error, &state);
  if (init != FLAC__STREAM_DECODER_INIT_STATUS_OK) return std::nullopt;
  const auto processed =
      FLAC__stream_decoder_process_until_end_of_stream(decoder.get());
  const auto finished = FLAC__stream_decoder_finish(decoder.get());
  if (!processed || !finished || !state.ok ||
      state.decoded.bits_per_sample != 16) {
    return std::nullopt;
  }
  return std::move(state.decoded);
}

}  // namespace

TEST(audio_flac_export_consumes_actual_d2_d3_state_and_round_trips_pcm) {
  const auto source_path = test_path("d2-source.wav");
  const auto archive_path = test_path("d2-output.coda");
  std::filesystem::remove(source_path);
  std::filesystem::remove(archive_path);

  const auto stream = stream_id("d5-d2-d3-integration");
  EXPECT_TRUE(sample_wav().write(source_path));
  const codec::profiles::audio::Pcm16WavIngestRequest request{
      .source_uri = source_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::audio,
          .label = "D.5 verified FLAC export integration",
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
  const codec::profiles::audio::Pcm16FlacExportLimits limits{
      .maximum_output_bytes = 1024 * 1024,
  };
  auto exported = codec::profiles::audio::export_verified_pcm16_flac(
      *archive, query, limits);
  EXPECT_TRUE(exported);
  EXPECT_EQ(exported->size(), std::size_t{1});
  EXPECT_EQ(exported->front().output.payload_type,
            std::string{"audio/flac"});
  EXPECT_TRUE(exported->front().output.payload.size() >= 4);
  EXPECT_EQ(exported->front().output.payload[0], std::byte{'f'});
  EXPECT_EQ(exported->front().output.payload[1], std::byte{'L'});
  EXPECT_EQ(exported->front().output.payload[2], std::byte{'a'});
  EXPECT_EQ(exported->front().output.payload[3], std::byte{'C'});
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

  auto decoded = decode_flac(exported->front().output.payload);
  EXPECT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded->sample_rate, sample_state().sample_rate);
  EXPECT_EQ(decoded->channels, sample_state().channels);
  EXPECT_EQ(decoded->bits_per_sample, std::uint32_t{16});
  EXPECT_EQ(decoded->samples, sample_state().samples);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source_path);
}

TEST(audio_flac_export_does_not_promote_unprovenanced_pcm16) {
  const auto path = test_path("unprovenanced.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d5-unprovenanced");
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
  auto exported =
      codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_TRUE(exported);
  EXPECT_TRUE(exported->empty());
  std::filesystem::remove(path);
}

TEST(audio_flac_export_rejects_non_source_lineage) {
  const auto path = test_path("non-source-lineage.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d5-non-source");
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
  auto exported =
      codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_flac_export_rejects_cross_stream_lineage) {
  const auto path = test_path("cross-stream-lineage.coda");
  std::filesystem::remove(path);
  const auto state_stream = stream_id("d5-state-stream");
  const auto source_stream = stream_id("d5-source-stream");
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
  auto exported =
      codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_flac_export_rejects_mismatched_interval_lineage) {
  const auto path = test_path("mismatched-interval.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d5-interval");
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
  auto exported =
      codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::archive_corrupt);
  std::filesystem::remove(path);
}

TEST(audio_flac_export_propagates_malformed_aps1_decode_error) {
  const auto path = test_path("malformed-aps1.coda");
  std::filesystem::remove(path);
  const auto stream = stream_id("d5-malformed");
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
  auto exported =
      codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_FALSE(exported);
  EXPECT_EQ(exported.error().code, codec::ErrorCode::decode);
  std::filesystem::remove(path);
}

TEST(audio_flac_export_enforces_aggregate_output_limits) {
  const auto one =
      make_verified_archive("one-output.coda", stream_id("d5-one-output"));
  auto one_archive = codec::CodaArchive::open(one.path);
  EXPECT_TRUE(one_archive);

  codec::profiles::audio::Pcm16FlacExportLimits zero;
  zero.maximum_output_bytes = 0;
  auto invalid = codec::profiles::audio::export_verified_pcm16_flac(
      *one_archive, {}, zero);
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  codec::profiles::audio::Pcm16FlacExportLimits generous;
  generous.maximum_output_bytes = 1024 * 1024;
  auto baseline = codec::profiles::audio::export_verified_pcm16_flac(
      *one_archive, {}, generous);
  EXPECT_TRUE(baseline);
  EXPECT_EQ(baseline->size(), std::size_t{1});
  const auto one_size = baseline->front().output.payload.size();
  EXPECT_TRUE(one_size > 1);

  codec::profiles::audio::Pcm16FlacExportLimits too_small;
  too_small.maximum_output_bytes = one_size - 1;
  auto exhausted_one = codec::profiles::audio::export_verified_pcm16_flac(
      *one_archive, {}, too_small);
  EXPECT_FALSE(exhausted_one);
  EXPECT_EQ(exhausted_one.error().code,
            codec::ErrorCode::resource_exhausted);

  const auto two = make_verified_archive("two-output.coda",
                                         stream_id("d5-two-output"), 2);
  auto two_archive = codec::CodaArchive::open(two.path);
  EXPECT_TRUE(two_archive);
  codec::profiles::audio::Pcm16FlacExportLimits fits_one_not_two;
  fits_one_not_two.maximum_output_bytes =
      static_cast<std::uint64_t>(one_size) * 2 - 1;
  auto exhausted_all = codec::profiles::audio::export_verified_pcm16_flac(
      *two_archive, {}, fits_one_not_two);
  EXPECT_FALSE(exhausted_all);
  EXPECT_EQ(exhausted_all.error().code,
            codec::ErrorCode::resource_exhausted);

  std::filesystem::remove(two.path);
  std::filesystem::remove(one.path);
}
