#include <codec/profiles/audio_model_bundle.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

constexpr std::array<std::byte, 4> amb1_magic{
    std::byte{'A'}, std::byte{'M'}, std::byte{'B'}, std::byte{'1'}};
constexpr std::uint16_t causal_flag = 0x0001U;
constexpr std::uint16_t known_flags = causal_flag;
constexpr std::uint16_t maximum_profile_channels = 64U;
constexpr std::uint16_t maximum_profile_sources = 64U;
constexpr std::uint64_t fixed_header_bytes = 50U;
constexpr std::uint64_t fixed_manifest_bytes = 20U;

enum class ValidationOrigin { caller, encoded_bundle };

ErrorCode semantic_error(ValidationOrigin origin) {
  return origin == ValidationOrigin::caller ? ErrorCode::invalid_argument
                                             : ErrorCode::model_incompatible;
}

Result<void> validate_limits(const SeparationModelBundleLimits& limits) {
  if (limits.maximum_bundle_bytes == 0 || limits.maximum_model_bytes == 0 ||
      limits.maximum_text_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "separation ModelBundle limits must be non-zero");
  }
  return {};
}

Result<void> validate_text(std::string_view text, std::string_view field,
                           const SeparationModelBundleLimits& limits,
                           ValidationOrigin origin) {
  if (text.empty()) {
    return fail(semantic_error(origin),
                std::string(field) + " must not be empty");
  }
  if (text.size() > limits.maximum_text_bytes) {
    return fail(ErrorCode::resource_exhausted,
                std::string(field) + " exceeds the configured text limit");
  }
  for (const unsigned char value : text) {
    if (value < 0x20U || value > 0x7eU) {
      return fail(semantic_error(origin),
                  std::string(field) + " must be printable ASCII");
    }
  }
  return {};
}

Result<void> validate_manifest(
    const SeparationModelManifest& manifest,
    const SeparationModelBundleLimits& limits, ValidationOrigin origin) {
  const std::array<std::pair<std::string_view, std::string_view>, 6> texts{{
      {manifest.model_id, "model_id"},
      {manifest.model_version, "model_version"},
      {manifest.license_id, "license_id"},
      {manifest.quality_domain, "quality_domain"},
      {manifest.input_tensor_name, "input_tensor_name"},
      {manifest.output_tensor_name, "output_tensor_name"},
  }};
  for (const auto& [text, field] : texts) {
    auto valid = validate_text(text, field, limits, origin);
    if (!valid) return valid.error();
  }

  const auto invalid = semantic_error(origin);
  if (manifest.input_sample_rate == 0) {
    return fail(invalid, "separation ModelBundle sample rate must be non-zero");
  }
  if (manifest.input_channels == 0 ||
      manifest.input_channels > maximum_profile_channels) {
    return fail(invalid,
                "separation ModelBundle channels must be between 1 and 64");
  }
  if (manifest.window_samples == 0 || manifest.hop_samples == 0 ||
      manifest.hop_samples > manifest.window_samples ||
      manifest.lookahead_samples > manifest.window_samples) {
    return fail(invalid, "separation ModelBundle framing is invalid");
  }
  if (manifest.causal && manifest.lookahead_samples != 0) {
    return fail(invalid,
                "causal separation ModelBundle must have zero lookahead");
  }
  if (manifest.maximum_sources == 0 ||
      manifest.maximum_sources > maximum_profile_sources) {
    return fail(
        invalid,
        "separation ModelBundle maximum sources must be between 1 and 64");
  }
  if (manifest.input_tensor_name == manifest.output_tensor_name) {
    return fail(invalid,
                "separation ModelBundle tensor names must be distinct");
  }
  return {};
}

Result<void> validate_model_size(
    std::uint64_t model_bytes, const SeparationModelBundleLimits& limits,
    ValidationOrigin origin) {
  if (model_bytes == 0) {
    return fail(semantic_error(origin),
                "separation ModelBundle model bytes must not be empty");
  }
  if (model_bytes > limits.maximum_model_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "separation ModelBundle model exceeds the configured limit");
  }
  return {};
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t* output) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
  *output = left + right;
  return true;
}

Result<std::uint64_t> manifest_size(
    const SeparationModelManifest& manifest) {
  std::uint64_t size = fixed_manifest_bytes;
  const std::array<std::string_view, 6> texts{
      manifest.model_id,         manifest.model_version,
      manifest.license_id,       manifest.quality_domain,
      manifest.input_tensor_name, manifest.output_tensor_name,
  };
  for (const auto text : texts) {
    std::uint64_t next{};
    if (!checked_add(size, 2U, &next) ||
        !checked_add(next, static_cast<std::uint64_t>(text.size()), &size)) {
      return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                                 "separation ModelBundle manifest is too large");
    }
  }
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "separation ModelBundle manifest is too large");
  }
  return size;
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::byte>(value & 0xffU));
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  for (unsigned index = 0; index < 4U; ++index) {
    const auto shift = (3U - index) * 8U;
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
  for (unsigned index = 0; index < 8U; ++index) {
    const auto shift = (7U - index) * 8U;
    output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
  }
}

void append_text(std::vector<std::byte>& output, std::string_view text) {
  append_u16(output, static_cast<std::uint16_t>(text.size()));
  for (const unsigned char value : text) {
    output.push_back(static_cast<std::byte>(value));
  }
}

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  bool read_u16(std::uint16_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(2U, &bytes)) return false;
    *value = static_cast<std::uint16_t>(
        (std::to_integer<std::uint16_t>(bytes[0]) << 8U) |
        std::to_integer<std::uint16_t>(bytes[1]));
    return true;
  }

  bool read_u32(std::uint32_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(4U, &bytes)) return false;
    std::uint32_t decoded{};
    for (const auto byte : bytes) {
      decoded = (decoded << 8U) | std::to_integer<std::uint32_t>(byte);
    }
    *value = decoded;
    return true;
  }

  bool read_u64(std::uint64_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(8U, &bytes)) return false;
    std::uint64_t decoded{};
    for (const auto byte : bytes) {
      decoded = (decoded << 8U) | std::to_integer<std::uint64_t>(byte);
    }
    *value = decoded;
    return true;
  }

  bool read_bytes(std::size_t count, std::span<const std::byte>* value) {
    if (count > bytes_.size() - offset_) return false;
    *value = bytes_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

  bool empty() const noexcept { return offset_ == bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

Result<void> read_text(Reader& reader, std::string* value,
                       const SeparationModelBundleLimits& limits) {
  std::uint16_t length{};
  if (!reader.read_u16(&length)) {
    return fail(ErrorCode::model_incompatible,
                "separation ModelBundle text length is truncated");
  }
  if (length > limits.maximum_text_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "separation ModelBundle text exceeds the configured limit");
  }
  std::span<const std::byte> bytes;
  if (!reader.read_bytes(length, &bytes)) {
    return fail(ErrorCode::model_incompatible,
                "separation ModelBundle text is truncated");
  }
  value->clear();
  value->reserve(bytes.size());
  for (const auto byte : bytes) {
    value->push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
  }
  return {};
}

Result<SeparationModelManifest> decode_manifest(
    std::span<const std::byte> encoded,
    const SeparationModelBundleLimits& limits) {
  Reader reader(encoded);
  SeparationModelManifest manifest;
  if (!reader.read_u32(&manifest.input_sample_rate) ||
      !reader.read_u16(&manifest.input_channels) ||
      !reader.read_u16(&manifest.maximum_sources) ||
      !reader.read_u32(&manifest.window_samples) ||
      !reader.read_u32(&manifest.hop_samples) ||
      !reader.read_u32(&manifest.lookahead_samples)) {
    return fail<SeparationModelManifest>(
        ErrorCode::model_incompatible,
        "separation ModelBundle manifest header is truncated");
  }
  const std::array<std::string*, 6> fields{
      &manifest.model_id,          &manifest.model_version,
      &manifest.license_id,        &manifest.quality_domain,
      &manifest.input_tensor_name, &manifest.output_tensor_name,
  };
  for (auto* field : fields) {
    auto read = read_text(reader, field, limits);
    if (!read) return read.error();
  }
  if (!reader.empty()) {
    return fail<SeparationModelManifest>(
        ErrorCode::model_incompatible,
        "separation ModelBundle manifest has trailing bytes");
  }
  return manifest;
}

bool equal_hash(const Sha256& left, const Sha256& right) {
  std::uint8_t difference{};
  for (std::size_t index = 0; index < left.size(); ++index) {
    difference = static_cast<std::uint8_t>(difference |
                                           (left[index] ^ right[index]));
  }
  return difference == 0;
}

}  // namespace

Result<std::vector<std::byte>> encode_separation_model_bundle(
    const SeparationModelBundle& bundle,
    const SeparationModelBundleLimits& limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto valid_manifest =
      validate_manifest(bundle.manifest, limits, ValidationOrigin::caller);
  if (!valid_manifest) return valid_manifest.error();

  const auto model_bytes =
      static_cast<std::uint64_t>(bundle.onnx_model.size());
  auto valid_model =
      validate_model_size(model_bytes, limits, ValidationOrigin::caller);
  if (!valid_model) return valid_model.error();

  auto encoded_manifest_size = manifest_size(bundle.manifest);
  if (!encoded_manifest_size) return encoded_manifest_size.error();

  std::uint64_t total{};
  if (!checked_add(fixed_header_bytes, *encoded_manifest_size, &total) ||
      !checked_add(total, model_bytes, &total) ||
      total > limits.maximum_bundle_bytes ||
      total > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "separation ModelBundle exceeds the configured bundle limit");
  }

  const auto model_hash = sha256(bundle.onnx_model);
  std::vector<std::byte> encoded;
  encoded.reserve(static_cast<std::size_t>(total));
  encoded.insert(encoded.end(), amb1_magic.begin(), amb1_magic.end());
  append_u16(encoded, bundle.manifest.causal ? causal_flag : 0U);
  append_u32(encoded, static_cast<std::uint32_t>(*encoded_manifest_size));
  append_u64(encoded, model_bytes);
  for (const auto value : model_hash) {
    encoded.push_back(static_cast<std::byte>(value));
  }
  append_u32(encoded, bundle.manifest.input_sample_rate);
  append_u16(encoded, bundle.manifest.input_channels);
  append_u16(encoded, bundle.manifest.maximum_sources);
  append_u32(encoded, bundle.manifest.window_samples);
  append_u32(encoded, bundle.manifest.hop_samples);
  append_u32(encoded, bundle.manifest.lookahead_samples);
  append_text(encoded, bundle.manifest.model_id);
  append_text(encoded, bundle.manifest.model_version);
  append_text(encoded, bundle.manifest.license_id);
  append_text(encoded, bundle.manifest.quality_domain);
  append_text(encoded, bundle.manifest.input_tensor_name);
  append_text(encoded, bundle.manifest.output_tensor_name);
  encoded.insert(encoded.end(), bundle.onnx_model.begin(),
                 bundle.onnx_model.end());

  if (encoded.size() != static_cast<std::size_t>(total)) {
    return fail<std::vector<std::byte>>(
        ErrorCode::internal,
        "separation ModelBundle encoder produced an inconsistent size");
  }
  return encoded;
}

Result<VerifiedSeparationModelBundle> decode_separation_model_bundle(
    std::span<const std::byte> encoded,
    const SeparationModelBundleLimits& limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  if (encoded.size() > limits.maximum_bundle_bytes) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::resource_exhausted,
        "encoded separation ModelBundle exceeds the configured bundle limit");
  }
  if (encoded.size() < fixed_header_bytes) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle header is truncated");
  }

  Reader header(encoded.first(static_cast<std::size_t>(fixed_header_bytes)));
  std::span<const std::byte> magic;
  if (!header.read_bytes(amb1_magic.size(), &magic) ||
      !std::equal(magic.begin(), magic.end(), amb1_magic.begin())) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle magic is incompatible");
  }
  std::uint16_t flags{};
  std::uint32_t manifest_bytes{};
  std::uint64_t model_bytes{};
  std::span<const std::byte> stored_hash_bytes;
  if (!header.read_u16(&flags) || !header.read_u32(&manifest_bytes) ||
      !header.read_u64(&model_bytes) ||
      !header.read_bytes(Sha256{}.size(), &stored_hash_bytes) ||
      !header.empty()) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle header is malformed");
  }
  if ((flags & static_cast<std::uint16_t>(~known_flags)) != 0U) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle flags are incompatible");
  }
  auto valid_model = validate_model_size(
      model_bytes, limits, ValidationOrigin::encoded_bundle);
  if (!valid_model) return valid_model.error();

  std::uint64_t expected_size{};
  if (!checked_add(fixed_header_bytes, manifest_bytes, &expected_size) ||
      !checked_add(expected_size, model_bytes, &expected_size) ||
      expected_size != encoded.size()) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle lengths are noncanonical");
  }

  const auto manifest_span = encoded.subspan(
      static_cast<std::size_t>(fixed_header_bytes), manifest_bytes);
  auto manifest = decode_manifest(manifest_span, limits);
  if (!manifest) return manifest.error();
  manifest->causal = (flags & causal_flag) != 0U;
  auto valid_manifest =
      validate_manifest(*manifest, limits, ValidationOrigin::encoded_bundle);
  if (!valid_manifest) return valid_manifest.error();

  const auto model_span = encoded.last(static_cast<std::size_t>(model_bytes));
  Sha256 stored_hash{};
  for (std::size_t index = 0; index < stored_hash.size(); ++index) {
    stored_hash[index] =
        std::to_integer<std::uint8_t>(stored_hash_bytes[index]);
  }
  const auto actual_hash = sha256(model_span);
  if (!equal_hash(stored_hash, actual_hash)) {
    return fail<VerifiedSeparationModelBundle>(
        ErrorCode::model_incompatible,
        "encoded separation ModelBundle model hash does not match");
  }

  return VerifiedSeparationModelBundle{
      .manifest = std::move(*manifest),
      .onnx_model = std::vector<std::byte>(model_span.begin(), model_span.end()),
      .model_hash = actual_hash,
      .bundle_hash = sha256(encoded),
  };
}

}  // namespace codec::profiles::audio
