#include "test.hpp"

#include <codec/audio.hpp>
#include <codec/watermark.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

namespace {

codec::WavPcm16 quiet_program(std::uint32_t sample_rate,
                              double duration_seconds) {
  codec::WavPcm16 audio;
  audio.sample_rate = sample_rate;
  audio.channels = 1;
  const auto count = static_cast<std::size_t>(sample_rate * duration_seconds);
  audio.samples.resize(count);
  for (std::size_t index = 0; index < count; ++index) {
    const double time = static_cast<double>(index) / sample_rate;
    audio.samples[index] = static_cast<std::int16_t>(
        std::sin(2.0 * 3.14159265358979323846 * 440.0 * time) * 1200.0);
  }
  return audio;
}

std::filesystem::path wav_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-test-" + std::string{name});
}

bool contains_code(const std::vector<codec::WatermarkObservation>& observations,
                   std::uint16_t code, codec::CarrierBand band) {
  return std::any_of(observations.begin(), observations.end(),
                     [=](const auto& item) {
                       return item.code == code && item.band == band;
                     });
}

}  // namespace

TEST(pcm16_wav_round_trips_samples_exactly) {
  const auto path = wav_path("sample-exact.wav");
  std::filesystem::remove(path);
  auto original = quiet_program(48000, 0.25);
  original.channels = 2;
  original.samples.resize(original.samples.size() * 2, -321);
  EXPECT_TRUE(original.write(path));
  auto decoded = codec::WavPcm16::read(path);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->sample_rate, original.sample_rate);
  EXPECT_EQ(decoded->channels, original.channels);
  EXPECT_EQ(decoded->samples, original.samples);
  std::filesystem::remove(path);
}

TEST(w1_reference_carrier_recovers_the_embedded_code) {
  auto audio = quiet_program(48000, 2.2);
  codec::WatermarkPolicy policy;
  policy.amplitude_dbfs = -32.0;
  auto embedded = codec::embed_watermark(audio, 0x4a31,
                                         codec::CarrierBand::w1, policy);
  EXPECT_TRUE(embedded);
  EXPECT_TRUE(embedded->frames_embedded >= 2);
  const auto observations = codec::detect_watermarks(audio, policy);
  EXPECT_TRUE(observations);
  EXPECT_TRUE(contains_code(*observations, 0x4a31,
                            codec::CarrierBand::w1));
}

TEST(w2_is_rejected_below_the_qualified_sample_rate) {
  auto audio = quiet_program(48000, 1.0);
  codec::WatermarkPolicy policy;
  auto result = codec::embed_watermark(audio, 0x1007,
                                       codec::CarrierBand::w2, policy);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::watermark_path_unqualified);
}

TEST(w2_recovers_a_code_on_a_guarded_96khz_path) {
  auto audio = quiet_program(96000, 2.2);
  codec::WatermarkPolicy policy;
  policy.amplitude_dbfs = -32.0;
  auto embedded = codec::embed_watermark(audio, 0x92b4,
                                         codec::CarrierBand::w2, policy);
  EXPECT_TRUE(embedded);
  const auto observations = codec::detect_watermarks(audio, policy);
  EXPECT_TRUE(observations);
  EXPECT_TRUE(contains_code(*observations, 0x92b4,
                            codec::CarrierBand::w2));
}

TEST(w1_detection_tolerates_a_non_aligned_live_capture_start) {
  auto audio = quiet_program(48000, 2.4);
  codec::WatermarkPolicy policy;
  policy.amplitude_dbfs = -32.0;
  EXPECT_TRUE(codec::embed_watermark(audio, 0x22d9,
                                     codec::CarrierBand::w1, policy));
  const auto cropped_frames = static_cast<std::size_t>(audio.sample_rate * 0.013);
  audio.samples.erase(audio.samples.begin(),
                      audio.samples.begin() + cropped_frames * audio.channels);
  const auto observations = codec::detect_watermarks(audio, policy);
  EXPECT_TRUE(observations);
  EXPECT_TRUE(contains_code(*observations, 0x22d9,
                            codec::CarrierBand::w1));
}

TEST(watermark_policy_cannot_relabel_audible_frequencies_as_w2) {
  auto audio = quiet_program(48000, 1.0);
  codec::WatermarkPolicy policy;
  policy.w2_minimum_sample_rate = 48000;
  policy.w2_zero_hz = 1000.0;
  policy.w2_one_hz = 2000.0;
  auto result = codec::embed_watermark(audio, 0x0102,
                                       codec::CarrierBand::w2, policy);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::watermark_path_unqualified);
}

TEST(wav_writer_refuses_to_replace_its_input_path) {
  const auto path = wav_path("no-replace.wav");
  std::filesystem::remove(path);
  auto original = quiet_program(48000, 0.1);
  EXPECT_TRUE(original.write(path));
  auto changed = quiet_program(48000, 0.2);
  auto replaced = changed.write(path);
  EXPECT_FALSE(replaced);
  auto reread = codec::WavPcm16::read(path);
  EXPECT_TRUE(reread);
  EXPECT_EQ(reread->samples, original.samples);
  std::filesystem::remove(path);
}
