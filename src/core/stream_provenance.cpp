#include "internal.hpp"

#include <cstring>
#include <limits>
#include <type_traits>

namespace codec::detail {
namespace {

constexpr std::size_t fixed_header_size = 56;
constexpr std::size_t link_size = 60;
constexpr std::uint32_t has_implementation_hash = 1U << 0U;
constexpr std::uint32_t has_configuration_hash = 1U << 1U;
constexpr std::uint32_t known_flags =
    has_implementation_hash | has_configuration_hash;
constexpr std::size_t maximum_inputs = 4096;
constexpr std::size_t maximum_text_size = 4096;
constexpr std::size_t maximum_details_size = 1024 * 1024;

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
                static_cast<std::uint8_t>(input[offset + index]))
            << (index * 8U);
  }
  return static_cast<Integer>(bits);
}

void put_hash(std::span<std::byte> output, std::size_t offset,
              const Sha256& hash) {
  for (std::size_t index = 0; index < hash.size(); ++index) {
    output[offset + index] = static_cast<std::byte>(hash[index]);
  }
}

Sha256 get_hash(std::span<const std::byte> input, std::size_t offset) {
  Sha256 hash{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    hash[index] = static_cast<std::uint8_t>(input[offset + index]);
  }
  return hash;
}

void put_link(std::span<std::byte> output, std::size_t offset,
              const ProvenanceRecordLink& link) {
  for (std::size_t index = 0; index < link.stream.bytes.size(); ++index) {
    output[offset + index] = static_cast<std::byte>(link.stream.bytes[index]);
  }
  put_le<RecordTypeCode>(output, offset + 16, link.type);
  put_le<std::uint16_t>(output, offset + 18, 0);
  put_le<std::uint64_t>(output, offset + 20, link.sequence);
  put_hash(output, offset + 28, link.hash);
}

Result<ProvenanceRecordLink> get_link(std::span<const std::byte> input,
                                      std::size_t offset) {
  if (get_le<std::uint16_t>(input, offset + 18) != 0) {
    return fail<ProvenanceRecordLink>(ErrorCode::archive_corrupt,
                                      "invalid provenance link reserved field");
  }
  ProvenanceRecordLink link;
  for (std::size_t index = 0; index < link.stream.bytes.size(); ++index) {
    link.stream.bytes[index] =
        static_cast<std::uint8_t>(input[offset + index]);
  }
  link.type = get_le<RecordTypeCode>(input, offset + 16);
  link.sequence = get_le<std::uint64_t>(input, offset + 20);
  link.hash = get_hash(input, offset + 28);
  return link;
}

bool add_size(std::uint64_t& total, std::uint64_t value) {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

bool contains_nul(std::string_view value) {
  return value.find('\0') != std::string_view::npos;
}

}  // namespace

Result<std::vector<std::byte>> encode_stream_provenance(
    const StreamProvenance& provenance) {
  constexpr auto maximum_u32 =
      static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  if (provenance.subject_truth != TruthClass::state_exact &&
      provenance.subject_truth != TruthClass::derived) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance subject truth class must be S1 or D");
  }
  if (provenance.inputs.empty() ||
      provenance.inputs.size() > maximum_inputs) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance input count exceeds the development profile limit");
  }
  const auto& process = provenance.process;
  if (process.operation.empty() || process.implementation_id.empty() ||
      process.implementation_version.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance process identity fields must not be empty");
  }
  if (process.operation.size() > maximum_text_size ||
      process.implementation_id.size() > maximum_text_size ||
      process.implementation_version.size() > maximum_text_size ||
      process.details_type.size() > maximum_text_size ||
      process.details.size() > maximum_details_size) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance process field exceeds the development profile limit");
  }
  if (contains_nul(process.operation) ||
      contains_nul(process.implementation_id) ||
      contains_nul(process.implementation_version) ||
      contains_nul(process.details_type)) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance process text contains an embedded NUL");
  }
  if (process.details_type.empty() != process.details.empty()) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance details type and bytes must be present together");
  }
  if (provenance.inputs.size() > maximum_u32 ||
      provenance.process.operation.size() > maximum_u32 ||
      provenance.process.implementation_id.size() > maximum_u32 ||
      provenance.process.implementation_version.size() > maximum_u32 ||
      provenance.process.details_type.size() > maximum_u32 ||
      provenance.process.details.size() > maximum_u32) {
    return fail<std::vector<std::byte>>(
        ErrorCode::invalid_argument,
        "provenance payload exceeds the development profile encoding");
  }

  std::uint64_t size = fixed_header_size + link_size;
  const auto flags =
      (provenance.process.implementation_hash ? has_implementation_hash : 0U) |
      (provenance.process.configuration_hash ? has_configuration_hash : 0U);
  if ((flags & has_implementation_hash) != 0U) size += Sha256{}.size();
  if ((flags & has_configuration_hash) != 0U) size += Sha256{}.size();
  if (!add_size(size, provenance.inputs.size() * link_size) ||
      !add_size(size, provenance.process.operation.size()) ||
      !add_size(size, provenance.process.implementation_id.size()) ||
      !add_size(size, provenance.process.implementation_version.size()) ||
      !add_size(size, provenance.process.details_type.size()) ||
      !add_size(size, provenance.process.details.size()) ||
      size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                        "provenance payload is too large");
  }

  std::vector<std::byte> output(static_cast<std::size_t>(size));
  std::memcpy(output.data(), "SPV1", 4);
  put_le<std::uint16_t>(output, 4, 1);
  put_le<std::uint16_t>(output, 6, 0);
  output[8] = static_cast<std::byte>(provenance.subject_truth);
  put_le<std::uint32_t>(output, 12, flags);
  put_le<std::uint32_t>(output, 16,
                        static_cast<std::uint32_t>(provenance.inputs.size()));
  put_le<std::uint32_t>(
      output, 20,
      static_cast<std::uint32_t>(provenance.process.operation.size()));
  put_le<std::uint32_t>(
      output, 24,
      static_cast<std::uint32_t>(provenance.process.implementation_id.size()));
  put_le<std::uint32_t>(
      output, 28, static_cast<std::uint32_t>(
                      provenance.process.implementation_version.size()));
  put_le<std::uint32_t>(
      output, 32,
      static_cast<std::uint32_t>(provenance.process.details_type.size()));
  put_le<std::uint32_t>(
      output, 36,
      static_cast<std::uint32_t>(provenance.process.details.size()));
  put_le<std::int64_t>(output, 40, provenance.process.created_utc_ns);
  put_le<std::uint64_t>(output, 48, 0);

  std::size_t cursor = fixed_header_size;
  put_link(output, cursor, provenance.subject);
  cursor += link_size;
  if (provenance.process.implementation_hash) {
    put_hash(output, cursor, *provenance.process.implementation_hash);
    cursor += Sha256{}.size();
  }
  if (provenance.process.configuration_hash) {
    put_hash(output, cursor, *provenance.process.configuration_hash);
    cursor += Sha256{}.size();
  }
  for (const auto& input : provenance.inputs) {
    put_link(output, cursor, input);
    cursor += link_size;
  }
  const auto copy_text = [&output, &cursor](std::string_view value) {
    std::memcpy(output.data() + cursor, value.data(), value.size());
    cursor += value.size();
  };
  copy_text(provenance.process.operation);
  copy_text(provenance.process.implementation_id);
  copy_text(provenance.process.implementation_version);
  copy_text(provenance.process.details_type);
  std::copy(provenance.process.details.begin(),
            provenance.process.details.end(), output.begin() + cursor);
  return output;
}

Result<StreamProvenance> decode_stream_provenance(
    std::span<const std::byte> payload) {
  if (payload.size() < fixed_header_size + link_size ||
      std::memcmp(payload.data(), "SPV1", 4) != 0 ||
      get_le<std::uint16_t>(payload, 4) != 1 ||
      get_le<std::uint16_t>(payload, 6) != 0 ||
      payload[9] != std::byte{0} || payload[10] != std::byte{0} ||
      payload[11] != std::byte{0} ||
      get_le<std::uint64_t>(payload, 48) != 0) {
    return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                  "invalid stream provenance record");
  }
  const auto flags = get_le<std::uint32_t>(payload, 12);
  if ((flags & ~known_flags) != 0U) {
    return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                  "invalid stream provenance flags");
  }
  const auto input_count = get_le<std::uint32_t>(payload, 16);
  const std::array lengths{
      get_le<std::uint32_t>(payload, 20),
      get_le<std::uint32_t>(payload, 24),
      get_le<std::uint32_t>(payload, 28),
      get_le<std::uint32_t>(payload, 32),
      get_le<std::uint32_t>(payload, 36),
  };
  const auto truth =
      static_cast<TruthClass>(static_cast<std::uint8_t>(payload[8]));
  if (truth != TruthClass::state_exact && truth != TruthClass::derived) {
    return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                  "invalid provenance subject truth class");
  }
  if (input_count == 0 || input_count > maximum_inputs ||
      lengths[0] == 0 || lengths[0] > maximum_text_size ||
      lengths[1] == 0 || lengths[1] > maximum_text_size ||
      lengths[2] == 0 || lengths[2] > maximum_text_size ||
      lengths[3] > maximum_text_size ||
      lengths[4] > maximum_details_size) {
    return fail<StreamProvenance>(
        ErrorCode::archive_corrupt,
        "provenance field exceeds the development profile limit");
  }
  std::uint64_t expected_size = fixed_header_size + link_size;
  if ((flags & has_implementation_hash) != 0U) {
    expected_size += Sha256{}.size();
  }
  if ((flags & has_configuration_hash) != 0U) {
    expected_size += Sha256{}.size();
  }
  if (!add_size(expected_size,
                static_cast<std::uint64_t>(input_count) * link_size)) {
    return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                  "invalid stream provenance size");
  }
  for (const auto length : lengths) {
    if (!add_size(expected_size, length)) {
      return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                    "invalid stream provenance size");
    }
  }
  if (expected_size != payload.size()) {
    return fail<StreamProvenance>(ErrorCode::archive_corrupt,
                                  "invalid stream provenance lengths");
  }

  StreamProvenance provenance;
  provenance.subject_truth = truth;
  provenance.process.created_utc_ns = get_le<std::int64_t>(payload, 40);
  std::size_t cursor = fixed_header_size;
  auto subject = get_link(payload, cursor);
  if (!subject) return subject.error();
  provenance.subject = *subject;
  cursor += link_size;
  if ((flags & has_implementation_hash) != 0U) {
    provenance.process.implementation_hash = get_hash(payload, cursor);
    cursor += Sha256{}.size();
  }
  if ((flags & has_configuration_hash) != 0U) {
    provenance.process.configuration_hash = get_hash(payload, cursor);
    cursor += Sha256{}.size();
  }
  provenance.inputs.reserve(input_count);
  for (std::uint32_t index = 0; index < input_count; ++index) {
    auto input = get_link(payload, cursor);
    if (!input) return input.error();
    provenance.inputs.push_back(*input);
    cursor += link_size;
  }
  const auto read_text = [&payload, &cursor](std::uint32_t length) {
    const auto* data = reinterpret_cast<const char*>(payload.data() + cursor);
    cursor += length;
    return std::string{data, length};
  };
  provenance.process.operation = read_text(lengths[0]);
  provenance.process.implementation_id = read_text(lengths[1]);
  provenance.process.implementation_version = read_text(lengths[2]);
  provenance.process.details_type = read_text(lengths[3]);
  provenance.process.details.assign(payload.begin() + cursor,
                                    payload.begin() + cursor + lengths[4]);
  if (contains_nul(provenance.process.operation) ||
      contains_nul(provenance.process.implementation_id) ||
      contains_nul(provenance.process.implementation_version) ||
      contains_nul(provenance.process.details_type)) {
    return fail<StreamProvenance>(
        ErrorCode::archive_corrupt,
        "provenance process text contains an embedded NUL");
  }
  if (provenance.process.details_type.empty() !=
      provenance.process.details.empty()) {
    return fail<StreamProvenance>(
        ErrorCode::archive_corrupt,
        "provenance details type and bytes must be present together");
  }
  return provenance;
}

}  // namespace codec::detail
