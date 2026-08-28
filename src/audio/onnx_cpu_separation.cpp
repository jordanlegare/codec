#include <codec/profiles/audio_onnx_cpu_runtime.hpp>

#include <codec/audio.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef CODEC_HAS_ONNXRUNTIME
#include <onnxruntime_c_api.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

namespace codec::profiles::audio {
namespace {

constexpr std::size_t maximum_runtime_path_bytes = 4096U;
constexpr std::uint32_t maximum_runtime_threads = 64U;
constexpr std::size_t maximum_runtime_error_bytes = 512U;
constexpr std::size_t maximum_runtime_version_bytes = 128U;
constexpr std::size_t maximum_profile_sources = 64U;

#ifdef CODEC_HAS_ONNXRUNTIME
bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t* output) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
  *output = left + right;
  return true;
}

bool checked_multiply(std::uint64_t left, std::uint64_t right,
                      std::uint64_t* output) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    return false;
  }
  *output = left * right;
  return true;
}
#endif

bool hashes_equal(const Sha256& left, const Sha256& right) noexcept {
  std::uint8_t difference{};
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
  }
  return difference == 0U;
}

#ifdef CODEC_HAS_ONNXRUNTIME
std::string hash_hex(const Sha256& hash) {
  return sha256_hex(std::as_bytes(std::span{hash}));
}
#endif

Result<void> validate_options(const OnnxCpuSeparationOptions& options) {
  if (options.intra_op_threads == 0U ||
      options.intra_op_threads > maximum_runtime_threads ||
      options.inter_op_threads == 0U ||
      options.inter_op_threads > maximum_runtime_threads) {
    return fail(ErrorCode::invalid_argument,
                "ONNX CPU runtime thread counts must be between 1 and 64");
  }
  if (options.limits.maximum_input_frames == 0U ||
      options.limits.maximum_output_samples == 0U ||
      options.limits.maximum_windows == 0U) {
    return fail(ErrorCode::invalid_argument,
                "ONNX CPU runtime limits must be non-zero");
  }
  if (options.runtime_library.size() > maximum_runtime_path_bytes ||
      options.runtime_library.find('\0') != std::string::npos) {
    return fail(ErrorCode::invalid_argument,
                "ONNX CPU runtime library name is invalid");
  }
  return {};
}

Result<void> revalidate_bundle(
    const VerifiedSeparationModelBundle& bundle) {
  const auto encoded = encode_separation_model_bundle(
      {.manifest = bundle.manifest, .onnx_model = bundle.onnx_model});
  if (!encoded) {
    if (encoded.error().code == ErrorCode::resource_exhausted) {
      return encoded.error();
    }
    return fail(ErrorCode::model_incompatible,
                "verified separation ModelBundle is not a valid AMB1 value");
  }
  if (!hashes_equal(sha256(bundle.onnx_model), bundle.model_hash) ||
      !hashes_equal(sha256(*encoded), bundle.bundle_hash)) {
    return fail(ErrorCode::model_incompatible,
                "verified separation ModelBundle identity does not match its bytes");
  }
  return {};
}

#ifdef CODEC_HAS_ONNXRUNTIME

std::string bounded_runtime_text(const char* value) {
  if (value == nullptr) return "unknown ONNX Runtime error";
  std::string output;
  output.reserve(std::min(std::strlen(value), maximum_runtime_error_bytes));
  for (std::size_t index = 0;
       value[index] != '\0' && index < maximum_runtime_error_bytes; ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    output.push_back(byte >= 0x20U && byte <= 0x7eU
                         ? static_cast<char>(byte)
                         : '?');
  }
  return output;
}

std::optional<Error> runtime_error(const OrtApi* api, OrtStatus* status,
                                   ErrorCode code,
                                   std::string_view context) {
  if (status == nullptr) return std::nullopt;
  std::string message{context};
  message += ": ";
  message += bounded_runtime_text(api->GetErrorMessage(status));
  api->ReleaseStatus(status);
  return Error{code, std::move(message), false};
}

class DynamicLibrary {
 public:
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  ~DynamicLibrary() {
#ifdef _WIN32
    if (handle_ != nullptr) FreeLibrary(handle_);
#else
    if (handle_ != nullptr) dlclose(handle_);
#endif
  }

  static Result<std::shared_ptr<DynamicLibrary>> open(
      std::string library_name) {
    if (library_name.empty()) {
#ifdef _WIN32
      library_name = "onnxruntime.dll";
#elif defined(__APPLE__)
      library_name = "libonnxruntime.dylib";
#else
      library_name = "libonnxruntime.so";
#endif
    }

    auto library = std::shared_ptr<DynamicLibrary>{new DynamicLibrary};
#ifdef _WIN32
    library->handle_ = LoadLibraryA(library_name.c_str());
    if (library->handle_ == nullptr) {
      return fail<std::shared_ptr<DynamicLibrary>>(
          ErrorCode::model_incompatible,
          "ONNX Runtime CPU library could not be loaded");
    }
#else
    dlerror();
    library->handle_ = dlopen(library_name.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library->handle_ == nullptr) {
      const auto detail = bounded_runtime_text(dlerror());
      return fail<std::shared_ptr<DynamicLibrary>>(
          ErrorCode::model_incompatible,
          "ONNX Runtime CPU library could not be loaded: " + detail);
    }
#endif
    return library;
  }

  const OrtApiBase* api_base() const noexcept {
    using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
    GetApiBase function{};
#ifdef _WIN32
    const auto symbol = GetProcAddress(handle_, "OrtGetApiBase");
    if (symbol == nullptr || sizeof(symbol) != sizeof(function)) return nullptr;
    std::memcpy(&function, &symbol, sizeof(function));
#else
    dlerror();
    const auto symbol = dlsym(handle_, "OrtGetApiBase");
    if (symbol == nullptr || dlerror() != nullptr ||
        sizeof(symbol) != sizeof(function)) {
      return nullptr;
    }
    std::memcpy(&function, &symbol, sizeof(function));
#endif
    return function == nullptr ? nullptr : function();
  }

 private:
  DynamicLibrary() = default;

#ifdef _WIN32
  HMODULE handle_{};
#else
  void* handle_{};
#endif
};

struct RuntimeState {
  ~RuntimeState() {
    if (api != nullptr) {
      if (session != nullptr) api->ReleaseSession(session);
      if (session_options != nullptr) {
        api->ReleaseSessionOptions(session_options);
      }
      if (environment != nullptr) api->ReleaseEnv(environment);
    }
  }

  std::shared_ptr<DynamicLibrary> library;
  const OrtApiBase* api_base{};
  const OrtApi* api{};
  OrtEnv* environment{};
  OrtSessionOptions* session_options{};
  OrtSession* session{};
  std::string version;
};

struct TypeInfoOwner {
  const OrtApi* api{};
  OrtTypeInfo* value{};
  ~TypeInfoOwner() {
    if (value != nullptr) api->ReleaseTypeInfo(value);
  }
};

struct TensorInfoOwner {
  const OrtApi* api{};
  OrtTensorTypeAndShapeInfo* value{};
  ~TensorInfoOwner() {
    if (value != nullptr) api->ReleaseTensorTypeAndShapeInfo(value);
  }
};

struct ValueOwner {
  const OrtApi* api{};
  OrtValue* value{};
  ~ValueOwner() {
    if (value != nullptr) api->ReleaseValue(value);
  }
};

struct MemoryInfoOwner {
  const OrtApi* api{};
  OrtMemoryInfo* value{};
  ~MemoryInfoOwner() {
    if (value != nullptr) api->ReleaseMemoryInfo(value);
  }
};

struct TensorContract {
  ONNXTensorElementDataType element_type{
      ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED};
  std::vector<std::int64_t> dimensions;
};

Result<TensorContract> session_tensor_contract(
    const RuntimeState& runtime, bool input) {
  TypeInfoOwner type_info{.api = runtime.api};
  auto status = input
                    ? runtime.api->SessionGetInputTypeInfo(runtime.session, 0U,
                                                           &type_info.value)
                    : runtime.api->SessionGetOutputTypeInfo(runtime.session, 0U,
                                                            &type_info.value);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX graph tensor type could not be read")) {
    return *error;
  }

  ONNXType onnx_type = ONNX_TYPE_UNKNOWN;
  status = runtime.api->GetOnnxTypeFromTypeInfo(type_info.value, &onnx_type);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX graph value type could not be read")) {
    return *error;
  }
  if (onnx_type != ONNX_TYPE_TENSOR) {
    return fail<TensorContract>(ErrorCode::model_incompatible,
                                "ONNX graph input and output must be tensors");
  }

  const OrtTensorTypeAndShapeInfo* tensor_info{};
  status = runtime.api->CastTypeInfoToTensorInfo(type_info.value, &tensor_info);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX tensor type could not be inspected")) {
    return *error;
  }

  TensorContract contract;
  status = runtime.api->GetTensorElementType(tensor_info,
                                              &contract.element_type);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX tensor element type could not be read")) {
    return *error;
  }
  std::size_t dimension_count{};
  status = runtime.api->GetDimensionsCount(tensor_info, &dimension_count);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX tensor rank could not be read")) {
    return *error;
  }
  contract.dimensions.resize(dimension_count);
  if (dimension_count != 0U) {
    status = runtime.api->GetDimensions(tensor_info,
                                        contract.dimensions.data(),
                                        contract.dimensions.size());
    if (auto error = runtime_error(runtime.api, status,
                                   ErrorCode::model_incompatible,
                                   "ONNX tensor dimensions could not be read")) {
      return *error;
    }
  }
  return contract;
}

Result<std::string> session_value_name(const RuntimeState& runtime,
                                       OrtAllocator* allocator, bool input) {
  char* allocated{};
  auto status = input
                    ? runtime.api->SessionGetInputName(runtime.session, 0U,
                                                      allocator, &allocated)
                    : runtime.api->SessionGetOutputName(runtime.session, 0U,
                                                       allocator, &allocated);
  if (auto error = runtime_error(runtime.api, status,
                                 ErrorCode::model_incompatible,
                                 "ONNX graph tensor name could not be read")) {
    return *error;
  }
  if (allocated == nullptr) {
    return fail<std::string>(ErrorCode::model_incompatible,
                             "ONNX graph tensor name is missing");
  }
  std::string name{allocated};
  allocator->Free(allocator, allocated);
  return name;
}

bool compatible_input_dimension(std::int64_t actual,
                                std::uint64_t expected) {
  return actual == -1 ||
         (actual > 0 && static_cast<std::uint64_t>(actual) == expected);
}

Result<std::size_t> validate_session_contract(
    const RuntimeState& runtime,
    const SeparationModelManifest& manifest) {
  std::size_t input_count{};
  std::size_t output_count{};
  if (auto error = runtime_error(
          runtime.api,
          runtime.api->SessionGetInputCount(runtime.session, &input_count),
          ErrorCode::model_incompatible,
          "ONNX graph input count could not be read")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime.api,
          runtime.api->SessionGetOutputCount(runtime.session, &output_count),
          ErrorCode::model_incompatible,
          "ONNX graph output count could not be read")) {
    return *error;
  }
  if (input_count != 1U || output_count != 1U) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX separation graph must have one input and one output");
  }

  OrtAllocator* allocator{};
  if (auto error = runtime_error(
          runtime.api,
          runtime.api->GetAllocatorWithDefaultOptions(&allocator),
          ErrorCode::model_incompatible,
          "ONNX Runtime default allocator is unavailable")) {
    return *error;
  }
  const auto input_name = session_value_name(runtime, allocator, true);
  if (!input_name) return input_name.error();
  const auto output_name = session_value_name(runtime, allocator, false);
  if (!output_name) return output_name.error();
  if (*input_name != manifest.input_tensor_name ||
      *output_name != manifest.output_tensor_name) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX graph tensor names do not match AMB1");
  }

  const auto input = session_tensor_contract(runtime, true);
  if (!input) return input.error();
  const auto output = session_tensor_contract(runtime, false);
  if (!output) return output.error();
  if (input->element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
      output->element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX separation tensors must use float32");
  }
  if (input->dimensions.size() != 3U ||
      output->dimensions.size() != 4U) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX separation tensor ranks do not match AMB1");
  }
  if (!compatible_input_dimension(input->dimensions[0], 1U) ||
      !compatible_input_dimension(input->dimensions[1],
                                  manifest.input_channels) ||
      !compatible_input_dimension(input->dimensions[2],
                                  manifest.window_samples)) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX input tensor dimensions do not match AMB1");
  }

  const auto source_dimension = output->dimensions[1];
  if (output->dimensions[0] != 1 || source_dimension <= 0 ||
      static_cast<std::uint64_t>(source_dimension) >
          manifest.maximum_sources ||
      output->dimensions[2] != manifest.input_channels ||
      output->dimensions[3] != manifest.window_samples) {
    return fail<std::size_t>(ErrorCode::model_incompatible,
                             "ONNX output tensor dimensions do not match AMB1");
  }
  return static_cast<std::size_t>(source_dimension);
}

std::int16_t float_to_pcm16(double value) {
  const auto scaled = value * 32768.0;
  if (scaled <= -32768.0) return std::numeric_limits<std::int16_t>::min();
  if (scaled >= 32767.0) return std::numeric_limits<std::int16_t>::max();
  const auto rounded =
      scaled >= 0.0 ? std::floor(scaled + 0.5) : std::ceil(scaled - 0.5);
  return static_cast<std::int16_t>(rounded);
}

std::int16_t saturate_pcm16(std::int64_t value) {
  if (value < std::numeric_limits<std::int16_t>::min()) {
    return std::numeric_limits<std::int16_t>::min();
  }
  if (value > std::numeric_limits<std::int16_t>::max()) {
    return std::numeric_limits<std::int16_t>::max();
  }
  return static_cast<std::int16_t>(value);
}

class OnnxCpuBackend final : public SeparationBackend {
 public:
  OnnxCpuBackend(std::shared_ptr<RuntimeState> runtime,
                 SeparationModelManifest manifest, Sha256 model_hash,
                 Sha256 bundle_hash, OnnxCpuSeparationOptions options,
                 std::size_t source_count)
      : runtime_(std::move(runtime)),
        manifest_(std::move(manifest)),
        model_hash_(model_hash),
        bundle_reference_(hash_hex(bundle_hash)),
        options_(std::move(options)),
        source_count_(source_count) {}

  std::string name() const override { return "codec-onnxruntime-cpu"; }
  bool available() const noexcept override { return true; }

  Result<SeparationResult> separate(
      const SeparationRequest& request) override {
    try {
      return separate_checked(request);
    } catch (const std::bad_alloc&) {
      return fail<SeparationResult>(
          ErrorCode::resource_exhausted,
          "ONNX CPU separation could not allocate bounded working memory");
    } catch (const std::exception&) {
      return fail<SeparationResult>(
          ErrorCode::internal,
          "ONNX CPU separation failed at the profile boundary");
    } catch (...) {
      return fail<SeparationResult>(
          ErrorCode::internal,
          "ONNX CPU separation failed at the profile boundary");
    }
  }

 private:
  Result<SeparationResult> separate_checked(
      const SeparationRequest& request) {
    const auto canonical = canonicalize_pcm16(request.mixture);
    if (!canonical) return canonical.error();
    if (canonical->samples.empty()) {
      return fail<SeparationResult>(
          ErrorCode::invalid_argument,
          "ONNX CPU separation requires at least one PCM16 frame");
    }
    if (canonical->sample_rate != manifest_.input_sample_rate ||
        canonical->channels != manifest_.input_channels) {
      return fail<SeparationResult>(
          ErrorCode::invalid_argument,
          "PCM16 geometry does not match the separation ModelBundle");
    }
    if (request.maximum_sources == 0U ||
        request.maximum_sources > maximum_profile_sources) {
      return fail<SeparationResult>(
          ErrorCode::invalid_argument,
          "ONNX CPU separation maximum sources must be between 1 and 64");
    }
    if (request.model_bundle != bundle_reference_) {
      return fail<SeparationResult>(
          ErrorCode::model_incompatible,
          "separation request does not reference the bound ModelBundle");
    }
    if (source_count_ > request.maximum_sources) {
      return fail<SeparationResult>(
          ErrorCode::resource_exhausted,
          "ONNX model source count exceeds the request limit");
    }

    const auto frames = static_cast<std::uint64_t>(canonical->frames());
    const auto channels = static_cast<std::uint64_t>(canonical->channels);
    if (frames > options_.limits.maximum_input_frames) {
      return fail<SeparationResult>(
          ErrorCode::resource_exhausted,
          "ONNX CPU separation input exceeds the frame limit");
    }
    const auto hop = static_cast<std::uint64_t>(manifest_.hop_samples);
    const auto window_count = 1U + (frames - 1U) / hop;
    if (window_count > options_.limits.maximum_windows) {
      return fail<SeparationResult>(
          ErrorCode::resource_exhausted,
          "ONNX CPU separation exceeds the window limit");
    }

    std::uint64_t samples_per_source{};
    std::uint64_t output_stream_count{};
    std::uint64_t aggregate_output_samples{};
    std::uint64_t accumulation_elements{};
    std::uint64_t window_elements{};
    std::uint64_t model_output_elements{};
    if (!checked_multiply(frames, channels, &samples_per_source) ||
        !checked_add(static_cast<std::uint64_t>(source_count_), 1U,
                     &output_stream_count) ||
        !checked_multiply(samples_per_source, output_stream_count,
                          &aggregate_output_samples) ||
        !checked_multiply(samples_per_source,
                          static_cast<std::uint64_t>(source_count_),
                          &accumulation_elements) ||
        !checked_multiply(channels, manifest_.window_samples,
                          &window_elements) ||
        !checked_multiply(window_elements,
                          static_cast<std::uint64_t>(source_count_),
                          &model_output_elements) ||
        aggregate_output_samples > options_.limits.maximum_output_samples ||
        frames > std::numeric_limits<std::size_t>::max() /
                     sizeof(std::uint32_t) ||
        samples_per_source > std::numeric_limits<std::size_t>::max() /
                                 sizeof(std::int16_t) ||
        aggregate_output_samples >
            std::numeric_limits<std::size_t>::max() / sizeof(std::int16_t) ||
        accumulation_elements >
            std::numeric_limits<std::size_t>::max() / sizeof(double) ||
        window_elements >
            std::numeric_limits<std::size_t>::max() / sizeof(float) ||
        model_output_elements >
            std::numeric_limits<std::size_t>::max() / sizeof(float)) {
      return fail<SeparationResult>(
          ErrorCode::resource_exhausted,
          "ONNX CPU separation working set exceeds configured bounds");
    }

    std::vector<double> accumulation(
        static_cast<std::size_t>(accumulation_elements), 0.0);
    std::vector<std::uint32_t> overlap_count(
        static_cast<std::size_t>(frames), 0U);
    std::vector<float> input(static_cast<std::size_t>(window_elements), 0.0F);

    for (std::uint64_t window_index = 0U; window_index < window_count;
         ++window_index) {
      const auto start = window_index * hop;
      std::fill(input.begin(), input.end(), 0.0F);
      for (std::uint64_t channel = 0U; channel < channels; ++channel) {
        for (std::uint64_t sample = 0U;
             sample < manifest_.window_samples; ++sample) {
          const auto frame = start + sample;
          if (frame >= frames) break;
          const auto source_index = frame * channels + channel;
          const auto tensor_index =
              channel * manifest_.window_samples + sample;
          input[static_cast<std::size_t>(tensor_index)] =
              static_cast<float>(canonical->samples[
                  static_cast<std::size_t>(source_index)]) /
              32768.0F;
        }
      }

      auto inferred = infer_window(input);
      if (!inferred) return inferred.error();
      const auto expected_values =
          static_cast<std::uint64_t>(source_count_) * window_elements;
      if (inferred->size() != expected_values) {
        return fail<SeparationResult>(
            ErrorCode::inference,
            "ONNX Runtime returned an inconsistent output size");
      }

      for (std::uint64_t sample = 0U;
           sample < manifest_.window_samples; ++sample) {
        const auto frame = start + sample;
        if (frame >= frames) break;
        if (overlap_count[static_cast<std::size_t>(frame)] ==
            std::numeric_limits<std::uint32_t>::max()) {
          return fail<SeparationResult>(
              ErrorCode::resource_exhausted,
              "ONNX CPU separation overlap count is too large");
        }
        ++overlap_count[static_cast<std::size_t>(frame)];
        for (std::uint64_t source = 0U; source < source_count_; ++source) {
          for (std::uint64_t channel = 0U; channel < channels; ++channel) {
            const auto output_index =
                (source * channels + channel) *
                    manifest_.window_samples +
                sample;
            const auto value =
                (*inferred)[static_cast<std::size_t>(output_index)];
            if (!std::isfinite(value)) {
              return fail<SeparationResult>(
                  ErrorCode::inference,
                  "ONNX Runtime returned a non-finite source sample");
            }
            const auto accumulation_index =
                (source * channels + channel) * frames + frame;
            accumulation[static_cast<std::size_t>(accumulation_index)] +=
                static_cast<double>(value);
          }
        }
      }
    }

    std::vector<WavPcm16> stems;
    stems.reserve(source_count_);
    for (std::size_t source = 0U; source < source_count_; ++source) {
      WavPcm16 stem{
          .sample_rate = canonical->sample_rate,
          .channels = canonical->channels,
          .samples = std::vector<std::int16_t>(canonical->samples.size()),
      };
      for (std::uint64_t frame = 0U; frame < frames; ++frame) {
        const auto count = overlap_count[static_cast<std::size_t>(frame)];
        if (count == 0U) {
          return fail<SeparationResult>(
              ErrorCode::inference,
              "ONNX CPU separation left an input frame uncovered");
        }
        for (std::uint64_t channel = 0U; channel < channels; ++channel) {
          const auto accumulation_index =
              (static_cast<std::uint64_t>(source) * channels + channel) *
                  frames +
              frame;
          const auto output_index = frame * channels + channel;
          const auto averaged =
              accumulation[static_cast<std::size_t>(accumulation_index)] /
              static_cast<double>(count);
          stem.samples[static_cast<std::size_t>(output_index)] =
              float_to_pcm16(averaged);
        }
      }
      stems.push_back(std::move(stem));
    }

    WavPcm16 residual{
        .sample_rate = canonical->sample_rate,
        .channels = canonical->channels,
        .samples = std::vector<std::int16_t>(canonical->samples.size()),
    };
    long double squared_error{};
    for (std::size_t index = 0U; index < canonical->samples.size(); ++index) {
      std::int64_t stem_sum{};
      for (const auto& stem : stems) stem_sum += stem.samples[index];
      residual.samples[index] = saturate_pcm16(
          static_cast<std::int64_t>(canonical->samples[index]) - stem_sum);
      const auto reconstructed =
          stem_sum + static_cast<std::int64_t>(residual.samples[index]);
      const auto error =
          static_cast<std::int64_t>(canonical->samples[index]) -
          reconstructed;
      squared_error += static_cast<long double>(error) *
                       static_cast<long double>(error);
    }
    const auto rms = std::sqrt(
        squared_error / static_cast<long double>(canonical->samples.size()));

    return SeparationResult{
        .stems = std::move(stems),
        .residual = std::move(residual),
        .mixture_reconstruction_error = static_cast<double>(rms),
        .model_hash = hash_hex(model_hash_),
        .provider = "onnxruntime-cpu/" + runtime_->version,
    };
  }

  Result<std::vector<float>> infer_window(std::vector<float>& input) {
    MemoryInfoOwner memory_info{.api = runtime_->api};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->CreateCpuMemoryInfo(
                OrtArenaAllocator, OrtMemTypeDefault, &memory_info.value),
            ErrorCode::inference,
            "ONNX Runtime CPU memory descriptor could not be created")) {
      return *error;
    }

    const std::array<std::int64_t, 3> input_shape{
        1, static_cast<std::int64_t>(manifest_.input_channels),
        static_cast<std::int64_t>(manifest_.window_samples)};
    ValueOwner input_value{.api = runtime_->api};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->CreateTensorWithDataAsOrtValue(
                memory_info.value, input.data(), input.size() * sizeof(float),
                input_shape.data(), input_shape.size(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input_value.value),
            ErrorCode::inference,
            "ONNX Runtime input tensor could not be created")) {
      return *error;
    }

    const std::array<const char*, 1> input_names{
        manifest_.input_tensor_name.c_str()};
    const std::array<const OrtValue*, 1> input_values{input_value.value};
    const std::array<const char*, 1> output_names{
        manifest_.output_tensor_name.c_str()};
    ValueOwner output_value{.api = runtime_->api};
    OrtValue* raw_output{};
    const auto status = runtime_->api->Run(
        runtime_->session, nullptr, input_names.data(), input_values.data(),
        input_values.size(), output_names.data(), output_names.size(),
        &raw_output);
    output_value.value = raw_output;
    if (auto error = runtime_error(runtime_->api, status,
                                   ErrorCode::inference,
                                   "ONNX Runtime CPU inference failed")) {
      return *error;
    }
    if (output_value.value == nullptr) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime returned no separation tensor");
    }

    int is_tensor{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->IsTensor(output_value.value, &is_tensor),
            ErrorCode::inference,
            "ONNX Runtime output kind could not be inspected")) {
      return *error;
    }
    if (is_tensor == 0) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime separation output is not a tensor");
    }

    const OrtMemoryInfo* output_memory{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetTensorMemoryInfo(output_value.value,
                                               &output_memory),
            ErrorCode::inference,
            "ONNX Runtime output memory could not be inspected")) {
      return *error;
    }
    OrtMemoryInfoDeviceType device_type{};
    runtime_->api->MemoryInfoGetDeviceType(output_memory, &device_type);
    if (device_type != OrtMemoryInfoDeviceType_CPU) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime separation output is not in CPU memory");
    }

    TensorInfoOwner tensor_info{.api = runtime_->api};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetTensorTypeAndShape(output_value.value,
                                                 &tensor_info.value),
            ErrorCode::inference,
            "ONNX Runtime output shape could not be read")) {
      return *error;
    }
    ONNXTensorElementDataType element_type{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetTensorElementType(tensor_info.value,
                                                &element_type),
            ErrorCode::inference,
            "ONNX Runtime output element type could not be read")) {
      return *error;
    }
    if (element_type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime output is not float32");
    }

    std::size_t dimension_count{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetDimensionsCount(tensor_info.value,
                                               &dimension_count),
            ErrorCode::inference,
            "ONNX Runtime output rank could not be read")) {
      return *error;
    }
    if (dimension_count != 4U) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime output rank changed during execution");
    }
    std::array<std::int64_t, 4> dimensions{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetDimensions(tensor_info.value,
                                         dimensions.data(), dimensions.size()),
            ErrorCode::inference,
            "ONNX Runtime output dimensions could not be read")) {
      return *error;
    }
    const std::array<std::int64_t, 4> expected_dimensions{
        1, static_cast<std::int64_t>(source_count_),
        static_cast<std::int64_t>(manifest_.input_channels),
        static_cast<std::int64_t>(manifest_.window_samples)};
    if (dimensions != expected_dimensions) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime output dimensions changed during execution");
    }

    std::size_t element_count{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetTensorShapeElementCount(tensor_info.value,
                                                      &element_count),
            ErrorCode::inference,
            "ONNX Runtime output size could not be read")) {
      return *error;
    }
    std::uint64_t expected_count{};
    if (!checked_multiply(source_count_, manifest_.input_channels,
                          &expected_count) ||
        !checked_multiply(expected_count, manifest_.window_samples,
                          &expected_count) ||
        expected_count != element_count) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime output size is inconsistent");
    }

    void* raw_data{};
    if (auto error = runtime_error(
            runtime_->api,
            runtime_->api->GetTensorMutableData(output_value.value,
                                                &raw_data),
            ErrorCode::inference,
            "ONNX Runtime output data could not be read")) {
      return *error;
    }
    if (raw_data == nullptr) {
      return fail<std::vector<float>>(
          ErrorCode::inference,
          "ONNX Runtime output data is missing");
    }
    const auto* values = static_cast<const float*>(raw_data);
    return std::vector<float>{values, values + element_count};
  }

  std::shared_ptr<RuntimeState> runtime_;
  SeparationModelManifest manifest_;
  Sha256 model_hash_{};
  std::string bundle_reference_;
  OnnxCpuSeparationOptions options_;
  std::size_t source_count_{};
};

Result<std::shared_ptr<RuntimeState>> create_runtime(
    const VerifiedSeparationModelBundle& bundle,
    const OnnxCpuSeparationOptions& options) {
  auto library = DynamicLibrary::open(options.runtime_library);
  if (!library) return library.error();

  auto runtime = std::make_shared<RuntimeState>();
  runtime->library = std::move(*library);
  runtime->api_base = runtime->library->api_base();
  if (runtime->api_base == nullptr) {
    return fail<std::shared_ptr<RuntimeState>>(
        ErrorCode::model_incompatible,
        "ONNX Runtime library does not export OrtGetApiBase");
  }
  runtime->api = runtime->api_base->GetApi(ORT_API_VERSION);
  if (runtime->api == nullptr) {
    return fail<std::shared_ptr<RuntimeState>>(
        ErrorCode::model_incompatible,
        "ONNX Runtime library does not support the compiled API version");
  }
  const auto* version = runtime->api_base->GetVersionString();
  if (version == nullptr || version[0] == '\0') {
    return fail<std::shared_ptr<RuntimeState>>(
        ErrorCode::model_incompatible,
        "ONNX Runtime library did not report a version");
  }
  runtime->version = bounded_runtime_text(version);
  if (runtime->version.size() > maximum_runtime_version_bytes) {
    runtime->version.resize(maximum_runtime_version_bytes);
  }

  if (auto error = runtime_error(
          runtime->api,
          runtime->api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "codec-d9",
                                  &runtime->environment),
          ErrorCode::model_incompatible,
          "ONNX Runtime environment could not be created")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->CreateSessionOptions(&runtime->session_options),
          ErrorCode::model_incompatible,
          "ONNX Runtime session options could not be created")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->SetIntraOpNumThreads(
              runtime->session_options,
              static_cast<int>(options.intra_op_threads)),
          ErrorCode::model_incompatible,
          "ONNX Runtime intra-op thread limit was rejected")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->SetInterOpNumThreads(
              runtime->session_options,
              static_cast<int>(options.inter_op_threads)),
          ErrorCode::model_incompatible,
          "ONNX Runtime inter-op thread limit was rejected")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->SetSessionExecutionMode(runtime->session_options,
                                                ORT_SEQUENTIAL),
          ErrorCode::model_incompatible,
          "ONNX Runtime sequential execution mode was rejected")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->SetSessionGraphOptimizationLevel(
              runtime->session_options, ORT_ENABLE_ALL),
          ErrorCode::model_incompatible,
          "ONNX Runtime graph optimization mode was rejected")) {
    return *error;
  }
  if (auto error = runtime_error(
          runtime->api,
          runtime->api->CreateSessionFromArray(
              runtime->environment, bundle.onnx_model.data(),
              bundle.onnx_model.size(), runtime->session_options,
              &runtime->session),
          ErrorCode::model_incompatible,
          "ONNX model session could not be created")) {
    return *error;
  }
  return runtime;
}

#endif  // CODEC_HAS_ONNXRUNTIME

}  // namespace

bool onnx_cpu_separation_runtime_compiled() noexcept {
#ifdef CODEC_HAS_ONNXRUNTIME
  return true;
#else
  return false;
#endif
}

Result<std::unique_ptr<SeparationBackend>>
create_onnx_cpu_separation_backend(
    const VerifiedSeparationModelBundle& bundle,
    const OnnxCpuSeparationOptions& options) {
  const auto valid_options = validate_options(options);
  if (!valid_options) {
    return valid_options.error();
  }
  const auto valid_bundle = revalidate_bundle(bundle);
  if (!valid_bundle) return valid_bundle.error();

#ifndef CODEC_HAS_ONNXRUNTIME
  return fail<std::unique_ptr<SeparationBackend>>(
      ErrorCode::model_incompatible,
      "CODEC was built without ONNX Runtime CPU headers");
#else
  try {
    auto runtime = create_runtime(bundle, options);
    if (!runtime) return runtime.error();
    auto source_count = validate_session_contract(**runtime, bundle.manifest);
    if (!source_count) return source_count.error();
    std::unique_ptr<SeparationBackend> backend =
        std::make_unique<OnnxCpuBackend>(
            std::move(*runtime), bundle.manifest, bundle.model_hash,
            bundle.bundle_hash, options, *source_count);
    return backend;
  } catch (const std::bad_alloc&) {
    return fail<std::unique_ptr<SeparationBackend>>(
        ErrorCode::resource_exhausted,
        "ONNX CPU backend could not allocate bounded setup state");
  } catch (const std::exception&) {
    return fail<std::unique_ptr<SeparationBackend>>(
        ErrorCode::internal,
        "ONNX CPU backend setup failed at the profile boundary");
  } catch (...) {
    return fail<std::unique_ptr<SeparationBackend>>(
        ErrorCode::internal,
        "ONNX CPU backend setup failed at the profile boundary");
  }
#endif
}

}  // namespace codec::profiles::audio
