#include "test.hpp"

#include <codec/integrity.hpp>
#include <codec/profiles/audio.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace audio_profile = codec::profiles::audio;

namespace {

audio_profile::SeparationModelBundle known_bundle() {
  return {
      .manifest = {
          .model_id = "codec.test.separator",
          .model_version = "1.2.3",
          .license_id = "Apache-2.0",
          .quality_domain = "test",
          .input_sample_rate = 48000,
          .input_channels = 2,
          .window_samples = 8,
          .hop_samples = 4,
          .lookahead_samples = 0,
          .maximum_sources = 2,
          .causal = true,
          .input_tensor_name = "mixture",
          .output_tensor_name = "sources",
      },
      .onnx_model = {
          std::byte{0x08}, std::byte{0x07}, std::byte{0x12},
          std::byte{0x03}, std::byte{0x6f}, std::byte{0x6e},
          std::byte{0x78},
      },
  };
}

std::uint8_t hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  throw std::runtime_error("invalid hexadecimal test fixture");
}

std::vector<std::byte> bytes_from_hex(std::string_view hex) {
  if ((hex.size() % 2U) != 0U) {
    throw std::runtime_error("odd hexadecimal test fixture");
  }
  std::vector<std::byte> bytes;
  bytes.reserve(hex.size() / 2U);
  for (std::size_t offset = 0; offset < hex.size(); offset += 2U) {
    const auto value = static_cast<std::uint8_t>(
        (hex_nibble(hex[offset]) << 4U) | hex_nibble(hex[offset + 1U]));
    bytes.push_back(static_cast<std::byte>(value));
  }
  return bytes;
}

std::vector<std::byte> amb1_fixture() {
  return bytes_from_hex(
      "414d42310001000000550000000000000007"
      "0d24ca63c08904d467eb2ff5d79d582125ebd58821a7a58eb534f01b8d736e1a"
      "0000bb8000020002000000080000000400000000"
      "0014636f6465632e746573742e736570617261746f72"
      "0005312e322e33"
      "000a4170616368652d322e30"
      "000474657374"
      "00076d697874757265"
      "0007736f7572636573"
      "080712036f6e78");
}

void expect_manifest_equal(
    const audio_profile::SeparationModelManifest& actual,
    const audio_profile::SeparationModelManifest& expected) {
  EXPECT_EQ(actual.model_id, expected.model_id);
  EXPECT_EQ(actual.model_version, expected.model_version);
  EXPECT_EQ(actual.license_id, expected.license_id);
  EXPECT_EQ(actual.quality_domain, expected.quality_domain);
  EXPECT_EQ(actual.input_sample_rate, expected.input_sample_rate);
  EXPECT_EQ(actual.input_channels, expected.input_channels);
  EXPECT_EQ(actual.window_samples, expected.window_samples);
  EXPECT_EQ(actual.hop_samples, expected.hop_samples);
  EXPECT_EQ(actual.lookahead_samples, expected.lookahead_samples);
  EXPECT_EQ(actual.maximum_sources, expected.maximum_sources);
  EXPECT_EQ(actual.causal, expected.causal);
  EXPECT_EQ(actual.input_tensor_name, expected.input_tensor_name);
  EXPECT_EQ(actual.output_tensor_name, expected.output_tensor_name);
}

void expect_bundle_equal(const audio_profile::SeparationModelBundle& actual,
                         const audio_profile::SeparationModelBundle& expected) {
  expect_manifest_equal(actual.manifest, expected.manifest);
  EXPECT_EQ(actual.onnx_model, expected.onnx_model);
}

using BundleMutation =
    std::function<void(audio_profile::SeparationModelBundle&)>;

void expect_encode_error(
    const BundleMutation& mutate, codec::ErrorCode expected_code,
    audio_profile::SeparationModelBundleLimits limits = {}) {
  auto bundle = known_bundle();
  mutate(bundle);
  const auto before = bundle;
  const auto encoded =
      audio_profile::encode_separation_model_bundle(bundle, limits);
  EXPECT_FALSE(encoded);
  if (!encoded) EXPECT_EQ(encoded.error().code, expected_code);
  expect_bundle_equal(bundle, before);
}

using EncodedMutation = std::function<void(std::vector<std::byte>&)>;

void expect_decode_error(
    const EncodedMutation& mutate, codec::ErrorCode expected_code,
    audio_profile::SeparationModelBundleLimits limits = {}) {
  auto encoded = amb1_fixture();
  mutate(encoded);
  const auto before = encoded;
  const auto decoded =
      audio_profile::decode_separation_model_bundle(encoded, limits);
  EXPECT_FALSE(decoded);
  if (!decoded) EXPECT_EQ(decoded.error().code, expected_code);
  EXPECT_EQ(encoded, before);
}

void set_u32(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint32_t value) {
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto shift = static_cast<unsigned>((3U - index) * 8U);
    bytes[offset + index] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

void set_u64(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint64_t value) {
  for (std::size_t index = 0; index < 8U; ++index) {
    const auto shift = static_cast<unsigned>((7U - index) * 8U);
    bytes[offset + index] =
        static_cast<std::byte>((value >> shift) & 0xffU);
  }
}

}  // namespace

TEST(audio_model_bundle_matches_the_amb1_fixture) {
  const auto bundle = known_bundle();
  const auto encoded = audio_profile::encode_separation_model_bundle(bundle);
  EXPECT_TRUE(encoded);
  if (encoded) EXPECT_EQ(*encoded, amb1_fixture());
}

TEST(audio_model_bundle_verifies_manifest_model_and_bundle_identity) {
  const auto fixture = amb1_fixture();
  const auto decoded = audio_profile::decode_separation_model_bundle(fixture);
  EXPECT_TRUE(decoded);
  if (!decoded) return;

  expect_manifest_equal(decoded->manifest, known_bundle().manifest);
  EXPECT_EQ(decoded->onnx_model, known_bundle().onnx_model);
  EXPECT_EQ(decoded->model_hash, codec::sha256(decoded->onnx_model));
  EXPECT_EQ(decoded->bundle_hash, codec::sha256(fixture));
  EXPECT_EQ(codec::sha256_hex(decoded->onnx_model),
            std::string{"0d24ca63c08904d467eb2ff5d79d5821"
                        "25ebd58821a7a58eb534f01b8d736e1a"});
  EXPECT_EQ(codec::sha256_hex(fixture),
            std::string{"3c1cf1aae3f87c83358eb69f59c822e7"
                        "190be188fd57cc9f01c905d9c9aec90a"});

  const auto reencoded = audio_profile::encode_separation_model_bundle(
      {.manifest = decoded->manifest, .onnx_model = decoded->onnx_model});
  EXPECT_TRUE(reencoded);
  if (reencoded) EXPECT_EQ(*reencoded, fixture);
}

TEST(audio_model_bundle_rejects_invalid_caller_manifests) {
  const std::vector<BundleMutation> mutations{
      [](auto& value) { value.manifest.model_id.clear(); },
      [](auto& value) { value.manifest.model_version.clear(); },
      [](auto& value) { value.manifest.license_id.clear(); },
      [](auto& value) { value.manifest.quality_domain.clear(); },
      [](auto& value) { value.manifest.input_tensor_name.clear(); },
      [](auto& value) { value.manifest.output_tensor_name.clear(); },
      [](auto& value) { value.manifest.model_id = std::string{"bad\0id", 6}; },
      [](auto& value) { value.manifest.model_version = "bad\nversion"; },
      [](auto& value) { value.manifest.license_id = std::string{1, '\x7f'}; },
      [](auto& value) { value.manifest.quality_domain = std::string{1, '\x80'}; },
      [](auto& value) { value.manifest.input_sample_rate = 0; },
      [](auto& value) { value.manifest.input_channels = 0; },
      [](auto& value) { value.manifest.input_channels = 65; },
      [](auto& value) { value.manifest.window_samples = 0; },
      [](auto& value) { value.manifest.hop_samples = 0; },
      [](auto& value) { value.manifest.hop_samples = 9; },
      [](auto& value) { value.manifest.lookahead_samples = 9; },
      [](auto& value) { value.manifest.lookahead_samples = 1; },
      [](auto& value) { value.manifest.maximum_sources = 0; },
      [](auto& value) { value.manifest.maximum_sources = 65; },
      [](auto& value) {
        value.manifest.output_tensor_name = value.manifest.input_tensor_name;
      },
      [](auto& value) { value.onnx_model.clear(); },
  };
  for (const auto& mutation : mutations) {
    expect_encode_error(mutation, codec::ErrorCode::invalid_argument);
  }
}

TEST(audio_model_bundle_enforces_caller_resource_limits) {
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 0,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 1024});
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 0,
       .maximum_text_bytes = 1024});
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 0});
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 19});
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 6,
       .maximum_text_bytes = 1024});
  expect_encode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 141,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 1024});
}

TEST(audio_model_bundle_rejects_malformed_or_noncanonical_bytes) {
  expect_decode_error(
      [](auto& value) { value[0] = std::byte{'X'}; },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { value[5] = std::byte{0x03}; },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 6, 84); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 6, 86); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u64(value, 10, 6); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u64(value, 10, 8); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u64(value, 10, 0); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { value[18] ^= std::byte{0x01}; },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { value.back() ^= std::byte{0x01}; },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { value.push_back(std::byte{0}); },
      codec::ErrorCode::model_incompatible);

  for (const std::size_t size :
       std::vector<std::size_t>{0, 1, 3, 4, 17, 49, 50, 70, 134, 141}) {
    expect_decode_error(
        [size](auto& value) { value.resize(size); },
        codec::ErrorCode::model_incompatible);
  }
}

TEST(audio_model_bundle_rejects_invalid_decoded_manifest_values) {
  expect_decode_error(
      [](auto& value) { value[72] = std::byte{'\n'}; },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 50, 0); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) {
        value[54] = std::byte{0};
        value[55] = std::byte{65};
      },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 58, 0); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 62, 9); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) { set_u32(value, 66, 1); },
      codec::ErrorCode::model_incompatible);
  expect_decode_error(
      [](auto& value) {
        const std::string mixture = "mixture";
        for (std::size_t index = 0; index < mixture.size(); ++index) {
          value[128 + index] = static_cast<std::byte>(mixture[index]);
        }
      },
      codec::ErrorCode::model_incompatible);
}

TEST(audio_model_bundle_enforces_decoder_resource_limits) {
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 0,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 1024});
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 0,
       .maximum_text_bytes = 1024});
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::invalid_argument,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 0});
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 141,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 1024});
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 6,
       .maximum_text_bytes = 1024});
  expect_decode_error(
      [](auto&) {}, codec::ErrorCode::resource_exhausted,
      {.maximum_bundle_bytes = 1024,
       .maximum_model_bytes = 1024,
       .maximum_text_bytes = 19});
}
