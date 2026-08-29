#include <codec/distributed_wire.hpp>

#include <codec/integrity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace codec {
namespace {

constexpr std::size_t envelope_header_size = 56;
constexpr std::size_t digest_offset = 24;
constexpr std::size_t encoded_record_fixed_bytes = 92;
constexpr std::size_t encoded_output_fixed_bytes = 80;
constexpr std::uint32_t process_has_implementation_hash = 1U << 0U;
constexpr std::uint32_t process_has_configuration_hash = 1U << 1U;
constexpr std::uint32_t known_process_flags =
    process_has_implementation_hash | process_has_configuration_hash;
constexpr std::array<std::byte, 4> request_magic{
    std::byte{'D'}, std::byte{'R'}, std::byte{'Q'}, std::byte{'1'}};
constexpr std::array<std::byte, 4> reply_magic{
    std::byte{'D'}, std::byte{'R'}, std::byte{'S'}, std::byte{'1'}};

template <typename Integer>
void put_le(std::span<std::byte> output, std::size_t offset, Integer value) {
  using Unsigned = std::make_unsigned_t<Integer>;
  auto bits = static_cast<Unsigned>(value);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    output[offset + index] = static_cast<std::byte>(bits & 0xffU);
    bits >>= 8U;
  }
}

template <typename Integer>
Integer get_le(std::span<const std::byte> input, std::size_t offset) {
  using Unsigned = std::make_unsigned_t<Integer>;
  Unsigned bits = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    bits |= static_cast<Unsigned>(
                std::to_integer<std::uint8_t>(input[offset + index]))
            << (index * 8U);
  }
  return static_cast<Integer>(bits);
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t* output) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) return false;
  *output = left + right;
  return true;
}

bool checked_add_to(std::uint64_t* total, std::uint64_t value) {
  std::uint64_t next{};
  if (!checked_add(*total, value, &next)) return false;
  *total = next;
  return true;
}

Result<void> validate_limits(const DistributedRemoteWireLimits& limits) {
  if (limits.maximum_envelope_bytes == 0 ||
      limits.maximum_input_records == 0 || limits.maximum_input_bytes == 0 ||
      limits.maximum_outputs == 0 || limits.maximum_output_bytes == 0 ||
      limits.maximum_label_bytes == 0 ||
      limits.maximum_process_text_bytes == 0 ||
      limits.maximum_process_details_bytes == 0 ||
      limits.maximum_error_message_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed remote wire limits must be non-zero");
  }
  return {};
}

Result<void> validate_label(std::string_view value,
                            const DistributedRemoteWireLimits& limits) {
  if (value.empty()) {
    return fail(ErrorCode::invalid_argument,
                "distributed remote wire label must be non-empty");
  }
  if (value.size() > limits.maximum_label_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "distributed remote wire label exceeds configured limit");
  }
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    return fail(ErrorCode::resource_exhausted,
                "distributed remote wire label exceeds encoding capacity");
  }
  return {};
}

bool valid_truth(TruthClass truth) {
  return truth == TruthClass::source_exact || truth == TruthClass::state_exact ||
         truth == TruthClass::derived;
}

void append_u8(std::vector<std::byte>& output, std::uint8_t value) {
  output.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& output, std::uint16_t value) {
  const auto start = output.size();
  output.resize(start + sizeof(value));
  put_le<std::uint16_t>(output, start, value);
}

void append_u32(std::vector<std::byte>& output, std::uint32_t value) {
  const auto start = output.size();
  output.resize(start + sizeof(value));
  put_le<std::uint32_t>(output, start, value);
}

void append_u64(std::vector<std::byte>& output, std::uint64_t value) {
  const auto start = output.size();
  output.resize(start + sizeof(value));
  put_le<std::uint64_t>(output, start, value);
}

void append_i64(std::vector<std::byte>& output, std::int64_t value) {
  const auto start = output.size();
  output.resize(start + sizeof(value));
  put_le<std::int64_t>(output, start, value);
}

void append_stream(std::vector<std::byte>& output, const StreamId& stream) {
  for (const auto value : stream.bytes) {
    output.push_back(static_cast<std::byte>(value));
  }
}

void append_hash(std::vector<std::byte>& output, const Sha256& hash) {
  for (const auto value : hash) output.push_back(static_cast<std::byte>(value));
}

void append_string(std::vector<std::byte>& output, std::string_view value) {
  append_u32(output, static_cast<std::uint32_t>(value.size()));
  const auto raw = std::as_bytes(std::span{value.data(), value.size()});
  output.insert(output.end(), raw.begin(), raw.end());
}

Result<Sha256> envelope_digest(std::span<const std::byte> prefix,
                               std::span<const std::byte> body) {
  try {
    std::vector<std::byte> input;
    input.reserve(prefix.size() + body.size());
    input.insert(input.end(), prefix.begin(), prefix.end());
    input.insert(input.end(), body.begin(), body.end());
    return sha256(input);
  } catch (const std::bad_alloc&) {
    return fail<Sha256>(ErrorCode::resource_exhausted,
                        "distributed remote wire digest allocation failed");
  }
}

Result<std::vector<std::byte>> finalize_envelope(
    const std::array<std::byte, 4>& magic,
    std::span<const std::byte> body,
    const DistributedRemoteWireLimits& limits) {
  std::uint64_t total = envelope_header_size;
  if (!checked_add_to(&total, static_cast<std::uint64_t>(body.size())) ||
      total > limits.maximum_envelope_bytes ||
      total > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire envelope exceeds configured limit");
  }

  try {
    std::vector<std::byte> output(static_cast<std::size_t>(total));
    std::copy(magic.begin(), magic.end(), output.begin());
    put_le<std::uint16_t>(output, 4, distributed_remote_wire_version);
    put_le<std::uint16_t>(output, 6, 0);
    put_le<std::uint64_t>(output, 8, total);
    put_le<std::uint64_t>(output, 16, static_cast<std::uint64_t>(body.size()));
    std::copy(body.begin(), body.end(),
              output.begin() + static_cast<std::ptrdiff_t>(envelope_header_size));
    auto digest = envelope_digest(
        std::span<const std::byte>{output}.first(digest_offset),
        std::span<const std::byte>{output}.subspan(envelope_header_size));
    if (!digest) return digest.error();
    for (std::size_t index = 0; index < digest->size(); ++index) {
      output[digest_offset + index] = static_cast<std::byte>((*digest)[index]);
    }
    return output;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire envelope allocation failed");
  }
}

bool digest_matches(std::span<const std::byte> encoded, const Sha256& digest) {
  for (std::size_t index = 0; index < digest.size(); ++index) {
    if (std::to_integer<std::uint8_t>(encoded[digest_offset + index]) !=
        digest[index]) {
      return false;
    }
  }
  return true;
}

Result<std::span<const std::byte>> validate_envelope(
    std::span<const std::byte> encoded,
    const std::array<std::byte, 4>& magic,
    const DistributedRemoteWireLimits& limits) {
  if (encoded.size() > limits.maximum_envelope_bytes) {
    return fail<std::span<const std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire envelope exceeds configured limit");
  }
  if (encoded.size() < envelope_header_size) {
    return fail<std::span<const std::byte>>(
        ErrorCode::protocol, "distributed remote wire header is truncated");
  }
  if (!std::equal(magic.begin(), magic.end(), encoded.begin()) ||
      get_le<std::uint16_t>(encoded, 4) != distributed_remote_wire_version ||
      get_le<std::uint16_t>(encoded, 6) != 0) {
    return fail<std::span<const std::byte>>(
        ErrorCode::protocol,
        "distributed remote wire header is incompatible");
  }
  const auto total = get_le<std::uint64_t>(encoded, 8);
  const auto body_size = get_le<std::uint64_t>(encoded, 16);
  if (total != encoded.size() || total < envelope_header_size ||
      body_size != total - envelope_header_size) {
    return fail<std::span<const std::byte>>(
        ErrorCode::protocol,
        "distributed remote wire envelope lengths are inconsistent");
  }
  const auto body = encoded.subspan(envelope_header_size);
  auto digest = envelope_digest(encoded.first(digest_offset), body);
  if (!digest) return digest.error();
  if (!digest_matches(encoded, *digest)) {
    return fail<std::span<const std::byte>>(
        ErrorCode::protocol,
        "distributed remote wire envelope SHA-256 mismatch");
  }
  return body;
}

class Reader {
 public:
  explicit Reader(std::span<const std::byte> input) : input_(input) {}

  bool read_u8(std::uint8_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(1, &bytes)) return false;
    *value = std::to_integer<std::uint8_t>(bytes[0]);
    return true;
  }

  bool read_u16(std::uint16_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(2, &bytes)) return false;
    *value = get_le<std::uint16_t>(bytes, 0);
    return true;
  }

  bool read_u32(std::uint32_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(4, &bytes)) return false;
    *value = get_le<std::uint32_t>(bytes, 0);
    return true;
  }

  bool read_u64(std::uint64_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(8, &bytes)) return false;
    *value = get_le<std::uint64_t>(bytes, 0);
    return true;
  }

  bool read_i64(std::int64_t* value) {
    std::span<const std::byte> bytes;
    if (!read_bytes(8, &bytes)) return false;
    *value = get_le<std::int64_t>(bytes, 0);
    return true;
  }

  bool read_bytes(std::size_t count, std::span<const std::byte>* value) {
    if (count > input_.size() - offset_) return false;
    *value = input_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

  bool empty() const noexcept { return offset_ == input_.size(); }

 private:
  std::span<const std::byte> input_;
  std::size_t offset_{};
};

Result<std::string> read_string(Reader& reader, std::size_t maximum_bytes,
                                bool require_nonempty) {
  std::uint32_t length{};
  if (!reader.read_u32(&length)) {
    return fail<std::string>(ErrorCode::protocol,
                             "distributed remote wire string length is truncated");
  }
  if (length > maximum_bytes) {
    return fail<std::string>(
        ErrorCode::resource_exhausted,
        "distributed remote wire string exceeds configured limit");
  }
  if (require_nonempty && length == 0) {
    return fail<std::string>(ErrorCode::protocol,
                             "distributed remote wire label is empty");
  }
  std::span<const std::byte> raw;
  if (!reader.read_bytes(length, &raw)) {
    return fail<std::string>(ErrorCode::protocol,
                             "distributed remote wire string is truncated");
  }
  try {
    return std::string{reinterpret_cast<const char*>(raw.data()), raw.size()};
  } catch (const std::bad_alloc&) {
    return fail<std::string>(ErrorCode::resource_exhausted,
                             "distributed remote wire string allocation failed");
  }
}

Result<std::uint16_t> error_code_to_wire(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument: return std::uint16_t{1};
    case ErrorCode::unauthorized_source: return std::uint16_t{2};
    case ErrorCode::network: return std::uint16_t{3};
    case ErrorCode::protocol: return std::uint16_t{4};
    case ErrorCode::decode: return std::uint16_t{5};
    case ErrorCode::archive_io: return std::uint16_t{6};
    case ErrorCode::archive_corrupt: return std::uint16_t{7};
    case ErrorCode::model_incompatible: return std::uint16_t{8};
    case ErrorCode::inference: return std::uint16_t{9};
    case ErrorCode::watermark_model_missing: return std::uint16_t{10};
    case ErrorCode::watermark_code_ambiguous: return std::uint16_t{11};
    case ErrorCode::watermark_signature_invalid: return std::uint16_t{12};
    case ErrorCode::watermark_replay_suspected: return std::uint16_t{13};
    case ErrorCode::watermark_path_unqualified: return std::uint16_t{14};
    case ErrorCode::identity_not_enrolled: return std::uint16_t{15};
    case ErrorCode::identity_uncalibrated: return std::uint16_t{16};
    case ErrorCode::cancelled: return std::uint16_t{17};
    case ErrorCode::resource_exhausted: return std::uint16_t{18};
    case ErrorCode::internal: return std::uint16_t{19};
    case ErrorCode::ok:
      return fail<std::uint16_t>(ErrorCode::invalid_argument,
                                 "ok is not a remote error reply");
  }
  return fail<std::uint16_t>(ErrorCode::invalid_argument,
                             "unknown remote error code");
}

Result<ErrorCode> error_code_from_wire(std::uint16_t code) {
  switch (code) {
    case 1: return ErrorCode::invalid_argument;
    case 2: return ErrorCode::unauthorized_source;
    case 3: return ErrorCode::network;
    case 4: return ErrorCode::protocol;
    case 5: return ErrorCode::decode;
    case 6: return ErrorCode::archive_io;
    case 7: return ErrorCode::archive_corrupt;
    case 8: return ErrorCode::model_incompatible;
    case 9: return ErrorCode::inference;
    case 10: return ErrorCode::watermark_model_missing;
    case 11: return ErrorCode::watermark_code_ambiguous;
    case 12: return ErrorCode::watermark_signature_invalid;
    case 13: return ErrorCode::watermark_replay_suspected;
    case 14: return ErrorCode::watermark_path_unqualified;
    case 15: return ErrorCode::identity_not_enrolled;
    case 16: return ErrorCode::identity_uncalibrated;
    case 17: return ErrorCode::cancelled;
    case 18: return ErrorCode::resource_exhausted;
    case 19: return ErrorCode::internal;
    default:
      return fail<ErrorCode>(ErrorCode::protocol,
                             "unknown distributed remote wire error code");
  }
}

Result<void> validate_process_bounds(
    const ProvenanceProcess& process,
    const DistributedRemoteWireLimits& limits) {
  const std::array<std::string_view, 4> texts{
      process.operation, process.implementation_id,
      process.implementation_version, process.details_type};
  for (const auto text : texts) {
    if (text.size() > limits.maximum_process_text_bytes ||
        text.size() > std::numeric_limits<std::uint32_t>::max()) {
      return fail(ErrorCode::resource_exhausted,
                  "distributed remote wire process text exceeds configured limit");
    }
  }
  if (process.details.size() > limits.maximum_process_details_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "distributed remote wire process details exceed configured limit");
  }
  return {};
}

Result<std::uint64_t> encoded_process_variable_size(
    const ProvenanceProcess& process) {
  std::uint64_t total = 0;
  if (process.implementation_hash) total += Sha256{}.size();
  if (process.configuration_hash) total += Sha256{}.size();
  const std::array<std::string_view, 4> texts{
      process.operation, process.implementation_id,
      process.implementation_version, process.details_type};
  for (const auto text : texts) {
    if (!checked_add_to(&total, static_cast<std::uint64_t>(text.size()))) {
      return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                                 "distributed remote wire process is too large");
    }
  }
  if (!checked_add_to(&total,
                      static_cast<std::uint64_t>(process.details.size()))) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "distributed remote wire process is too large");
  }
  return total;
}

void append_process_and_payload(std::vector<std::byte>& body,
                                const ProcessorOutput& output) {
  append_stream(body, output.stream);
  append_u16(body, output.type);
  append_u8(body, static_cast<std::uint8_t>(output.truth));
  append_u8(body, 0);
  append_i64(body, output.start_ns);
  append_i64(body, output.end_ns);
  append_u64(body, static_cast<std::uint64_t>(output.payload.size()));
  const auto flags =
      (output.process.implementation_hash ? process_has_implementation_hash : 0U) |
      (output.process.configuration_hash ? process_has_configuration_hash : 0U);
  append_u32(body, flags);
  append_i64(body, output.process.created_utc_ns);
  append_u32(body, static_cast<std::uint32_t>(output.process.operation.size()));
  append_u32(body,
             static_cast<std::uint32_t>(output.process.implementation_id.size()));
  append_u32(body, static_cast<std::uint32_t>(
                       output.process.implementation_version.size()));
  append_u32(body,
             static_cast<std::uint32_t>(output.process.details_type.size()));
  append_u64(body, static_cast<std::uint64_t>(output.process.details.size()));
  if (output.process.implementation_hash) {
    append_hash(body, *output.process.implementation_hash);
  }
  if (output.process.configuration_hash) {
    append_hash(body, *output.process.configuration_hash);
  }
  const std::array<std::string_view, 4> texts{
      output.process.operation, output.process.implementation_id,
      output.process.implementation_version, output.process.details_type};
  for (const auto text : texts) {
    const auto raw = std::as_bytes(std::span{text.data(), text.size()});
    body.insert(body.end(), raw.begin(), raw.end());
  }
  body.insert(body.end(), output.process.details.begin(),
              output.process.details.end());
  body.insert(body.end(), output.payload.begin(), output.payload.end());
}

Result<ProcessorOutput> decode_output(Reader& reader,
                                      DistributedRemoteWireLimits limits,
                                      std::uint64_t* aggregate_output_bytes) {
  std::span<const std::byte> stream_bytes;
  std::uint16_t type{};
  std::uint8_t truth_byte{};
  std::uint8_t reserved{};
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  std::uint64_t payload_size{};
  std::uint32_t flags{};
  std::int64_t created_utc_ns{};
  std::array<std::uint32_t, 4> text_lengths{};
  std::uint64_t details_size{};
  if (!reader.read_bytes(16, &stream_bytes) || !reader.read_u16(&type) ||
      !reader.read_u8(&truth_byte) || !reader.read_u8(&reserved) ||
      !reader.read_i64(&start_ns) || !reader.read_i64(&end_ns) ||
      !reader.read_u64(&payload_size) || !reader.read_u32(&flags) ||
      !reader.read_i64(&created_utc_ns)) {
    return fail<ProcessorOutput>(ErrorCode::protocol,
                                 "distributed remote wire output is truncated");
  }
  for (auto& length : text_lengths) {
    if (!reader.read_u32(&length)) {
      return fail<ProcessorOutput>(ErrorCode::protocol,
                                   "distributed remote wire process lengths are truncated");
    }
  }
  if (!reader.read_u64(&details_size)) {
    return fail<ProcessorOutput>(ErrorCode::protocol,
                                 "distributed remote wire process details length is truncated");
  }
  if (reserved != 0 || (flags & ~known_process_flags) != 0U) {
    return fail<ProcessorOutput>(ErrorCode::protocol,
                                 "distributed remote wire output flags are invalid");
  }
  const auto truth = static_cast<TruthClass>(truth_byte);
  if (!valid_truth(truth)) {
    return fail<ProcessorOutput>(ErrorCode::protocol,
                                 "distributed remote wire truth class is invalid");
  }
  for (const auto length : text_lengths) {
    if (length > limits.maximum_process_text_bytes) {
      return fail<ProcessorOutput>(ErrorCode::resource_exhausted,
                                   "distributed remote wire process text exceeds configured limit");
    }
  }
  if (details_size > limits.maximum_process_details_bytes) {
    return fail<ProcessorOutput>(ErrorCode::resource_exhausted,
                                 "distributed remote wire process details exceed configured limit");
  }
  if (payload_size > limits.maximum_output_bytes - *aggregate_output_bytes ||
      payload_size > std::numeric_limits<std::size_t>::max()) {
    return fail<ProcessorOutput>(ErrorCode::resource_exhausted,
                                 "distributed remote wire output payload exceeds configured limit");
  }

  ProcessorOutput output;
  for (std::size_t index = 0; index < output.stream.bytes.size(); ++index) {
    output.stream.bytes[index] =
        std::to_integer<std::uint8_t>(stream_bytes[index]);
  }
  output.type = type;
  output.truth = truth;
  output.start_ns = start_ns;
  output.end_ns = end_ns;
  output.process.created_utc_ns = created_utc_ns;

  std::span<const std::byte> hash_bytes;
  if ((flags & process_has_implementation_hash) != 0U) {
    if (!reader.read_bytes(Sha256{}.size(), &hash_bytes)) {
      return fail<ProcessorOutput>(ErrorCode::protocol,
                                   "distributed remote wire implementation hash is truncated");
    }
    Sha256 hash{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
      hash[index] = std::to_integer<std::uint8_t>(hash_bytes[index]);
    }
    output.process.implementation_hash = hash;
  }
  if ((flags & process_has_configuration_hash) != 0U) {
    if (!reader.read_bytes(Sha256{}.size(), &hash_bytes)) {
      return fail<ProcessorOutput>(ErrorCode::protocol,
                                   "distributed remote wire configuration hash is truncated");
    }
    Sha256 hash{};
    for (std::size_t index = 0; index < hash.size(); ++index) {
      hash[index] = std::to_integer<std::uint8_t>(hash_bytes[index]);
    }
    output.process.configuration_hash = hash;
  }

  std::array<std::string*, 4> texts{
      &output.process.operation, &output.process.implementation_id,
      &output.process.implementation_version, &output.process.details_type};
  try {
    for (std::size_t index = 0; index < texts.size(); ++index) {
      std::span<const std::byte> raw;
      if (!reader.read_bytes(text_lengths[index], &raw)) {
        return fail<ProcessorOutput>(ErrorCode::protocol,
                                     "distributed remote wire process text is truncated");
      }
      texts[index]->assign(reinterpret_cast<const char*>(raw.data()), raw.size());
    }
    std::span<const std::byte> details;
    if (details_size > std::numeric_limits<std::size_t>::max() ||
        !reader.read_bytes(static_cast<std::size_t>(details_size), &details)) {
      return fail<ProcessorOutput>(ErrorCode::protocol,
                                   "distributed remote wire process details are truncated");
    }
    output.process.details.assign(details.begin(), details.end());
    std::span<const std::byte> payload;
    if (!reader.read_bytes(static_cast<std::size_t>(payload_size), &payload)) {
      return fail<ProcessorOutput>(ErrorCode::protocol,
                                   "distributed remote wire output payload is truncated");
    }
    output.payload.assign(payload.begin(), payload.end());
  } catch (const std::bad_alloc&) {
    return fail<ProcessorOutput>(ErrorCode::resource_exhausted,
                                 "distributed remote wire output allocation failed");
  }
  *aggregate_output_bytes += payload_size;
  return output;
}

}  // namespace

Result<std::vector<std::byte>> encode_distributed_remote_request(
    const DistributedRemoteExecutionRequest& request,
    DistributedRemoteWireLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto worker_label = validate_label(request.worker_name, limits);
  if (!worker_label) return worker_label.error();
  auto processor_label = validate_label(request.processor_name, limits);
  if (!processor_label) return processor_label.error();
  if (request.inputs.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "distributed remote wire request requires at least one input");
  }
  if (request.inputs.size() > limits.maximum_input_records ||
      request.inputs.size() > std::numeric_limits<std::uint32_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire input count exceeds configured limit");
  }

  std::uint64_t aggregate_input_bytes = 0;
  std::uint64_t body_size = 12;
  if (!checked_add_to(&body_size,
                      static_cast<std::uint64_t>(request.worker_name.size())) ||
      !checked_add_to(&body_size,
                      static_cast<std::uint64_t>(request.processor_name.size()))) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire request is too large");
  }
  for (const auto& input : request.inputs) {
    if (input.record.payload_size != input.payload.size()) {
      return fail<std::vector<std::byte>>(
          ErrorCode::invalid_argument,
          "distributed remote wire input payload size is inconsistent");
    }
    const auto payload_size = static_cast<std::uint64_t>(input.payload.size());
    if (payload_size > limits.maximum_input_bytes - aggregate_input_bytes) {
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "distributed remote wire input bytes exceed configured limit");
    }
    aggregate_input_bytes += payload_size;
    if (!checked_add_to(&body_size, encoded_record_fixed_bytes) ||
        !checked_add_to(&body_size, payload_size)) {
      return fail<std::vector<std::byte>>(
          ErrorCode::resource_exhausted,
          "distributed remote wire request is too large");
    }
  }
  std::uint64_t envelope_size = envelope_header_size;
  if (!checked_add_to(&envelope_size, body_size) ||
      envelope_size > limits.maximum_envelope_bytes ||
      body_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire request exceeds configured envelope limit");
  }

  try {
    std::vector<std::byte> body;
    body.reserve(static_cast<std::size_t>(body_size));
    append_string(body, request.worker_name);
    append_string(body, request.processor_name);
    append_u32(body, static_cast<std::uint32_t>(request.inputs.size()));
    for (const auto& input : request.inputs) {
      append_u16(body, input.record.type_code());
      append_u16(body, 0);
      append_u64(body, input.record.sequence);
      append_stream(body, input.record.stream);
      append_i64(body, input.record.start_ns);
      append_i64(body, input.record.end_ns);
      append_u64(body, input.record.payload_size);
      append_u64(body, input.record.file_offset);
      append_hash(body, input.record.hash);
      body.insert(body.end(), input.payload.begin(), input.payload.end());
    }
    if (body.size() != body_size) {
      return fail<std::vector<std::byte>>(
          ErrorCode::internal,
          "distributed remote wire request encoder size mismatch");
    }
    return finalize_envelope(request_magic, body, limits);
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire request allocation failed");
  }
}

Result<DistributedRemoteExecutionRequest> decode_distributed_remote_request(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto body = validate_envelope(encoded, request_magic, limits);
  if (!body) return body.error();

  Reader reader{*body};
  auto worker_name = read_string(reader, limits.maximum_label_bytes, true);
  if (!worker_name) return worker_name.error();
  auto processor_name = read_string(reader, limits.maximum_label_bytes, true);
  if (!processor_name) return processor_name.error();
  std::uint32_t count{};
  if (!reader.read_u32(&count)) {
    return fail<DistributedRemoteExecutionRequest>(
        ErrorCode::protocol,
        "distributed remote wire input count is truncated");
  }
  if (count == 0) {
    return fail<DistributedRemoteExecutionRequest>(
        ErrorCode::protocol,
        "distributed remote wire request has no inputs");
  }
  if (count > limits.maximum_input_records) {
    return fail<DistributedRemoteExecutionRequest>(
        ErrorCode::resource_exhausted,
        "distributed remote wire input count exceeds configured limit");
  }

  DistributedRemoteExecutionRequest request;
  request.worker_name = std::move(*worker_name);
  request.processor_name = std::move(*processor_name);
  std::uint64_t aggregate_input_bytes = 0;
  try {
    request.inputs.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
      std::uint16_t type{};
      std::uint16_t reserved{};
      std::uint64_t sequence{};
      std::span<const std::byte> stream_bytes;
      std::int64_t start_ns{};
      std::int64_t end_ns{};
      std::uint64_t payload_size{};
      std::uint64_t file_offset{};
      std::span<const std::byte> hash_bytes;
      if (!reader.read_u16(&type) || !reader.read_u16(&reserved) ||
          !reader.read_u64(&sequence) ||
          !reader.read_bytes(16, &stream_bytes) ||
          !reader.read_i64(&start_ns) || !reader.read_i64(&end_ns) ||
          !reader.read_u64(&payload_size) ||
          !reader.read_u64(&file_offset) ||
          !reader.read_bytes(Sha256{}.size(), &hash_bytes)) {
        return fail<DistributedRemoteExecutionRequest>(
            ErrorCode::protocol,
            "distributed remote wire input record is truncated");
      }
      if (reserved != 0) {
        return fail<DistributedRemoteExecutionRequest>(
            ErrorCode::protocol,
            "distributed remote wire input reserved field is non-zero");
      }
      if (payload_size > limits.maximum_input_bytes - aggregate_input_bytes ||
          payload_size > std::numeric_limits<std::size_t>::max()) {
        return fail<DistributedRemoteExecutionRequest>(
            ErrorCode::resource_exhausted,
            "distributed remote wire input bytes exceed configured limit");
      }
      std::span<const std::byte> payload;
      if (!reader.read_bytes(static_cast<std::size_t>(payload_size), &payload)) {
        return fail<DistributedRemoteExecutionRequest>(
            ErrorCode::protocol,
            "distributed remote wire input payload is truncated");
      }

      ExtractedRecord record;
      record.record.type = static_cast<RecordType>(type);
      record.record.sequence = sequence;
      for (std::size_t stream_index = 0;
           stream_index < record.record.stream.bytes.size(); ++stream_index) {
        record.record.stream.bytes[stream_index] =
            std::to_integer<std::uint8_t>(stream_bytes[stream_index]);
      }
      record.record.start_ns = start_ns;
      record.record.end_ns = end_ns;
      record.record.payload_size = payload_size;
      record.record.file_offset = file_offset;
      for (std::size_t hash_index = 0; hash_index < record.record.hash.size();
           ++hash_index) {
        record.record.hash[hash_index] =
            std::to_integer<std::uint8_t>(hash_bytes[hash_index]);
      }
      record.payload.assign(payload.begin(), payload.end());
      request.inputs.push_back(std::move(record));
      aggregate_input_bytes += payload_size;
    }
  } catch (const std::bad_alloc&) {
    return fail<DistributedRemoteExecutionRequest>(
        ErrorCode::resource_exhausted,
        "distributed remote wire request allocation failed");
  }
  if (!reader.empty()) {
    return fail<DistributedRemoteExecutionRequest>(
        ErrorCode::protocol,
        "distributed remote wire request has trailing body bytes");
  }
  return request;
}

Result<std::vector<std::byte>> encode_distributed_remote_reply(
    const DistributedRemoteExecutionReply& reply,
    DistributedRemoteWireLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();

  try {
    std::vector<std::byte> body;
    if (std::holds_alternative<DistributedRemoteExecutionResponse>(reply)) {
      const auto& response =
          std::get<DistributedRemoteExecutionResponse>(reply);
      auto worker_label = validate_label(response.worker_name, limits);
      if (!worker_label) return worker_label.error();
      auto processor_label = validate_label(response.processor_name, limits);
      if (!processor_label) return processor_label.error();
      if (response.outputs.size() > limits.maximum_outputs ||
          response.outputs.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail<std::vector<std::byte>>(
            ErrorCode::resource_exhausted,
            "distributed remote wire output count exceeds configured limit");
      }

      std::uint64_t aggregate_output_bytes = 0;
      std::uint64_t body_size = 20;
      if (!checked_add_to(&body_size,
                          static_cast<std::uint64_t>(response.worker_name.size())) ||
          !checked_add_to(&body_size, static_cast<std::uint64_t>(
                                           response.processor_name.size()))) {
        return fail<std::vector<std::byte>>(
            ErrorCode::resource_exhausted,
            "distributed remote wire reply is too large");
      }
      for (const auto& output : response.outputs) {
        if (!valid_truth(output.truth)) {
          return fail<std::vector<std::byte>>(
              ErrorCode::invalid_argument,
              "distributed remote wire output truth class is invalid");
        }
        auto process_bounds = validate_process_bounds(output.process, limits);
        if (!process_bounds) return process_bounds.error();
        const auto payload_size =
            static_cast<std::uint64_t>(output.payload.size());
        if (payload_size > limits.maximum_output_bytes - aggregate_output_bytes) {
          return fail<std::vector<std::byte>>(
              ErrorCode::resource_exhausted,
              "distributed remote wire output bytes exceed configured limit");
        }
        aggregate_output_bytes += payload_size;
        auto variable = encoded_process_variable_size(output.process);
        if (!variable) return variable.error();
        if (!checked_add_to(&body_size, encoded_output_fixed_bytes) ||
            !checked_add_to(&body_size, *variable) ||
            !checked_add_to(&body_size, payload_size)) {
          return fail<std::vector<std::byte>>(
              ErrorCode::resource_exhausted,
              "distributed remote wire reply is too large");
        }
      }
      std::uint64_t envelope_size = envelope_header_size;
      if (!checked_add_to(&envelope_size, body_size) ||
          envelope_size > limits.maximum_envelope_bytes ||
          body_size > std::numeric_limits<std::size_t>::max()) {
        return fail<std::vector<std::byte>>(
            ErrorCode::resource_exhausted,
            "distributed remote wire reply exceeds configured envelope limit");
      }

      body.reserve(static_cast<std::size_t>(body_size));
      append_u8(body, 0);
      for (std::size_t index = 0; index < 7; ++index) append_u8(body, 0);
      append_string(body, response.worker_name);
      append_string(body, response.processor_name);
      append_u32(body, static_cast<std::uint32_t>(response.outputs.size()));
      for (const auto& output : response.outputs) {
        append_process_and_payload(body, output);
      }
      if (body.size() != body_size) {
        return fail<std::vector<std::byte>>(
            ErrorCode::internal,
            "distributed remote wire reply encoder size mismatch");
      }
    } else {
      const auto& error = std::get<Error>(reply);
      auto wire_code = error_code_to_wire(error.code);
      if (!wire_code) return wire_code.error();
      if (error.message.size() > limits.maximum_error_message_bytes ||
          error.message.size() > std::numeric_limits<std::uint32_t>::max()) {
        return fail<std::vector<std::byte>>(
            ErrorCode::resource_exhausted,
            "distributed remote wire error message exceeds configured limit");
      }
      const std::uint64_t body_size = 16 + error.message.size();
      std::uint64_t envelope_size = envelope_header_size;
      if (!checked_add_to(&envelope_size, body_size) ||
          envelope_size > limits.maximum_envelope_bytes) {
        return fail<std::vector<std::byte>>(
            ErrorCode::resource_exhausted,
            "distributed remote wire error reply exceeds configured envelope limit");
      }
      body.reserve(static_cast<std::size_t>(body_size));
      append_u8(body, 1);
      for (std::size_t index = 0; index < 7; ++index) append_u8(body, 0);
      append_u16(body, *wire_code);
      append_u8(body, error.retryable ? 1U : 0U);
      append_u8(body, 0);
      append_u32(body, static_cast<std::uint32_t>(error.message.size()));
      const auto raw =
          std::as_bytes(std::span{error.message.data(), error.message.size()});
      body.insert(body.end(), raw.begin(), raw.end());
    }
    return finalize_envelope(reply_magic, body, limits);
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "distributed remote wire reply allocation failed");
  }
}

Result<DistributedRemoteExecutionReply> decode_distributed_remote_reply(
    std::span<const std::byte> encoded,
    DistributedRemoteWireLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto body = validate_envelope(encoded, reply_magic, limits);
  if (!body) return body.error();

  Reader reader{*body};
  std::uint8_t outcome{};
  if (!reader.read_u8(&outcome)) {
    return fail<DistributedRemoteExecutionReply>(
        ErrorCode::protocol,
        "distributed remote wire reply outcome is truncated");
  }
  for (std::size_t index = 0; index < 7; ++index) {
    std::uint8_t reserved{};
    if (!reader.read_u8(&reserved) || reserved != 0) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire reply reserved field is invalid");
    }
  }

  if (outcome == 0) {
    auto worker_name = read_string(reader, limits.maximum_label_bytes, true);
    if (!worker_name) return worker_name.error();
    auto processor_name = read_string(reader, limits.maximum_label_bytes, true);
    if (!processor_name) return processor_name.error();
    std::uint32_t count{};
    if (!reader.read_u32(&count)) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire output count is truncated");
    }
    if (count > limits.maximum_outputs) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::resource_exhausted,
          "distributed remote wire output count exceeds configured limit");
    }

    DistributedRemoteExecutionResponse response;
    response.worker_name = std::move(*worker_name);
    response.processor_name = std::move(*processor_name);
    std::uint64_t aggregate_output_bytes = 0;
    try {
      response.outputs.reserve(count);
      for (std::uint32_t index = 0; index < count; ++index) {
        auto output = decode_output(reader, limits, &aggregate_output_bytes);
        if (!output) return output.error();
        response.outputs.push_back(std::move(*output));
      }
    } catch (const std::bad_alloc&) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::resource_exhausted,
          "distributed remote wire reply allocation failed");
    }
    if (!reader.empty()) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire success reply has trailing body bytes");
    }
    return DistributedRemoteExecutionReply{std::move(response)};
  }

  if (outcome == 1) {
    std::uint16_t wire_code{};
    std::uint8_t retryable{};
    std::uint8_t reserved{};
    std::uint32_t message_length{};
    if (!reader.read_u16(&wire_code) || !reader.read_u8(&retryable) ||
        !reader.read_u8(&reserved) || !reader.read_u32(&message_length)) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire error reply is truncated");
    }
    if (retryable > 1 || reserved != 0) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire error reply flags are invalid");
    }
    auto code = error_code_from_wire(wire_code);
    if (!code) return code.error();
    if (message_length > limits.maximum_error_message_bytes) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::resource_exhausted,
          "distributed remote wire error message exceeds configured limit");
    }
    std::span<const std::byte> raw;
    if (!reader.read_bytes(message_length, &raw) || !reader.empty()) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::protocol,
          "distributed remote wire error message length is inconsistent");
    }
    try {
      std::string message{reinterpret_cast<const char*>(raw.data()), raw.size()};
      return DistributedRemoteExecutionReply{
          Error{*code, std::move(message), retryable == 1}};
    } catch (const std::bad_alloc&) {
      return fail<DistributedRemoteExecutionReply>(
          ErrorCode::resource_exhausted,
          "distributed remote wire error allocation failed");
    }
  }

  return fail<DistributedRemoteExecutionReply>(
      ErrorCode::protocol,
      "distributed remote wire reply outcome is unknown");
}

}  // namespace codec
