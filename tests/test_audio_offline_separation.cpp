#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d7-" + std::string{name});
}

codec::Pcm16State sample_state() {
  return codec::Pcm16State{
      .sample_rate = 48000,
      .channels = 2,
      .samples = {-1000, 1000, -333, 333, 0, 42},
  };
}

std::vector<std::byte> raw_bytes(std::string_view value) {
  const auto characters = std::span{value.data(), value.size()};
  const auto bytes = std::as_bytes(characters);
  return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(static_cast<bool>(input));
  const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                std::istreambuf_iterator<char>{}};
  std::vector<std::byte> output;
  output.reserve(chars.size());
  for (const unsigned char value : chars) {
    output.push_back(static_cast<std::byte>(value));
  }
  return output;
}

codec::ProvenanceProcess canonicalize_process(std::int64_t created_ns) {
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
  codec::StreamId stream;
  codec::RecordInfo source;
  std::vector<codec::RecordInfo> states;
};

VerifiedArchiveFixture make_verified_archive(std::string_view name,
                                             std::size_t state_count) {
  const auto path = test_path(name);
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id(std::string{name});
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  auto source = writer.append(codec::RecordType::source_bytes, stream, 100,
                              200, raw_bytes("exact wav bytes"));
  EXPECT_TRUE(source);
  auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  std::vector<codec::RecordInfo> states;
  states.reserve(state_count);
  for (std::size_t index = 0; index < state_count; ++index) {
    auto state = writer.append(codec::RecordType::pcm16, stream, 100, 200,
                               *encoded);
    EXPECT_TRUE(state);
    const std::array inputs{*source};
    EXPECT_TRUE(writer.append_stream_provenance(
        *state, codec::TruthClass::state_exact, inputs,
        canonicalize_process(200 + static_cast<std::int64_t>(index))));
    states.push_back(*state);
  }
  EXPECT_TRUE(writer.finalize());
  return VerifiedArchiveFixture{path, stream, *source, std::move(states)};
}

codec::SeparationResult exact_separation(
    const codec::SeparationRequest& request,
    double backend_reported_error = 0.25) {
  codec::WavPcm16 stem = request.mixture;
  codec::WavPcm16 residual = request.mixture;
  for (std::size_t index = 0; index < request.mixture.samples.size(); ++index) {
    stem.samples[index] = static_cast<std::int16_t>(
        request.mixture.samples[index] / 2);
    residual.samples[index] = static_cast<std::int16_t>(
        request.mixture.samples[index] - stem.samples[index]);
  }
  return codec::SeparationResult{
      .stems = {std::move(stem)},
      .residual = std::move(residual),
      .mixture_reconstruction_error = backend_reported_error,
      .model_hash =
          "000102030405060708090a0b0c0d0e0f"
          "101112131415161718191a1b1c1d1e1f",
      .provider = "test-cpu",
  };
}

class DeterministicBackend final : public codec::SeparationBackend {
 public:
  using Behavior = std::function<codec::Result<codec::SeparationResult>(
      const codec::SeparationRequest&, std::size_t)>;

  std::string name() const override { return backend_name; }
  bool available() const noexcept override { return is_available; }

  codec::Result<codec::SeparationResult> separate(
      const codec::SeparationRequest& request) override {
    const auto call_index = calls;
    ++calls;
    last_request = request;
    if (behavior) return behavior(request, call_index);
    return exact_separation(request);
  }

  std::string backend_name{"test-runtime"};
  bool is_available{true};
  Behavior behavior;
  std::size_t calls{};
  codec::SeparationRequest last_request;
};

codec::profiles::audio::OfflinePcm16SeparationRequest request_for(
    const codec::StreamId& stream, std::size_t maximum_results = 8) {
  return codec::profiles::audio::OfflinePcm16SeparationRequest{
      .states = codec::profiles::audio::Pcm16StateQuery{
          .stream = stream,
          .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
          .maximum_results = maximum_results,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      .maximum_sources = 2,
      .model_bundle = "test-model.bundle",
      .created_utc_ns = 300,
      .limits = codec::profiles::audio::OfflinePcm16SeparationLimits{
          .maximum_output_bytes = 1024 * 1024,
      },
  };
}

void expect_error(const codec::CodaArchive& archive,
                  DeterministicBackend& backend,
                  const codec::profiles::audio::OfflinePcm16SeparationRequest&
                      request,
                  codec::ErrorCode expected) {
  auto result = codec::profiles::audio::separate_verified_pcm16_offline(
      archive, backend, request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, expected);
}

}  // namespace

TEST(audio_offline_separation_consumes_d2_d3_state_and_returns_d_artifacts) {
  const auto wav_path = test_path("success.wav");
  const auto archive_path = test_path("success.coda");
  std::filesystem::remove(wav_path);
  std::filesystem::remove(archive_path);

  const auto state = sample_state();
  const codec::WavPcm16 wav{
      .sample_rate = state.sample_rate,
      .channels = state.channels,
      .samples = state.samples,
  };
  EXPECT_TRUE(wav.write(wav_path));

  const auto stream = codec::derive_stream_id("d7-success");
  const codec::profiles::audio::Pcm16WavIngestRequest ingest{
      .source_uri = wav_path.string(),
      .archive_path = archive_path,
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::audio,
          .label = "D.7 offline separation",
          .source_id = "fixture",
          .payload_type = "audio/wav",
      },
      .start_ns = 100,
      .end_ns = 200,
  };
  auto report = codec::profiles::audio::ingest_pcm16_wav(ingest);
  EXPECT_TRUE(report);
  EXPECT_TRUE(report->state_exact());

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  DeterministicBackend backend;
  const codec::profiles::audio::OfflinePcm16SeparationRequest request{
      .states = codec::profiles::audio::Pcm16StateQuery{
          .stream = stream,
          .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
          .maximum_results = 1,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      .maximum_sources = 2,
      .model_bundle = "test-model.bundle",
      .created_utc_ns = 300,
      .limits = codec::profiles::audio::OfflinePcm16SeparationLimits{
          .maximum_output_bytes = 1024 * 1024,
      },
  };
  auto separated = codec::profiles::audio::separate_verified_pcm16_offline(
      *archive, backend, request);

  EXPECT_TRUE(separated);
  EXPECT_EQ(separated->size(), std::size_t{1});
  EXPECT_EQ(backend.calls, std::size_t{1});
  EXPECT_EQ(backend.last_request.maximum_sources, std::size_t{2});
  EXPECT_EQ(backend.last_request.model_bundle,
            std::string{"test-model.bundle"});
  EXPECT_EQ(separated->front().stems.size(), std::size_t{1});
  EXPECT_EQ(separated->front().stems.front().role,
            codec::profiles::audio::OfflinePcm16ArtifactRole::stem);
  EXPECT_EQ(separated->front().residual.role,
            codec::profiles::audio::OfflinePcm16ArtifactRole::residual);
  EXPECT_EQ(separated->front().stems.front().output.truth,
            codec::TruthClass::derived);
  EXPECT_EQ(separated->front().residual.output.truth,
            codec::TruthClass::derived);
  EXPECT_EQ(separated->front().stems.front().supporting_state.sequence,
            report->state->sequence);
  EXPECT_EQ(separated->front().stems.front().supporting_state.hash,
            report->state->hash);
  EXPECT_EQ(separated->front().input.source_record.sequence,
            report->source.sequence);
  EXPECT_EQ(separated->front().input.source_record.hash, report->source.hash);
  EXPECT_EQ(separated->front().backend, std::string{"test-runtime"});
  EXPECT_EQ(separated->front().provider, std::string{"test-cpu"});
  EXPECT_EQ(separated->front().model_hash.front(), std::uint8_t{0});
  EXPECT_EQ(separated->front().model_hash.back(), std::uint8_t{31});
  EXPECT_EQ(separated->front().reconstruction.maximum_absolute_sample_error,
            std::uint64_t{0});
  EXPECT_EQ(separated->front().reconstruction.root_mean_square_sample_error,
            0.0);
  EXPECT_EQ(separated->front().reconstruction.backend_reported_error, 0.25);
  EXPECT_EQ(separated->front().stems.front().output.process.operation,
            std::string{"audio.offline-separation"});
  EXPECT_EQ(separated->front().stems.front().output.process.implementation_id,
            std::string{"test-runtime/test-cpu"});
  EXPECT_EQ(
      separated->front().stems.front().output.process.implementation_hash,
      std::optional<codec::Sha256>{separated->front().model_hash});
  EXPECT_EQ(
      separated->front().stems.front().output.process.configuration_hash,
      std::optional<codec::Sha256>{separated->front().configuration_hash});
  EXPECT_EQ(separated->front().stems.front().output.process.details_type,
            std::string{"codec.audio.offline-separation-output.v1"});
  EXPECT_EQ(separated->front().stems.front().output.process.details.size(),
            std::size_t{40});

  std::filesystem::remove(archive_path);
  std::filesystem::remove(wav_path);
}

TEST(audio_offline_separation_validates_requests_before_backend_invocation) {
  const auto fixture = make_verified_archive("validation.coda", 1);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);

  const auto expect_invalid = [&](auto mutate) {
    auto request = request_for(fixture.stream);
    mutate(request);
    DeterministicBackend backend;
    expect_error(*archive, backend, request, codec::ErrorCode::invalid_argument);
    EXPECT_EQ(backend.calls, std::size_t{0});
  };
  expect_invalid([](auto& request) { request.states.time = std::nullopt; });
  expect_invalid([](auto& request) {
    request.states.time =
        codec::RecordTimeRange{.begin_ns = 200, .end_ns = 100};
  });
  expect_invalid([](auto& request) { request.states.maximum_results = 0; });
  expect_invalid(
      [](auto& request) { request.states.maximum_encoded_bytes = 0; });
  expect_invalid([](auto& request) { request.maximum_sources = 0; });
  expect_invalid([](auto& request) { request.maximum_sources = 65; });
  expect_invalid(
      [](auto& request) { request.limits.maximum_output_bytes = 0; });
  expect_invalid([](auto& request) { request.model_bundle.clear(); });
  expect_invalid([](auto& request) {
    request.model_bundle = std::string{"bad\0bundle", 10};
  });
  expect_invalid(
      [](auto& request) { request.model_bundle.assign(4097, 'x'); });

  auto valid_request = request_for(fixture.stream);
  DeterministicBackend empty_name;
  empty_name.backend_name.clear();
  expect_error(*archive, empty_name, valid_request,
               codec::ErrorCode::invalid_argument);
  EXPECT_EQ(empty_name.calls, std::size_t{0});

  DeterministicBackend nul_name;
  nul_name.backend_name = std::string{"bad\0runtime", 11};
  expect_error(*archive, nul_name, valid_request,
               codec::ErrorCode::invalid_argument);
  EXPECT_EQ(nul_name.calls, std::size_t{0});

  DeterministicBackend long_name;
  long_name.backend_name.assign(2049, 'x');
  expect_error(*archive, long_name, valid_request,
               codec::ErrorCode::invalid_argument);
  EXPECT_EQ(long_name.calls, std::size_t{0});
  std::filesystem::remove(fixture.path);
}

TEST(audio_offline_separation_propagates_explicit_model_unavailability) {
  const auto fixture = make_verified_archive("unavailable.coda", 1);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto backend = codec::default_separation_backend();
  auto result = codec::profiles::audio::separate_verified_pcm16_offline(
      *archive, *backend, request_for(fixture.stream));
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::model_incompatible);
  }
  std::filesystem::remove(fixture.path);
}

TEST(audio_offline_separation_rejects_malformed_backend_success_values) {
  const auto fixture = make_verified_archive("malformed.coda", 1);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  const auto request = request_for(fixture.stream);

  using Mutator = std::function<void(codec::SeparationResult&)>;
  const std::vector<Mutator> mutations{
      [](auto& value) { value.stems.clear(); },
      [](auto& value) { value.stems.push_back(value.stems.front());
                       value.stems.push_back(value.stems.front()); },
      [](auto& value) { ++value.stems.front().sample_rate; },
      [](auto& value) { ++value.stems.front().channels; },
      [](auto& value) { value.stems.front().samples.pop_back(); },
      [](auto& value) { ++value.residual.sample_rate; },
      [](auto& value) { value.provider.clear(); },
      [](auto& value) { value.provider = std::string{"bad\0provider", 12}; },
      [](auto& value) { value.provider.assign(2049, 'x'); },
      [](auto& value) { value.model_hash = "not-a-hash"; },
      [](auto& value) { value.model_hash.assign(64, 'z'); },
      [](auto& value) { value.mixture_reconstruction_error = -1.0; },
      [](auto& value) {
        value.mixture_reconstruction_error =
            std::numeric_limits<double>::quiet_NaN();
      },
      [](auto& value) {
        value.mixture_reconstruction_error =
            std::numeric_limits<double>::infinity();
      },
  };
  for (const auto& mutate : mutations) {
    DeterministicBackend backend;
    backend.behavior = [mutate](const auto& backend_request, std::size_t) {
      auto value = exact_separation(backend_request);
      mutate(value);
      return codec::Result<codec::SeparationResult>{std::move(value)};
    };
    expect_error(*archive, backend, request, codec::ErrorCode::inference);
    EXPECT_EQ(backend.calls, std::size_t{1});
  }
  std::filesystem::remove(fixture.path);
}

TEST(audio_offline_separation_enforces_one_aggregate_output_budget) {
  const auto fixture = make_verified_archive("aggregate.coda", 2);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  auto request = request_for(fixture.stream);
  const auto encoded = codec::encode_pcm16_state(sample_state());
  EXPECT_TRUE(encoded);
  request.limits.maximum_output_bytes = encoded->size() * 2;
  DeterministicBackend backend;
  expect_error(*archive, backend, request,
               codec::ErrorCode::resource_exhausted);
  EXPECT_EQ(backend.calls, std::size_t{1});
  std::filesystem::remove(fixture.path);
}

TEST(audio_offline_separation_returns_no_partial_vector_on_later_failure) {
  const auto fixture = make_verified_archive("atomic.coda", 2);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  DeterministicBackend backend;
  backend.behavior = [](const auto& request, std::size_t call) {
    if (call == 1) {
      return codec::Result<codec::SeparationResult>{codec::Error{
          codec::ErrorCode::inference, "second state failed", false}};
    }
    return codec::Result<codec::SeparationResult>{exact_separation(request)};
  };
  auto result = codec::profiles::audio::separate_verified_pcm16_offline(
      *archive, backend, request_for(fixture.stream));
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::inference);
  EXPECT_EQ(backend.calls, std::size_t{2});
  std::filesystem::remove(fixture.path);
}

TEST(audio_offline_separation_reports_independent_metrics_without_mutation) {
  const auto fixture = make_verified_archive("metrics.coda", 1);
  const auto before = read_bytes(fixture.path);
  auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  DeterministicBackend backend;
  backend.behavior = [](const auto& request, std::size_t) {
    auto value = exact_separation(request, 9.5);
    for (std::size_t index = 0; index < value.residual.samples.size(); ++index) {
      value.residual.samples[index] = static_cast<std::int16_t>(
          value.residual.samples[index] + (index % 2 == 0 ? -1 : 1));
    }
    return codec::Result<codec::SeparationResult>{std::move(value)};
  };
  auto result = codec::profiles::audio::separate_verified_pcm16_offline(
      *archive, backend, request_for(fixture.stream));
  EXPECT_TRUE(result);
  EXPECT_EQ(result->front().reconstruction.maximum_absolute_sample_error,
            std::uint64_t{1});
  EXPECT_EQ(result->front().reconstruction.root_mean_square_sample_error,
            1.0);
  EXPECT_EQ(result->front().reconstruction.backend_reported_error, 9.5);
  EXPECT_EQ(read_bytes(fixture.path), before);
  const auto verification = archive->verify();
  EXPECT_TRUE(verification.ok);
  EXPECT_TRUE(verification.finalized);
  std::filesystem::remove(fixture.path);
}
