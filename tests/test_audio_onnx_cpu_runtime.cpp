#include "test.hpp"

#include <codec/profiles/audio.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace audio_profile = codec::profiles::audio;

namespace {

using Bytes = std::vector<std::byte>;

void append_varint(Bytes& output, std::uint64_t value) {
  while (value >= 0x80U) {
    output.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
    value >>= 7U;
  }
  output.push_back(static_cast<std::byte>(value));
}

void append_key(Bytes& output, std::uint32_t field, std::uint8_t wire) {
  append_varint(output, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void append_uint(Bytes& output, std::uint32_t field, std::uint64_t value) {
  append_key(output, field, 0U);
  append_varint(output, value);
}

void append_bytes(Bytes& output, std::uint32_t field,
                  std::span<const std::byte> value) {
  append_key(output, field, 2U);
  append_varint(output, value.size());
  output.insert(output.end(), value.begin(), value.end());
}

void append_string(Bytes& output, std::uint32_t field,
                   std::string_view value) {
  append_bytes(output, field, std::as_bytes(std::span{value.data(),
                                                       value.size()}));
}

Bytes dimension(std::int64_t value) {
  Bytes encoded;
  append_uint(encoded, 1U, static_cast<std::uint64_t>(value));
  return encoded;
}

Bytes tensor_shape(std::span<const std::int64_t> dimensions) {
  Bytes encoded;
  for (const auto value : dimensions) {
    const auto item = dimension(value);
    append_bytes(encoded, 1U, item);
  }
  return encoded;
}

Bytes tensor_type(std::span<const std::int64_t> dimensions,
                  std::uint64_t element_type = 1U) {
  Bytes tensor;
  append_uint(tensor, 1U, element_type);
  const auto shape = tensor_shape(dimensions);
  append_bytes(tensor, 2U, shape);

  Bytes type;
  append_bytes(type, 1U, tensor);
  return type;
}

Bytes value_info(std::string_view name,
                 std::span<const std::int64_t> dimensions,
                 std::uint64_t element_type = 1U) {
  Bytes encoded;
  append_string(encoded, 1U, name);
  const auto type = tensor_type(dimensions, element_type);
  append_bytes(encoded, 2U, type);
  return encoded;
}

Bytes axes_attribute() {
  Bytes packed_axes;
  append_varint(packed_axes, 1U);

  Bytes encoded;
  append_string(encoded, 1U, "axes");
  append_bytes(encoded, 8U, packed_axes);
  append_uint(encoded, 20U, 7U);  // AttributeProto::INTS
  return encoded;
}

Bytes unsqueeze_node(std::string_view input_name,
                     std::string_view output_name) {
  Bytes encoded;
  append_string(encoded, 1U, input_name);
  append_string(encoded, 2U, output_name);
  append_string(encoded, 3U, "codec_identity_unsqueeze");
  append_string(encoded, 4U, "Unsqueeze");
  const auto axes = axes_attribute();
  append_bytes(encoded, 5U, axes);
  return encoded;
}

Bytes identity_model(std::string_view graph_input = "mixture",
                     std::string_view graph_output = "sources",
                     std::array<std::int64_t, 3> input_shape = {1, 1, 4},
                     std::array<std::int64_t, 4> output_shape = {1, 1, 1, 4},
                     std::uint64_t input_type = 1U,
                     bool extra_input = false) {
  Bytes graph;
  const auto node = unsqueeze_node(graph_input, graph_output);
  append_bytes(graph, 1U, node);
  append_string(graph, 2U, "codec-d9-test-graph");
  const auto input = value_info(graph_input, input_shape, input_type);
  append_bytes(graph, 11U, input);
  if (extra_input) {
    const auto unused = value_info("unused", input_shape, input_type);
    append_bytes(graph, 11U, unused);
  }
  const auto output = value_info(graph_output, output_shape);
  append_bytes(graph, 12U, output);

  Bytes opset;
  append_uint(opset, 2U, 11U);

  Bytes model;
  append_uint(model, 1U, 8U);
  append_string(model, 2U, "codec-d9-tests");
  append_bytes(model, 7U, graph);
  append_bytes(model, 8U, opset);
  return model;
}

audio_profile::SeparationModelBundle model_bundle(Bytes model) {
  return {
      .manifest = {
          .model_id = "codec.test.identity-separator",
          .model_version = "1",
          .license_id = "MIT",
          .quality_domain = "contract-test",
          .input_sample_rate = 8000,
          .input_channels = 1,
          .window_samples = 4,
          .hop_samples = 2,
          .lookahead_samples = 0,
          .maximum_sources = 1,
          .causal = true,
          .input_tensor_name = "mixture",
          .output_tensor_name = "sources",
      },
      .onnx_model = std::move(model),
  };
}

audio_profile::VerifiedSeparationModelBundle verified_bundle(
    Bytes model = identity_model()) {
  const auto encoded =
      audio_profile::encode_separation_model_bundle(model_bundle(std::move(model)));
  EXPECT_TRUE(encoded);
  if (!encoded) return {};
  const auto verified = audio_profile::decode_separation_model_bundle(*encoded);
  EXPECT_TRUE(verified);
  return verified ? *verified : audio_profile::VerifiedSeparationModelBundle{};
}

[[maybe_unused]] std::string runtime_library() {
#ifdef CODEC_TEST_ONNXRUNTIME_LIBRARY
  return CODEC_TEST_ONNXRUNTIME_LIBRARY;
#else
  return {};
#endif
}

[[maybe_unused]] std::string hash_hex(const codec::Sha256& hash) {
  return codec::sha256_hex(std::as_bytes(std::span{hash}));
}

[[maybe_unused]] audio_profile::OnnxCpuSeparationOptions runtime_options() {
  return {
      .runtime_library = runtime_library(),
      .intra_op_threads = 1,
      .inter_op_threads = 1,
      .limits = {},
  };
}

[[maybe_unused]] codec::SeparationRequest separation_request(
    const audio_profile::VerifiedSeparationModelBundle& bundle) {
  return {
      .mixture = codec::WavPcm16{
          .sample_rate = 8000,
          .channels = 1,
          .samples = {-32768, -1000, 0, 1000, 32767, 1234},
      },
      .maximum_sources = 1,
      .model_bundle = hash_hex(bundle.bundle_hash),
  };
}

[[maybe_unused]] std::filesystem::path test_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-d9-" + std::string{name});
}

struct ArchiveFixture {
  std::filesystem::path path;
  codec::StreamId stream;
};

[[maybe_unused]] ArchiveFixture make_archive() {
  const auto path = test_path("runtime-integration.coda");
  std::filesystem::remove(path);
  const auto stream = codec::derive_stream_id("d9-runtime-integration");
  auto writer_result = codec::CodaWriter::create(path);
  EXPECT_TRUE(writer_result);
  auto writer = std::move(*writer_result);
  const std::array source_bytes{std::byte{0x01}, std::byte{0x02}};
  auto source = writer.append(codec::RecordType::source_bytes, stream, 100,
                              200, source_bytes);
  EXPECT_TRUE(source);
  const codec::Pcm16State state{
      .sample_rate = 8000,
      .channels = 1,
      .samples = {-32768, -1000, 0, 1000, 32767, 1234},
  };
  const auto encoded = codec::encode_pcm16_state(state);
  EXPECT_TRUE(encoded);
  auto state_record = writer.append(codec::RecordType::pcm16, stream, 100,
                                    200, *encoded);
  EXPECT_TRUE(state_record);
  const std::array inputs{*source};
  EXPECT_TRUE(writer.append_stream_provenance(
      *state_record, codec::TruthClass::state_exact, inputs,
      codec::ProvenanceProcess{
          .operation = "audio.pcm16.canonicalize",
          .implementation_id = "codec-d9-test",
          .implementation_version = "1",
          .implementation_hash = std::nullopt,
          .configuration_hash = std::nullopt,
          .created_utc_ns = 200,
          .details_type = {},
          .details = {},
      }));
  EXPECT_TRUE(writer.finalize());
  return {path, stream};
}

[[maybe_unused]] void expect_factory_error(
    audio_profile::VerifiedSeparationModelBundle bundle,
    codec::ErrorCode code) {
  const auto backend = audio_profile::create_onnx_cpu_separation_backend(
      bundle, runtime_options());
  EXPECT_FALSE(backend);
  if (!backend) EXPECT_EQ(backend.error().code, code);
}

}  // namespace

TEST(audio_onnx_cpu_runtime_reports_build_support_truthfully) {
#ifdef CODEC_TEST_ONNXRUNTIME_LIBRARY
  EXPECT_TRUE(audio_profile::onnx_cpu_separation_runtime_compiled());
#else
  EXPECT_FALSE(audio_profile::onnx_cpu_separation_runtime_compiled());
  const auto result = audio_profile::create_onnx_cpu_separation_backend(
      verified_bundle(), runtime_options());
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::model_incompatible);
#endif
}

TEST(audio_onnx_cpu_runtime_executes_bounded_overlap_add) {
#ifndef CODEC_TEST_ONNXRUNTIME_LIBRARY
  return;
#else
  const auto bundle = verified_bundle();
  auto backend = audio_profile::create_onnx_cpu_separation_backend(
      bundle, runtime_options());
  EXPECT_TRUE(backend);
  if (!backend) return;
  EXPECT_TRUE((*backend)->available());
  EXPECT_EQ((*backend)->name(), std::string{"codec-onnxruntime-cpu"});

  const auto request = separation_request(bundle);
  const auto before = request.mixture.samples;
  const auto result = (*backend)->separate(request);
  EXPECT_TRUE(result);
  if (!result) return;
  EXPECT_EQ(request.mixture.samples, before);
  EXPECT_EQ(result->stems.size(), std::size_t{1});
  EXPECT_EQ(result->stems.front().sample_rate, request.mixture.sample_rate);
  EXPECT_EQ(result->stems.front().channels, request.mixture.channels);
  EXPECT_EQ(result->stems.front().samples, request.mixture.samples);
  EXPECT_TRUE(std::all_of(result->residual.samples.begin(),
                          result->residual.samples.end(),
                          [](std::int16_t value) { return value == 0; }));
  EXPECT_EQ(result->mixture_reconstruction_error, 0.0);
  EXPECT_EQ(result->model_hash, hash_hex(bundle.model_hash));
  EXPECT_TRUE(result->provider.starts_with("onnxruntime-cpu/1.29."));
#endif
}

TEST(audio_onnx_cpu_runtime_drives_d7_verified_state_to_derived_outputs) {
#ifndef CODEC_TEST_ONNXRUNTIME_LIBRARY
  return;
#else
  const auto bundle = verified_bundle();
  auto backend = audio_profile::create_onnx_cpu_separation_backend(
      bundle, runtime_options());
  EXPECT_TRUE(backend);
  if (!backend) return;
  const auto fixture = make_archive();
  const auto before = std::filesystem::file_size(fixture.path);
  const auto archive = codec::CodaArchive::open(fixture.path);
  EXPECT_TRUE(archive);
  if (!archive) return;
  const audio_profile::OfflinePcm16SeparationRequest request{
      .states = audio_profile::Pcm16StateQuery{
          .stream = fixture.stream,
          .time = codec::RecordTimeRange{.begin_ns = 100, .end_ns = 200},
          .maximum_results = 1,
          .maximum_encoded_bytes = 1024 * 1024,
      },
      .maximum_sources = 1,
      .model_bundle = hash_hex(bundle.bundle_hash),
      .created_utc_ns = 300,
      .limits = audio_profile::OfflinePcm16SeparationLimits{
          .maximum_output_bytes = 1024 * 1024,
      },
  };
  const auto separated = audio_profile::separate_verified_pcm16_offline(
      *archive, **backend, request);
  EXPECT_TRUE(separated);
  if (!separated) return;
  EXPECT_EQ(separated->size(), std::size_t{1});
  EXPECT_EQ(separated->front().stems.size(), std::size_t{1});
  EXPECT_EQ(separated->front().stems.front().output.truth,
            codec::TruthClass::derived);
  EXPECT_EQ(separated->front().residual.output.truth,
            codec::TruthClass::derived);
  EXPECT_EQ(separated->front().stems.front().state.samples,
            separated->front().input.state.samples);
  EXPECT_TRUE(std::all_of(
      separated->front().residual.state.samples.begin(),
      separated->front().residual.state.samples.end(),
      [](std::int16_t value) { return value == 0; }));
  EXPECT_EQ(std::filesystem::file_size(fixture.path), before);
#endif
}

TEST(audio_onnx_cpu_runtime_revalidates_bundle_and_graph_compatibility) {
#ifndef CODEC_TEST_ONNXRUNTIME_LIBRARY
  return;
#else
  auto wrong_model_hash = verified_bundle();
  wrong_model_hash.model_hash.front() ^= 0xffU;
  expect_factory_error(std::move(wrong_model_hash),
                       codec::ErrorCode::model_incompatible);

  auto wrong_bundle_hash = verified_bundle();
  wrong_bundle_hash.bundle_hash.back() ^= 0xffU;
  expect_factory_error(std::move(wrong_bundle_hash),
                       codec::ErrorCode::model_incompatible);

  expect_factory_error(verified_bundle(Bytes{std::byte{0x08}}),
                       codec::ErrorCode::model_incompatible);
  expect_factory_error(verified_bundle(identity_model("wrong-input")),
                       codec::ErrorCode::model_incompatible);
  expect_factory_error(
      verified_bundle(identity_model("mixture", "sources", {1, 2, 4})),
      codec::ErrorCode::model_incompatible);
  expect_factory_error(
      verified_bundle(identity_model("mixture", "sources", {1, 1, 4},
                                     {1, 1, 1, 5})),
      codec::ErrorCode::model_incompatible);
  expect_factory_error(
      verified_bundle(identity_model("mixture", "sources", {1, 1, 4},
                                     {1, 1, 1, 4}, 7U)),
      codec::ErrorCode::model_incompatible);
  expect_factory_error(
      verified_bundle(identity_model("mixture", "sources", {1, 1, 4},
                                     {1, 1, 1, 4}, 1U, true)),
      codec::ErrorCode::model_incompatible);

  auto missing = runtime_options();
  missing.runtime_library = "/codec/does/not/exist/libonnxruntime.so";
  const auto unavailable = audio_profile::create_onnx_cpu_separation_backend(
      verified_bundle(), missing);
  EXPECT_FALSE(unavailable);
  if (!unavailable) {
    EXPECT_EQ(unavailable.error().code, codec::ErrorCode::model_incompatible);
  }
#endif
}

TEST(audio_onnx_cpu_runtime_enforces_options_requests_and_resource_bounds) {
#ifndef CODEC_TEST_ONNXRUNTIME_LIBRARY
  return;
#else
  const auto bundle = verified_bundle();
  auto options = runtime_options();
  options.intra_op_threads = 0;
  auto invalid = audio_profile::create_onnx_cpu_separation_backend(bundle,
                                                                   options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  options = runtime_options();
  options.limits.maximum_windows = 0;
  invalid = audio_profile::create_onnx_cpu_separation_backend(bundle, options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  options = runtime_options();
  options.inter_op_threads = 65;
  invalid = audio_profile::create_onnx_cpu_separation_backend(bundle, options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  options = runtime_options();
  options.limits.maximum_input_frames = 0;
  invalid = audio_profile::create_onnx_cpu_separation_backend(bundle, options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  options = runtime_options();
  options.limits.maximum_output_samples = 0;
  invalid = audio_profile::create_onnx_cpu_separation_backend(bundle, options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  options = runtime_options();
  options.runtime_library = std::string{"bad\0library", 11};
  invalid = audio_profile::create_onnx_cpu_separation_backend(bundle, options);
  EXPECT_FALSE(invalid);
  if (!invalid) EXPECT_EQ(invalid.error().code, codec::ErrorCode::invalid_argument);

  auto backend = audio_profile::create_onnx_cpu_separation_backend(
      bundle, runtime_options());
  EXPECT_TRUE(backend);
  if (!backend) return;

  auto request = separation_request(bundle);
  request.model_bundle[0] = request.model_bundle[0] == '0' ? '1' : '0';
  auto result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::model_incompatible);

  request = separation_request(bundle);
  request.mixture.sample_rate = 16000;
  result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  request = separation_request(bundle);
  request.maximum_sources = 0;
  result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  request = separation_request(bundle);
  request.maximum_sources = 65;
  result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  request = separation_request(bundle);
  request.mixture.samples.clear();
  result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  request = separation_request(bundle);
  request.mixture.channels = 2;
  request.mixture.samples.pop_back();
  result = (*backend)->separate(request);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);

  auto bounded = runtime_options();
  bounded.limits.maximum_input_frames = 5;
  backend = audio_profile::create_onnx_cpu_separation_backend(bundle, bounded);
  EXPECT_TRUE(backend);
  if (!backend) return;
  result = (*backend)->separate(separation_request(bundle));
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  bounded = runtime_options();
  bounded.limits.maximum_windows = 2;
  backend = audio_profile::create_onnx_cpu_separation_backend(bundle, bounded);
  EXPECT_TRUE(backend);
  if (!backend) return;
  result = (*backend)->separate(separation_request(bundle));
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);

  bounded = runtime_options();
  bounded.limits.maximum_output_samples = 11;
  backend = audio_profile::create_onnx_cpu_separation_backend(bundle, bounded);
  EXPECT_TRUE(backend);
  if (!backend) return;
  result = (*backend)->separate(separation_request(bundle));
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
#endif
}
