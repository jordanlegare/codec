#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/profiles/audio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace audio_profile = codec::profiles::audio;

namespace {

std::vector<std::byte> aps1_fixture() {
  return {
      std::byte{0x41}, std::byte{0x50}, std::byte{0x53}, std::byte{0x31},
      std::byte{0x01}, std::byte{0x00}, std::byte{0x01}, std::byte{0x00},
      std::byte{0x80}, std::byte{0xbb}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x04}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
      std::byte{0x00}, std::byte{0x80}, std::byte{0xff}, std::byte{0xff},
      std::byte{0x01}, std::byte{0x00}, std::byte{0xff}, std::byte{0x7f},
  };
}

codec::WavPcm16 exact_wav() {
  return codec::WavPcm16{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-32768, -1, 1, 32767},
  };
}

}  // namespace

static_assert(std::is_same_v<audio_profile::WavPcm16, codec::WavPcm16>);
static_assert(std::is_same_v<audio_profile::Pcm16State, codec::Pcm16State>);
static_assert(std::is_same_v<audio_profile::CarrierBand, codec::CarrierBand>);
static_assert(std::is_same_v<audio_profile::WatermarkPolicy,
                             codec::WatermarkPolicy>);
static_assert(std::is_same_v<audio_profile::WatermarkEmbedReport,
                             codec::WatermarkEmbedReport>);
static_assert(std::is_same_v<audio_profile::WatermarkObservation,
                             codec::WatermarkObservation>);
static_assert(std::is_same_v<audio_profile::FeedStatement,
                             codec::FeedStatement>);
static_assert(std::is_same_v<audio_profile::StatementState,
                             codec::StatementState>);
static_assert(std::is_same_v<audio_profile::StatementVerification,
                             codec::StatementVerification>);
static_assert(std::is_same_v<audio_profile::SeparationRequest,
                             codec::SeparationRequest>);
static_assert(std::is_same_v<audio_profile::SeparationResult,
                             codec::SeparationResult>);
static_assert(std::is_same_v<audio_profile::SeparationBackend,
                             codec::SeparationBackend>);

TEST(audio_profile_facade_uses_existing_audio_implementation) {
  audio_profile::WavPcm16 audio;
  audio.sample_rate = 48000;
  audio.channels = 2;
  audio.samples = {1, 2, 3, 4};

  EXPECT_EQ(audio.frames(), 2U);
  EXPECT_EQ(std::string(audio_profile::carrier_band_name(
                audio_profile::CarrierBand::w1)),
            std::string("W1"));
  EXPECT_EQ(std::string(audio_profile::statement_state_name(
                audio_profile::StatementState::valid)),
            std::string("valid"));

  auto backend = audio_profile::default_separation_backend();
  EXPECT_TRUE(static_cast<bool>(backend));
  EXPECT_EQ(backend->name(), std::string("unavailable"));
  EXPECT_FALSE(backend->available());
}

TEST(audio_pcm16_canonicalization_preserves_exact_state) {
  auto state = audio_profile::canonicalize_pcm16(exact_wav());
  EXPECT_TRUE(state);
  if (state) {
    EXPECT_EQ(state->sample_rate, std::uint32_t{48000});
    EXPECT_EQ(state->channels, std::uint16_t{2});
    EXPECT_EQ(state->frames(), std::size_t{2});
    EXPECT_EQ(state->samples,
              (std::vector<std::int16_t>{-32768, -1, 1, 32767}));
  }
}

TEST(audio_pcm16_state_encoding_matches_the_aps1_fixture) {
  const codec::Pcm16State state{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-32768, -1, 1, 32767},
  };
  auto encoded = codec::encode_pcm16_state(state);
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_EQ(*encoded, aps1_fixture());
}

TEST(audio_pcm16_state_rejects_invalid_canonical_geometry) {
  auto zero_rate = exact_wav();
  zero_rate.sample_rate = 0;
  EXPECT_FALSE(codec::canonicalize_pcm16(zero_rate));

  auto zero_channels = exact_wav();
  zero_channels.channels = 0;
  EXPECT_FALSE(codec::canonicalize_pcm16(zero_channels));

  auto incomplete_frame = exact_wav();
  incomplete_frame.samples.pop_back();
  EXPECT_FALSE(codec::canonicalize_pcm16(incomplete_frame));

  EXPECT_FALSE(codec::encode_pcm16_state(codec::Pcm16State{
      .sample_rate = 0, .channels = 2, .samples = {1, 2}}));
  EXPECT_FALSE(codec::encode_pcm16_state(codec::Pcm16State{
      .sample_rate = 48000, .channels = 0, .samples = {1, 2}}));
  EXPECT_FALSE(codec::encode_pcm16_state(codec::Pcm16State{
      .sample_rate = 48000, .channels = 2, .samples = {1}}));
}

TEST(audio_pcm16_state_decodes_and_reencodes_the_exact_aps1_fixture) {
  const auto fixture = aps1_fixture();
  auto decoded = codec::decode_pcm16_state(fixture);
  EXPECT_TRUE(decoded);
  if (!decoded) return;

  EXPECT_EQ(decoded->sample_rate, std::uint32_t{48000});
  EXPECT_EQ(decoded->channels, std::uint16_t{2});
  EXPECT_EQ(decoded->samples,
            (std::vector<std::int16_t>{-32768, -1, 1, 32767}));
  auto reencoded = codec::encode_pcm16_state(*decoded);
  EXPECT_TRUE(reencoded);
  if (reencoded) EXPECT_EQ(*reencoded, fixture);
}

TEST(audio_pcm16_state_decoder_rejects_malformed_aps1_payloads) {
  std::vector<std::vector<std::byte>> malformed;

  auto short_header = aps1_fixture();
  short_header.resize(23);
  malformed.push_back(std::move(short_header));

  auto wrong_magic = aps1_fixture();
  wrong_magic[0] = std::byte{'X'};
  malformed.push_back(std::move(wrong_magic));

  auto wrong_version = aps1_fixture();
  wrong_version[4] = std::byte{2};
  malformed.push_back(std::move(wrong_version));

  auto wrong_format = aps1_fixture();
  wrong_format[6] = std::byte{2};
  malformed.push_back(std::move(wrong_format));

  auto nonzero_reserved = aps1_fixture();
  nonzero_reserved[14] = std::byte{1};
  malformed.push_back(std::move(nonzero_reserved));

  auto zero_rate = aps1_fixture();
  zero_rate[8] = zero_rate[9] = zero_rate[10] = zero_rate[11] = std::byte{0};
  malformed.push_back(std::move(zero_rate));

  auto zero_channels = aps1_fixture();
  zero_channels[12] = zero_channels[13] = std::byte{0};
  malformed.push_back(std::move(zero_channels));

  auto count_mismatch = aps1_fixture();
  count_mismatch[16] = std::byte{5};
  malformed.push_back(std::move(count_mismatch));

  auto incomplete_frame = aps1_fixture();
  incomplete_frame[12] = std::byte{3};
  malformed.push_back(std::move(incomplete_frame));

  auto trailing_byte = aps1_fixture();
  trailing_byte.push_back(std::byte{0});
  malformed.push_back(std::move(trailing_byte));

  for (const auto& payload : malformed) {
    auto decoded = codec::decode_pcm16_state(payload);
    EXPECT_FALSE(decoded);
    if (!decoded) EXPECT_EQ(decoded.error().code, codec::ErrorCode::decode);
  }
}

TEST(audio_pcm16_s1_storage_round_trips_state_and_exact_lineage) {
  const auto path = std::filesystem::temp_directory_path() /
                    "codec-test-audio-pcm16-s1.coda";
  std::filesystem::remove(path);
  codec::StreamId stream{};
  stream.bytes[0] = 0xd1;
  const std::vector<std::byte> source_payload{
      std::byte{'R'}, std::byte{'I'}, std::byte{'F'}, std::byte{'F'},
      std::byte{0x00}, std::byte{0x80}, std::byte{0xff}, std::byte{0x7f},
  };

  auto state = codec::canonicalize_pcm16(exact_wav());
  EXPECT_TRUE(state);
  if (!state) return;
  auto encoded = audio_profile::encode_pcm16_state(*state);
  EXPECT_TRUE(encoded);
  if (!encoded) return;

  auto writer = std::move(*codec::CodaWriter::create(path));
  auto source = writer.append(codec::RecordType::source_bytes, stream, 10, 20,
                              source_payload);
  EXPECT_TRUE(source);
  auto pcm = writer.append(codec::RecordType::pcm16, stream, 10, 20, *encoded);
  EXPECT_TRUE(pcm);
  if (!source || !pcm) return;
  const codec::ProvenanceProcess process{
      .operation = "audio.pcm16.canonicalize",
      .implementation_id = "codec-audio-profile",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1'788'000'000'000'000'000LL,
      .details_type = {},
      .details = {},
  };
  const std::array inputs{*source};
  auto sidecar = writer.append_stream_provenance(
      *pcm, codec::TruthClass::state_exact, inputs, process);
  EXPECT_TRUE(sidecar);
  EXPECT_TRUE(writer.finalize());

  auto archive = std::move(*codec::CodaArchive::open(path));
  auto extracted = archive.extract_records(codec::RecordQuery{
      .stream = stream,
      .type = codec::record_type_code(codec::RecordType::pcm16),
      .sequence = std::nullopt,
      .time = std::nullopt,
  });
  EXPECT_TRUE(extracted);
  if (!extracted) return;
  EXPECT_EQ(extracted->size(), std::size_t{1});
  auto decoded = audio_profile::decode_pcm16_state(extracted->front().payload);
  EXPECT_TRUE(decoded);
  if (decoded) {
    EXPECT_EQ(decoded->sample_rate, state->sample_rate);
    EXPECT_EQ(decoded->channels, state->channels);
    EXPECT_EQ(decoded->samples, state->samples);
  }

  auto provenance = archive.query_provenance(codec::ProvenanceQuery{
      .subject_truth = codec::TruthClass::state_exact,
      .subject = codec::RecordQuery{
          .stream = stream,
          .type = codec::record_type_code(codec::RecordType::pcm16),
          .sequence = std::nullopt,
          .time = std::nullopt,
      },
      .direct_input = codec::RecordQuery{
          .stream = stream,
          .type = codec::record_type_code(codec::RecordType::source_bytes),
          .sequence = std::nullopt,
          .time = std::nullopt,
      },
  });
  EXPECT_TRUE(provenance);
  if (!provenance) return;
  EXPECT_EQ(provenance->size(), std::size_t{1});
  const auto& proof = provenance->front();
  EXPECT_EQ(proof.subject_truth, codec::TruthClass::state_exact);
  EXPECT_EQ(proof.subject.stream, pcm->stream);
  EXPECT_EQ(proof.subject.type, pcm->type_code());
  EXPECT_EQ(proof.subject.sequence, pcm->sequence);
  EXPECT_EQ(proof.subject.hash, pcm->hash);
  EXPECT_EQ(proof.inputs.size(), std::size_t{1});
  EXPECT_EQ(proof.inputs.front().stream, source->stream);
  EXPECT_EQ(proof.inputs.front().type, source->type_code());
  EXPECT_EQ(proof.inputs.front().sequence, source->sequence);
  EXPECT_EQ(proof.inputs.front().hash, source->hash);
  EXPECT_EQ(proof.process.operation,
            std::string{"audio.pcm16.canonicalize"});
  EXPECT_EQ(proof.process.implementation_id,
            std::string{"codec-audio-profile"});
  EXPECT_EQ(proof.process.implementation_version, std::string{"1"});

  auto exact_source = archive.extract_stream(
      stream, codec::RecordType::source_bytes);
  EXPECT_TRUE(exact_source);
  if (exact_source) EXPECT_EQ(*exact_source, source_payload);
  std::filesystem::remove(path);
}
