#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d6-" + std::string{name});
}

codec::StreamId stream_id(std::string_view seed) {
  return codec::derive_stream_id(seed);
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.good());
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  EXPECT_TRUE(size >= 0);
  input.seekg(0, std::ios::beg);
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    EXPECT_TRUE(input.good());
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path,
                 std::span<const std::byte> bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  EXPECT_TRUE(output.good());
  if (!bytes.empty()) {
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  EXPECT_TRUE(output.good());
}

void write_native_flac(const std::filesystem::path& path,
                       unsigned bits_per_sample,
                       std::uint32_t sample_rate,
                       std::uint32_t channels,
                       const std::vector<FLAC__int32>& samples) {
  std::filesystem::remove(path);
  EXPECT_TRUE(channels != 0);
  EXPECT_EQ(samples.size() % channels, std::size_t{0});

  using EncoderPtr =
      std::unique_ptr<FLAC__StreamEncoder, decltype(&FLAC__stream_encoder_delete)>;
  EncoderPtr encoder{FLAC__stream_encoder_new(), &FLAC__stream_encoder_delete};
  EXPECT_TRUE(static_cast<bool>(encoder));
  EXPECT_TRUE(FLAC__stream_encoder_set_channels(encoder.get(), channels));
  EXPECT_TRUE(FLAC__stream_encoder_set_bits_per_sample(encoder.get(),
                                                        bits_per_sample));
  EXPECT_TRUE(FLAC__stream_encoder_set_sample_rate(encoder.get(), sample_rate));
  EXPECT_TRUE(FLAC__stream_encoder_set_compression_level(encoder.get(), 5));
  EXPECT_TRUE(FLAC__stream_encoder_set_total_samples_estimate(
      encoder.get(), samples.size() / channels));
  const auto init = FLAC__stream_encoder_init_file(
      encoder.get(), path.string().c_str(), nullptr, nullptr);
  EXPECT_EQ(init, FLAC__STREAM_ENCODER_INIT_STATUS_OK);
  EXPECT_TRUE(FLAC__stream_encoder_process_interleaved(
      encoder.get(), samples.data(),
      static_cast<unsigned>(samples.size() / channels)));
  EXPECT_TRUE(FLAC__stream_encoder_finish(encoder.get()));
}

codec::StreamDescriptor flac_descriptor(const codec::StreamId& stream,
                                        std::string payload_type = "audio/flac") {
  return codec::StreamDescriptor{
      .id = stream,
      .type = codec::StreamType::audio,
      .label = "D.6 native FLAC ingest",
      .source_id = "fixture",
      .payload_type = std::move(payload_type),
  };
}

codec::profiles::audio::Pcm16FlacIngestRequest request_for(
    const std::filesystem::path& source,
    const std::filesystem::path& archive,
    const codec::StreamDescriptor& descriptor) {
  return codec::profiles::audio::Pcm16FlacIngestRequest{
      .source_uri = source.string(),
      .archive_path = archive,
      .descriptor = descriptor,
      .start_ns = 100,
      .end_ns = 200,
      .capture_chunk_bytes = 256U * 1024U,
      .maximum_source_bytes = 1024ULL * 1024ULL,
      .maximum_decoded_pcm_bytes = 1024ULL * 1024ULL,
      .maximum_redirects = 5,
      .deny_private_network = true,
  };
}

struct DecodedFile {
  std::uint32_t sample_rate{};
  std::uint32_t channels{};
  std::uint32_t bits_per_sample{};
  std::vector<std::int16_t> samples;
  bool ok{true};
};

FLAC__StreamDecoderWriteStatus decode_write(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client_data) {
  auto& decoded = *static_cast<DecodedFile*>(client_data);
  if (frame == nullptr || buffer == nullptr) {
    decoded.ok = false;
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  for (std::uint32_t sample = 0; sample < frame->header.blocksize; ++sample) {
    for (std::uint32_t channel = 0; channel < frame->header.channels;
         ++channel) {
      const auto value = buffer[channel][sample];
      if (value < std::numeric_limits<std::int16_t>::min() ||
          value > std::numeric_limits<std::int16_t>::max()) {
        decoded.ok = false;
        return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
      }
      decoded.samples.push_back(static_cast<std::int16_t>(value));
    }
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void decode_metadata(const FLAC__StreamDecoder*,
                     const FLAC__StreamMetadata* metadata,
                     void* client_data) {
  if (metadata == nullptr || metadata->type != FLAC__METADATA_TYPE_STREAMINFO) {
    return;
  }
  auto& decoded = *static_cast<DecodedFile*>(client_data);
  decoded.sample_rate = metadata->data.stream_info.sample_rate;
  decoded.channels = metadata->data.stream_info.channels;
  decoded.bits_per_sample = metadata->data.stream_info.bits_per_sample;
}

void decode_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus,
                  void* client_data) {
  static_cast<DecodedFile*>(client_data)->ok = false;
}

DecodedFile decode_file(const std::filesystem::path& path) {
  using DecoderPtr =
      std::unique_ptr<FLAC__StreamDecoder, decltype(&FLAC__stream_decoder_delete)>;
  DecoderPtr decoder{FLAC__stream_decoder_new(), &FLAC__stream_decoder_delete};
  EXPECT_TRUE(static_cast<bool>(decoder));
  DecodedFile decoded;
  const auto init = FLAC__stream_decoder_init_file(
      decoder.get(), path.string().c_str(), decode_write, decode_metadata,
      decode_error, &decoded);
  EXPECT_EQ(init, FLAC__STREAM_DECODER_INIT_STATUS_OK);
  EXPECT_TRUE(FLAC__stream_decoder_process_until_end_of_stream(decoder.get()));
  EXPECT_TRUE(FLAC__stream_decoder_finish(decoder.get()));
  return decoded;
}

}  // namespace

TEST(audio_flac_ingest_preserves_exact_s0_and_emits_verified_aps1) {
  const auto source = test_path("happy.flac");
  const auto archive_path = test_path("happy.coda");
  std::filesystem::remove(archive_path);
  const std::vector<FLAC__int32> samples = {-32768, 32767, -1234, 1234, 0, 42};
  write_native_flac(source, 16, 48000, 2, samples);
  const auto source_bytes = read_bytes(source);
  const auto stream = stream_id("d6-happy");

  auto report = codec::profiles::audio::ingest_pcm16_flac(
      request_for(source, archive_path, flac_descriptor(stream)));
  EXPECT_TRUE(report);
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, source_bytes);

  codec::profiles::audio::Pcm16StateQuery query{
      .stream = stream,
      .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
      .maximum_results = 4,
      .maximum_encoded_bytes = 1024 * 1024,
  };
  auto verified = codec::profiles::audio::query_verified_pcm16_states(*archive,
                                                                       query);
  EXPECT_TRUE(verified);
  EXPECT_EQ(verified->size(), std::size_t{1});
  EXPECT_EQ(verified->front().state.sample_rate, std::uint32_t{48000});
  EXPECT_EQ(verified->front().state.channels, std::uint16_t{2});
  EXPECT_EQ(verified->front().state.samples,
            std::vector<std::int16_t>({-32768, 32767, -1234, 1234, 0, 42}));
  EXPECT_EQ(verified->front().source_record.hash, report->source.hash);
  EXPECT_EQ(verified->front().state_record.hash, report->state->hash);
  EXPECT_EQ(verified->front().provenance.subject_truth,
            codec::TruthClass::state_exact);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_round_trips_through_verified_flac_export) {
  const auto source = test_path("roundtrip.flac");
  const auto archive_path = test_path("roundtrip.coda");
  const auto exported_path = test_path("roundtrip-export.flac");
  std::filesystem::remove(archive_path);
  std::filesystem::remove(exported_path);
  const std::vector<FLAC__int32> samples = {-7, 7, -1024, 1024, 300, -300};
  write_native_flac(source, 16, 44100, 2, samples);
  const auto stream = stream_id("d6-roundtrip");
  auto report = codec::profiles::audio::ingest_pcm16_flac(
      request_for(source, archive_path, flac_descriptor(stream)));
  EXPECT_TRUE(report);
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto exported = codec::profiles::audio::export_verified_pcm16_flac(*archive);
  EXPECT_TRUE(exported);
  EXPECT_EQ(exported->size(), std::size_t{1});
  write_bytes(exported_path, exported->front().output.payload);
  const auto decoded = decode_file(exported_path);
  EXPECT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.sample_rate, std::uint32_t{44100});
  EXPECT_EQ(decoded.channels, std::uint32_t{2});
  EXPECT_EQ(decoded.bits_per_sample, std::uint32_t{16});
  EXPECT_EQ(decoded.samples,
            std::vector<std::int16_t>({-7, 7, -1024, 1024, 300, -300}));

  std::filesystem::remove(exported_path);
  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_preserves_malformed_native_flac_as_s0_only) {
  const auto source = test_path("malformed.flac");
  const auto archive_path = test_path("malformed.coda");
  std::filesystem::remove(archive_path);
  const std::vector<std::byte> bytes = {
      std::byte{'f'}, std::byte{'L'}, std::byte{'a'}, std::byte{'C'},
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
  write_bytes(source, bytes);
  const auto stream = stream_id("d6-malformed");
  auto report = codec::profiles::audio::ingest_pcm16_flac(
      request_for(source, archive_path, flac_descriptor(stream)));
  EXPECT_TRUE(report);
  EXPECT_FALSE(report->state_exact());
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_EQ(report->profile_error->code, codec::ErrorCode::decode);
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, bytes);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_preserves_unsupported_24_bit_flac_as_s0_only) {
  const auto source = test_path("24bit.flac");
  const auto archive_path = test_path("24bit.coda");
  std::filesystem::remove(archive_path);
  const std::vector<FLAC__int32> samples = {100000, -100000, 0, 1};
  write_native_flac(source, 24, 48000, 1, samples);
  const auto source_bytes = read_bytes(source);
  const auto stream = stream_id("d6-24bit");
  auto report = codec::profiles::audio::ingest_pcm16_flac(
      request_for(source, archive_path, flac_descriptor(stream)));
  EXPECT_TRUE(report);
  EXPECT_FALSE(report->state_exact());
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_EQ(report->profile_error->code, codec::ErrorCode::decode);
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  EXPECT_TRUE(archive->verify().ok);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, source_bytes);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_rejects_ogg_container_after_preserving_s0) {
  const auto source = test_path("ogg.flac");
  const auto archive_path = test_path("ogg.coda");
  std::filesystem::remove(archive_path);
  const std::vector<std::byte> bytes = {
      std::byte{'O'}, std::byte{'g'}, std::byte{'g'}, std::byte{'S'},
      std::byte{0}, std::byte{2}, std::byte{0}, std::byte{0}};
  write_bytes(source, bytes);
  const auto stream = stream_id("d6-ogg");
  auto report = codec::profiles::audio::ingest_pcm16_flac(
      request_for(source, archive_path, flac_descriptor(stream)));
  EXPECT_TRUE(report);
  EXPECT_FALSE(report->state_exact());
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_EQ(report->profile_error->code, codec::ErrorCode::decode);
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, bytes);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_decoded_limit_fails_profile_but_preserves_s0) {
  const auto source = test_path("bounded.flac");
  const auto archive_path = test_path("bounded.coda");
  std::filesystem::remove(archive_path);
  const std::vector<FLAC__int32> samples = {1, 2, 3, 4, 5, 6};
  write_native_flac(source, 16, 48000, 1, samples);
  const auto source_bytes = read_bytes(source);
  const auto stream = stream_id("d6-bound");
  auto request = request_for(source, archive_path, flac_descriptor(stream));
  request.maximum_decoded_pcm_bytes = 4;
  auto report = codec::profiles::audio::ingest_pcm16_flac(request);
  EXPECT_TRUE(report);
  EXPECT_FALSE(report->state_exact());
  EXPECT_TRUE(report->profile_error.has_value());
  EXPECT_EQ(report->profile_error->code, codec::ErrorCode::resource_exhausted);
  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  auto extracted = archive->extract_stream(stream);
  EXPECT_TRUE(extracted);
  EXPECT_EQ(*extracted, source_bytes);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}

TEST(audio_flac_ingest_validates_before_capture_or_replacement) {
  const auto source = test_path("validation.flac");
  const auto archive_path = test_path("validation.coda");
  std::filesystem::remove(archive_path);
  write_native_flac(source, 16, 48000, 1, {1, 2});
  const auto stream = stream_id("d6-validation");

  auto zero_decoded = request_for(source, archive_path, flac_descriptor(stream));
  zero_decoded.maximum_decoded_pcm_bytes = 0;
  auto zero_result = codec::profiles::audio::ingest_pcm16_flac(zero_decoded);
  EXPECT_FALSE(zero_result);
  EXPECT_EQ(zero_result.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(archive_path));

  auto wrong_type = request_for(source, archive_path,
                                flac_descriptor(stream, "audio/wav"));
  auto wrong_result = codec::profiles::audio::ingest_pcm16_flac(wrong_type);
  EXPECT_FALSE(wrong_result);
  EXPECT_EQ(wrong_result.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(archive_path));

  const std::vector<std::byte> sentinel = {std::byte{'k'}, std::byte{'e'},
                                          std::byte{'e'}, std::byte{'p'}};
  write_bytes(archive_path, sentinel);
  auto existing = request_for(source, archive_path, flac_descriptor(stream));
  auto existing_result = codec::profiles::audio::ingest_pcm16_flac(existing);
  EXPECT_FALSE(existing_result);
  EXPECT_EQ(existing_result.error().code, codec::ErrorCode::archive_io);
  EXPECT_EQ(read_bytes(archive_path), sentinel);

  std::filesystem::remove(archive_path);
  std::filesystem::remove(source);
}
