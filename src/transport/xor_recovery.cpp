#include <codec/xor_recovery.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace codec {
namespace {

constexpr std::uint64_t cmx1_header_size = 164;
constexpr std::uint64_t xrf1_fixed_header_size = 92;
constexpr std::uint64_t xrf1_table_entry_size = 40;
constexpr std::uint64_t xrf1_digest_size = 32;
constexpr std::array<std::byte, 4> xrf1_magic{
    std::byte{'X'}, std::byte{'R'}, std::byte{'F'}, std::byte{'1'}};

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

bool same_epoch(const StreamEpoch& lhs, const StreamEpoch& rhs) noexcept {
  return lhs.connection == rhs.connection && lhs.format == rhs.format;
}

bool checked_add(std::uint64_t lhs, std::uint64_t rhs,
                 std::uint64_t* output) noexcept {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) return false;
  *output = lhs + rhs;
  return true;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs,
                      std::uint64_t* output) noexcept {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return false;
  }
  *output = lhs * rhs;
  return true;
}

Result<void> validate_limits(const XorRepairLimits& limits) {
  if (limits.maximum_source_frames == 0 ||
      limits.maximum_total_encoded_source_bytes == 0 ||
      limits.maximum_symbol_bytes == 0 ||
      limits.multiplex.maximum_payload_bytes == 0 ||
      limits.multiplex.maximum_buffered_bytes == 0 ||
      limits.multiplex.maximum_frames_per_push == 0) {
    return fail(ErrorCode::invalid_argument,
                "XOR recovery limits must be non-zero");
  }
  if (limits.multiplex.maximum_buffered_bytes < cmx1_header_size ||
      limits.multiplex.maximum_payload_bytes >
          limits.multiplex.maximum_buffered_bytes - cmx1_header_size) {
    return fail(ErrorCode::invalid_argument,
                "XOR recovery multiplex limits are inconsistent");
  }
  if (limits.multiplex.maximum_payload_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.multiplex.maximum_buffered_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.maximum_total_encoded_source_bytes >
          std::numeric_limits<std::size_t>::max() ||
      limits.maximum_symbol_bytes >
          std::numeric_limits<std::size_t>::max()) {
    return fail(ErrorCode::invalid_argument,
                "XOR recovery limits exceed addressable memory");
  }
  return {};
}

Result<std::uint64_t> validate_descriptor(
    const RecoveryGroupDescriptor& descriptor,
    const XorRepairLimits& limits, ErrorCode structural_error) {
  if (descriptor.source_count < 2) {
    return fail<std::uint64_t>(
        structural_error,
        "XOR recovery groups require at least two source frames");
  }
  if (descriptor.source_count > limits.maximum_source_frames ||
      descriptor.source_count >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return fail<std::uint64_t>(
        ErrorCode::resource_exhausted,
        "XOR recovery source-frame limit exceeded");
  }
  std::uint64_t end = 0;
  if (!checked_add(descriptor.first_sequence, descriptor.source_count, &end)) {
    return fail<std::uint64_t>(
        structural_error,
        "XOR recovery source sequence range overflows");
  }
  return end;
}

Result<std::uint64_t> required_symbol_size(std::uint64_t source_count,
                                           std::uint64_t parity_size) {
  std::uint64_t table_size = 0;
  std::uint64_t total_size = 0;
  if (!checked_multiply(source_count, xrf1_table_entry_size, &table_size) ||
      !checked_add(xrf1_fixed_header_size, table_size, &total_size) ||
      !checked_add(total_size, parity_size, &total_size) ||
      !checked_add(total_size, xrf1_digest_size, &total_size)) {
    return fail<std::uint64_t>(ErrorCode::resource_exhausted,
                               "XRF1 symbol size overflows");
  }
  return total_size;
}

Result<std::uint64_t> validate_symbol(const XorRepairSymbol& symbol,
                                      const XorRepairLimits& limits,
                                      ErrorCode structural_error) {
  auto end = validate_descriptor(symbol.descriptor, limits, structural_error);
  if (!end) return end.error();
  const auto expected_count =
      static_cast<std::size_t>(symbol.descriptor.source_count);
  if (symbol.encoded_frame_sizes.size() != expected_count ||
      symbol.encoded_frame_hashes.size() != expected_count ||
      symbol.parity.empty()) {
    return fail<std::uint64_t>(
        structural_error,
        "XRF1 slot vectors do not match the recovery group");
  }

  std::uint64_t aggregate_bytes = 0;
  std::uint64_t maximum_frame_bytes = 0;
  for (const auto frame_size : symbol.encoded_frame_sizes) {
    if (frame_size < cmx1_header_size) {
      return fail<std::uint64_t>(structural_error,
                                 "XRF1 frame size is smaller than CMX1");
    }
    if (frame_size > limits.multiplex.maximum_buffered_bytes ||
        frame_size > std::numeric_limits<std::size_t>::max()) {
      return fail<std::uint64_t>(
          ErrorCode::resource_exhausted,
          "XRF1 frame size exceeds the configured multiplex limit");
    }
    if (!checked_add(aggregate_bytes, frame_size, &aggregate_bytes) ||
        aggregate_bytes > limits.maximum_total_encoded_source_bytes) {
      return fail<std::uint64_t>(
          ErrorCode::resource_exhausted,
          "XRF1 aggregate source-byte limit exceeded");
    }
    maximum_frame_bytes = std::max(maximum_frame_bytes, frame_size);
  }
  if (symbol.parity.size() != maximum_frame_bytes) {
    return fail<std::uint64_t>(
        structural_error,
        "XRF1 parity size is not the maximum encoded frame size");
  }

  auto total_size = required_symbol_size(symbol.descriptor.source_count,
                                         maximum_frame_bytes);
  if (!total_size) return total_size.error();
  if (*total_size > limits.maximum_symbol_bytes ||
      *total_size > std::numeric_limits<std::size_t>::max()) {
    return fail<std::uint64_t>(
        ErrorCode::resource_exhausted,
        "XRF1 symbol exceeds the configured byte limit");
  }
  return *total_size;
}

bool digest_matches(std::span<const std::byte> bytes,
                    const Sha256& digest) noexcept {
  const auto digest_offset = bytes.size() - digest.size();
  for (std::size_t index = 0; index < digest.size(); ++index) {
    if (static_cast<std::uint8_t>(bytes[digest_offset + index]) !=
        digest[index]) {
      return false;
    }
  }
  return true;
}

}  // namespace

Result<XorRepairSymbol> create_xor_repair_symbol(
    const RecoveryGroupDescriptor& descriptor,
    std::span<const MultiplexFrame> source_frames,
    XorRepairLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto end = validate_descriptor(descriptor, limits,
                                 ErrorCode::invalid_argument);
  if (!end) return end.error();
  if (source_frames.size() != descriptor.source_count) {
    return fail<XorRepairSymbol>(
        ErrorCode::invalid_argument,
        "XOR recovery source-frame count does not match the group");
  }

  try {
    std::vector<std::optional<std::vector<std::byte>>> slots(
        static_cast<std::size_t>(descriptor.source_count));
    std::uint64_t aggregate_bytes = 0;
    std::uint64_t maximum_frame_bytes = 0;

    for (const auto& frame : source_frames) {
      if (frame.stream != descriptor.key.stream ||
          !same_epoch(frame.epoch, descriptor.key.epoch) ||
          frame.sequence < descriptor.first_sequence ||
          frame.sequence >= *end) {
        return fail<XorRepairSymbol>(
            ErrorCode::invalid_argument,
            "XOR recovery source frame is outside the exact group");
      }
      const auto index = static_cast<std::size_t>(
          frame.sequence - descriptor.first_sequence);
      if (slots[index].has_value()) {
        return fail<XorRepairSymbol>(
            ErrorCode::invalid_argument,
            "XOR recovery source group contains a duplicate sequence");
      }

      auto encoded = encode_multiplex_frame(frame, limits.multiplex);
      if (!encoded) return encoded.error();
      const auto encoded_size = static_cast<std::uint64_t>(encoded->size());
      if (encoded_size > limits.multiplex.maximum_buffered_bytes) {
        return fail<XorRepairSymbol>(
            ErrorCode::resource_exhausted,
            "XOR recovery encoded frame exceeds the multiplex limit");
      }
      if (!checked_add(aggregate_bytes, encoded_size, &aggregate_bytes) ||
          aggregate_bytes > limits.maximum_total_encoded_source_bytes) {
        return fail<XorRepairSymbol>(
            ErrorCode::resource_exhausted,
            "XOR recovery aggregate source-byte limit exceeded");
      }
      maximum_frame_bytes = std::max(maximum_frame_bytes, encoded_size);
      slots[index] = std::move(*encoded);
    }

    auto symbol_size = required_symbol_size(descriptor.source_count,
                                            maximum_frame_bytes);
    if (!symbol_size) return symbol_size.error();
    if (*symbol_size > limits.maximum_symbol_bytes ||
        *symbol_size > std::numeric_limits<std::size_t>::max()) {
      return fail<XorRepairSymbol>(
          ErrorCode::resource_exhausted,
          "XRF1 symbol exceeds the configured byte limit");
    }

    XorRepairSymbol symbol;
    symbol.descriptor = descriptor;
    symbol.encoded_frame_sizes.reserve(slots.size());
    symbol.encoded_frame_hashes.reserve(slots.size());
    symbol.parity.assign(static_cast<std::size_t>(maximum_frame_bytes),
                         std::byte{0});
    for (const auto& slot : slots) {
      if (!slot.has_value()) {
        return fail<XorRepairSymbol>(
            ErrorCode::internal,
            "XOR recovery canonical source slot is unexpectedly empty");
      }
      symbol.encoded_frame_sizes.push_back(slot->size());
      symbol.encoded_frame_hashes.push_back(sha256(*slot));
      for (std::size_t offset = 0; offset < slot->size(); ++offset) {
        symbol.parity[offset] ^= (*slot)[offset];
      }
    }
    return symbol;
  } catch (const std::bad_alloc&) {
    return fail<XorRepairSymbol>(ErrorCode::resource_exhausted,
                                 "XOR recovery allocation failed");
  }
}

Result<std::vector<std::byte>> encode_xor_repair_symbol(
    const XorRepairSymbol& symbol, XorRepairLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto total_size = validate_symbol(symbol, limits,
                                    ErrorCode::invalid_argument);
  if (!total_size) return total_size.error();

  try {
    std::vector<std::byte> output(static_cast<std::size_t>(*total_size));
    std::copy(xrf1_magic.begin(), xrf1_magic.end(), output.begin());
    put_le<std::uint16_t>(output, 4, xor_repair_symbol_version);
    put_le<std::uint16_t>(output, 6, 0);
    put_le<std::uint32_t>(
        output, 8, static_cast<std::uint32_t>(xrf1_fixed_header_size));
    put_le<std::uint64_t>(output, 12, *total_size);
    for (std::size_t index = 0;
         index < symbol.descriptor.key.stream.bytes.size(); ++index) {
      output[20 + index] = static_cast<std::byte>(
          symbol.descriptor.key.stream.bytes[index]);
    }
    put_le<std::uint64_t>(output, 36,
                          symbol.descriptor.key.epoch.connection);
    put_le<std::uint64_t>(output, 44,
                          symbol.descriptor.key.epoch.format);
    put_le<std::uint64_t>(output, 52,
                          symbol.descriptor.key.group_sequence);
    put_le<std::uint64_t>(output, 60,
                          symbol.descriptor.first_sequence);
    put_le<std::uint64_t>(output, 68,
                          symbol.descriptor.source_count);
    put_le<std::uint64_t>(output, 76, symbol.parity.size());
    put_le<std::uint32_t>(
        output, 84, static_cast<std::uint32_t>(xrf1_table_entry_size));
    put_le<std::uint32_t>(output, 88, 0);

    std::size_t offset = static_cast<std::size_t>(xrf1_fixed_header_size);
    for (std::size_t index = 0;
         index < symbol.encoded_frame_sizes.size(); ++index) {
      put_le<std::uint64_t>(output, offset,
                            symbol.encoded_frame_sizes[index]);
      offset += sizeof(std::uint64_t);
      for (const auto byte : symbol.encoded_frame_hashes[index]) {
        output[offset++] = static_cast<std::byte>(byte);
      }
    }
    std::copy(symbol.parity.begin(), symbol.parity.end(),
              output.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += symbol.parity.size();

    const auto digest = sha256(
        std::span<const std::byte>{output}.first(offset));
    for (const auto byte : digest) {
      output[offset++] = static_cast<std::byte>(byte);
    }
    if (offset != output.size()) {
      return fail<std::vector<std::byte>>(
          ErrorCode::internal,
          "XRF1 encoder produced an inconsistent output size");
    }
    return output;
  } catch (const std::bad_alloc&) {
    return fail<std::vector<std::byte>>(
        ErrorCode::resource_exhausted,
        "XRF1 encoder allocation failed");
  }
}

Result<XorRepairSymbol> decode_xor_repair_symbol(
    std::span<const std::byte> bytes, XorRepairLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  if (bytes.size() < xrf1_fixed_header_size + xrf1_digest_size) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "XRF1 symbol is truncated");
  }
  if (!std::equal(xrf1_magic.begin(), xrf1_magic.end(), bytes.begin())) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "invalid XRF1 magic");
  }
  if (get_le<std::uint16_t>(bytes, 4) != xor_repair_symbol_version) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "unsupported XRF1 version");
  }
  if (get_le<std::uint16_t>(bytes, 6) != 0) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "unsupported XRF1 flags");
  }
  if (get_le<std::uint32_t>(bytes, 8) != xrf1_fixed_header_size) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "invalid XRF1 fixed header size");
  }
  if (get_le<std::uint32_t>(bytes, 84) != xrf1_table_entry_size) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "invalid XRF1 table entry size");
  }
  if (get_le<std::uint32_t>(bytes, 88) != 0) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "XRF1 reserved field is non-zero");
  }

  const auto declared_total_size = get_le<std::uint64_t>(bytes, 12);
  const auto source_count = get_le<std::uint64_t>(bytes, 68);
  const auto parity_size = get_le<std::uint64_t>(bytes, 76);
  if (declared_total_size > limits.maximum_symbol_bytes ||
      declared_total_size > std::numeric_limits<std::size_t>::max()) {
    return fail<XorRepairSymbol>(
        ErrorCode::resource_exhausted,
        "XRF1 declared size exceeds the configured byte limit");
  }
  if (parity_size > limits.multiplex.maximum_buffered_bytes ||
      parity_size > std::numeric_limits<std::size_t>::max()) {
    return fail<XorRepairSymbol>(
        ErrorCode::resource_exhausted,
        "XRF1 parity exceeds the configured multiplex limit");
  }

  XorRepairSymbol symbol;
  for (std::size_t index = 0;
       index < symbol.descriptor.key.stream.bytes.size(); ++index) {
    symbol.descriptor.key.stream.bytes[index] =
        static_cast<std::uint8_t>(bytes[20 + index]);
  }
  symbol.descriptor.key.epoch.connection =
      get_le<std::uint64_t>(bytes, 36);
  symbol.descriptor.key.epoch.format = get_le<std::uint64_t>(bytes, 44);
  symbol.descriptor.key.group_sequence = get_le<std::uint64_t>(bytes, 52);
  symbol.descriptor.first_sequence = get_le<std::uint64_t>(bytes, 60);
  symbol.descriptor.source_count = source_count;

  auto end = validate_descriptor(symbol.descriptor, limits,
                                 ErrorCode::protocol);
  if (!end) return end.error();
  std::uint64_t table_size = 0;
  std::uint64_t canonical_total_size = 0;
  if (!checked_multiply(source_count, xrf1_table_entry_size, &table_size) ||
      !checked_add(xrf1_fixed_header_size, table_size,
                   &canonical_total_size) ||
      !checked_add(canonical_total_size, parity_size,
                   &canonical_total_size) ||
      !checked_add(canonical_total_size, xrf1_digest_size,
                   &canonical_total_size)) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "XRF1 declared sizes overflow");
  }
  if (canonical_total_size > limits.maximum_symbol_bytes ||
      canonical_total_size > std::numeric_limits<std::size_t>::max()) {
    return fail<XorRepairSymbol>(
        ErrorCode::resource_exhausted,
        "XRF1 canonical size exceeds the configured byte limit");
  }
  if (declared_total_size != canonical_total_size ||
      bytes.size() != canonical_total_size) {
    return fail<XorRepairSymbol>(
        ErrorCode::protocol,
        "XRF1 total size is noncanonical");
  }

  const auto digest = sha256(bytes.first(
      bytes.size() - static_cast<std::size_t>(xrf1_digest_size)));
  if (!digest_matches(bytes, digest)) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "XRF1 SHA-256 mismatch");
  }

  try {
    symbol.encoded_frame_sizes.reserve(static_cast<std::size_t>(source_count));
    symbol.encoded_frame_hashes.reserve(
        static_cast<std::size_t>(source_count));
    std::size_t offset = static_cast<std::size_t>(xrf1_fixed_header_size);
    for (std::uint64_t index = 0; index < source_count; ++index) {
      symbol.encoded_frame_sizes.push_back(
          get_le<std::uint64_t>(bytes, offset));
      offset += sizeof(std::uint64_t);
      Sha256 frame_hash{};
      for (std::size_t byte_index = 0; byte_index < frame_hash.size();
           ++byte_index) {
        frame_hash[byte_index] =
            static_cast<std::uint8_t>(bytes[offset++]);
      }
      symbol.encoded_frame_hashes.push_back(frame_hash);
    }
    symbol.parity.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(
                            offset + static_cast<std::size_t>(parity_size)));
  } catch (const std::bad_alloc&) {
    return fail<XorRepairSymbol>(ErrorCode::resource_exhausted,
                                 "XRF1 decoder allocation failed");
  }

  auto validated = validate_symbol(symbol, limits, ErrorCode::protocol);
  if (!validated) return validated.error();
  if (*validated != canonical_total_size) {
    return fail<XorRepairSymbol>(ErrorCode::protocol,
                                 "XRF1 decoded size is inconsistent");
  }
  return symbol;
}

Result<XorRecoveredFrame> recover_xor_single_erasure(
    const XorRepairSymbol& symbol,
    std::span<const MultiplexFrame> observed_source_frames,
    XorRepairLimits limits) {
  auto valid_limits = validate_limits(limits);
  if (!valid_limits) return valid_limits.error();
  auto total_size = validate_symbol(symbol, limits,
                                    ErrorCode::invalid_argument);
  if (!total_size) return total_size.error();

  const auto source_count =
      static_cast<std::size_t>(symbol.descriptor.source_count);
  if (observed_source_frames.size() != source_count - 1) {
    return fail<XorRecoveredFrame>(
        ErrorCode::invalid_argument,
        "XOR recovery requires exactly one missing source frame");
  }
  auto end = validate_descriptor(symbol.descriptor, limits,
                                 ErrorCode::invalid_argument);
  if (!end) return end.error();

  try {
    std::vector<std::uint8_t> present(source_count, 0);
    auto recovered_bytes = symbol.parity;
    std::uint64_t aggregate_observed_bytes = 0;

    for (const auto& frame : observed_source_frames) {
      if (frame.stream != symbol.descriptor.key.stream ||
          !same_epoch(frame.epoch, symbol.descriptor.key.epoch) ||
          frame.sequence < symbol.descriptor.first_sequence ||
          frame.sequence >= *end) {
        return fail<XorRecoveredFrame>(
            ErrorCode::invalid_argument,
            "XOR recovery observed frame is outside the exact group");
      }
      const auto index = static_cast<std::size_t>(
          frame.sequence - symbol.descriptor.first_sequence);
      if (present[index] != 0) {
        return fail<XorRecoveredFrame>(
            ErrorCode::invalid_argument,
            "XOR recovery observed group contains a duplicate sequence");
      }

      auto encoded = encode_multiplex_frame(frame, limits.multiplex);
      if (!encoded) return encoded.error();
      const auto encoded_size = static_cast<std::uint64_t>(encoded->size());
      if (!checked_add(aggregate_observed_bytes, encoded_size,
                       &aggregate_observed_bytes) ||
          aggregate_observed_bytes >
              limits.maximum_total_encoded_source_bytes) {
        return fail<XorRecoveredFrame>(
            ErrorCode::resource_exhausted,
            "XOR recovery aggregate observed-byte limit exceeded");
      }
      if (encoded_size != symbol.encoded_frame_sizes[index] ||
          sha256(*encoded) != symbol.encoded_frame_hashes[index]) {
        return fail<XorRecoveredFrame>(
            ErrorCode::protocol,
            "XOR recovery observed frame conflicts with its commitment");
      }
      for (std::size_t offset = 0; offset < encoded->size(); ++offset) {
        recovered_bytes[offset] ^= (*encoded)[offset];
      }
      present[index] = 1;
    }

    std::size_t missing_index = source_count;
    for (std::size_t index = 0; index < present.size(); ++index) {
      if (present[index] == 0) {
        if (missing_index != source_count) {
          return fail<XorRecoveredFrame>(
              ErrorCode::invalid_argument,
              "XOR recovery has more than one missing source frame");
        }
        missing_index = index;
      }
    }
    if (missing_index == source_count) {
      return fail<XorRecoveredFrame>(
          ErrorCode::invalid_argument,
          "XOR recovery has no missing source frame");
    }

    recovered_bytes.resize(static_cast<std::size_t>(
        symbol.encoded_frame_sizes[missing_index]));
    const auto recovered_hash = sha256(recovered_bytes);
    if (recovered_hash != symbol.encoded_frame_hashes[missing_index]) {
      return fail<XorRecoveredFrame>(
          ErrorCode::protocol,
          "XOR recovered frame SHA-256 does not match its commitment");
    }

    MultiplexDecoder decoder{limits.multiplex};
    auto decoded = decoder.push(recovered_bytes);
    if (!decoded) return decoded.error();
    if (decoded->size() != 1) {
      return fail<XorRecoveredFrame>(
          ErrorCode::protocol,
          "XOR recovery did not produce exactly one CMX1 frame");
    }
    auto finished = decoder.finish();
    if (!finished) return finished.error();
    if (decoder.buffered_bytes() != 0) {
      return fail<XorRecoveredFrame>(
          ErrorCode::protocol,
          "XOR recovered CMX1 decoder retained trailing bytes");
    }

    const auto expected_sequence =
        symbol.descriptor.first_sequence + missing_index;
    const auto& frame = decoded->front();
    if (frame.stream != symbol.descriptor.key.stream ||
        !same_epoch(frame.epoch, symbol.descriptor.key.epoch) ||
        frame.sequence != expected_sequence) {
      return fail<XorRecoveredFrame>(
          ErrorCode::protocol,
          "XOR recovered CMX1 frame does not match the missing group slot");
    }

    XorRecoveredFrame result;
    result.frame = std::move(decoded->front());
    result.encoded_frame = std::move(recovered_bytes);
    result.encoded_frame_hash = recovered_hash;
    return result;
  } catch (const std::bad_alloc&) {
    return fail<XorRecoveredFrame>(ErrorCode::resource_exhausted,
                                    "XOR recovery allocation failed");
  }
}

}  // namespace codec
