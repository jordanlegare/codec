#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <string>
#include <type_traits>

namespace audio_profile = codec::profiles::audio;

static_assert(std::is_same_v<audio_profile::WavPcm16, codec::WavPcm16>);
static_assert(std::is_same_v<audio_profile::CarrierBand, codec::CarrierBand>);
static_assert(std::is_same_v<audio_profile::WatermarkPolicy,
                             codec::WatermarkPolicy>);
static_assert(std::is_same_v<audio_profile::WatermarkEmbedReport,
                             codec::WatermarkEmbedReport>);
static_assert(std::is_same_v<audio_profile::WatermarkObservation,
                             codec::WatermarkObservation>);
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
            std::string("w1"));

  auto backend = audio_profile::default_separation_backend();
  EXPECT_TRUE(static_cast<bool>(backend));
  EXPECT_EQ(backend->name(), std::string("unavailable"));
  EXPECT_FALSE(backend->available());
}
