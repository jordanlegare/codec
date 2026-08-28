#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

class DeterministicBackend final : public codec::SeparationBackend {
 public:
  std::string name() const override { return "test-runtime"; }
  bool available() const noexcept override { return true; }

  codec::Result<codec::SeparationResult> separate(
      const codec::SeparationRequest& request) override {
    ++calls;
    last_request = request;
    codec::WavPcm16 stem = request.mixture;
    codec::WavPcm16 residual = request.mixture;
    for (std::size_t index = 0;
         index < request.mixture.samples.size(); ++index) {
      stem.samples[index] = static_cast<std::int16_t>(
          request.mixture.samples[index] / 2);
      residual.samples[index] = static_cast<std::int16_t>(
          request.mixture.samples[index] - stem.samples[index]);
    }
    return codec::SeparationResult{
        .stems = {std::move(stem)},
        .residual = std::move(residual),
        .mixture_reconstruction_error = 0.25,
        .model_hash =
            "000102030405060708090a0b0c0d0e0f"
            "101112131415161718191a1b1c1d1e1f",
        .provider = "test-cpu",
    };
  }

  std::size_t calls{};
  codec::SeparationRequest last_request;
};

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

  std::filesystem::remove(archive_path);
  std::filesystem::remove(wav_path);
}
