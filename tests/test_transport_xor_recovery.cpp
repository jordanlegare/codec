#include "test.hpp"

#include <codec/integrity.hpp>
#include <codec/xor_recovery.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

codec::MultiplexFrame source_frame(std::uint64_t sequence,
                                   std::size_t payload_size) {
  codec::MultiplexFrame frame;
  frame.stream = codec::derive_stream_id("recovery/xor/e4");
  frame.sequence = sequence;
  frame.epoch = {.connection = 7, .format = 3};
  frame.clock = {
      .monotonic_ns = 1000 + static_cast<std::int64_t>(sequence),
      .observed_utc_ns = 2000 + static_cast<std::int64_t>(sequence),
      .observed_utc_uncertainty_ns = 9,
      .source_timestamp = static_cast<std::int64_t>(sequence * 480),
      .source_timebase_numerator = 1,
      .source_timebase_denominator = 48000,
  };
  frame.start_ns = static_cast<std::int64_t>(sequence * 10);
  frame.end_ns = frame.start_ns + 10;
  frame.payload.resize(payload_size);
  for (std::size_t index = 0; index < payload_size; ++index) {
    frame.payload[index] = static_cast<std::byte>(
        static_cast<unsigned char>((sequence + index * 17U) & 0xffU));
  }
  return frame;
}

std::vector<codec::MultiplexFrame> source_group() {
  return {
      source_frame(40, 0),
      source_frame(41, 1),
      source_frame(42, 17),
      source_frame(43, 257),
  };
}

codec::RecoveryGroupDescriptor descriptor_for(
    const std::vector<codec::MultiplexFrame>& frames) {
  codec::RecoveryGroupDescriptor descriptor;
  descriptor.key.stream = frames.front().stream;
  descriptor.key.epoch = frames.front().epoch;
  descriptor.key.group_sequence = 12;
  descriptor.first_sequence = frames.front().sequence;
  descriptor.source_count = frames.size();
  return descriptor;
}

std::vector<std::byte> exact_encoding(const codec::MultiplexFrame& frame) {
  auto encoded = codec::encode_multiplex_frame(frame);
  if (!encoded) throw std::runtime_error("test CMX1 frame failed to encode");
  return *encoded;
}

std::uint16_t get_u16(std::span<const std::byte> bytes,
                      std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(
           static_cast<std::uint8_t>(bytes[offset + 1]))
       << 8U));
}

std::uint32_t get_u32(std::span<const std::byte> bytes,
                      std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(
                 static_cast<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

std::uint64_t get_u64(std::span<const std::byte> bytes,
                      std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(
                 static_cast<std::uint8_t>(bytes[offset + index]))
             << (index * 8U);
  }
  return value;
}

void put_u16(std::span<std::byte> bytes, std::size_t offset,
             std::uint16_t value) {
  for (std::size_t index = 0; index < 2; ++index) {
    bytes[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

void put_u32(std::span<std::byte> bytes, std::size_t offset,
             std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

void put_u64(std::span<std::byte> bytes, std::size_t offset,
             std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8U;
  }
}

void rehash_xrf1(std::vector<std::byte>& bytes) {
  if (bytes.size() < codec::Sha256{}.size()) {
    throw std::runtime_error("test XRF1 bytes are too short to rehash");
  }
  const auto digest_size = codec::Sha256{}.size();
  const auto digest = codec::sha256(
      std::span<const std::byte>{bytes}.first(bytes.size() - digest_size));
  for (std::size_t index = 0; index < digest.size(); ++index) {
    bytes[bytes.size() - digest_size + index] =
        static_cast<std::byte>(digest[index]);
  }
}

codec::XorRepairSymbol repair_symbol() {
  const auto frames = source_group();
  auto symbol = codec::create_xor_repair_symbol(descriptor_for(frames),
                                                 frames);
  if (!symbol) throw std::runtime_error("test XOR symbol creation failed");
  return *symbol;
}

std::vector<std::byte> encoded_symbol() {
  auto encoded = codec::encode_xor_repair_symbol(repair_symbol());
  if (!encoded) throw std::runtime_error("test XRF1 encoding failed");
  return *encoded;
}

void expect_decode_error(std::vector<std::byte> bytes,
                         codec::ErrorCode expected,
                         bool rehash = false) {
  if (rehash) rehash_xrf1(bytes);
  auto decoded = codec::decode_xor_repair_symbol(bytes);
  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error().code, expected);
}

std::vector<codec::MultiplexFrame> observed_without(
    const std::vector<codec::MultiplexFrame>& frames,
    std::size_t missing) {
  std::vector<codec::MultiplexFrame> observed;
  observed.reserve(frames.size() - 1);
  for (std::size_t index = 0; index < frames.size(); ++index) {
    if (index != missing) observed.push_back(frames[index]);
  }
  return observed;
}

void expect_same_frame(const codec::MultiplexFrame& actual,
                       const codec::MultiplexFrame& expected) {
  EXPECT_EQ(actual.stream, expected.stream);
  EXPECT_EQ(actual.sequence, expected.sequence);
  EXPECT_EQ(actual.epoch.connection, expected.epoch.connection);
  EXPECT_EQ(actual.epoch.format, expected.epoch.format);
  EXPECT_EQ(actual.clock.monotonic_ns, expected.clock.monotonic_ns);
  EXPECT_EQ(actual.clock.observed_utc_ns, expected.clock.observed_utc_ns);
  EXPECT_EQ(actual.clock.observed_utc_uncertainty_ns,
            expected.clock.observed_utc_uncertainty_ns);
  EXPECT_EQ(actual.clock.source_timestamp, expected.clock.source_timestamp);
  EXPECT_EQ(actual.clock.source_timebase_numerator,
            expected.clock.source_timebase_numerator);
  EXPECT_EQ(actual.clock.source_timebase_denominator,
            expected.clock.source_timebase_denominator);
  EXPECT_EQ(actual.start_ns, expected.start_ns);
  EXPECT_EQ(actual.end_ns, expected.end_ns);
  EXPECT_EQ(actual.payload, expected.payload);
}

}  // namespace

TEST(transport_xor_recovery_round_trips_symbol_and_one_erasure) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);

  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  auto encoded = codec::encode_xor_repair_symbol(*symbol);
  EXPECT_TRUE(encoded);
  auto decoded = codec::decode_xor_repair_symbol(*encoded);
  EXPECT_TRUE(decoded);

  const std::vector<codec::MultiplexFrame> observed{
      frames[0], frames[2], frames[3]};
  auto recovered = codec::recover_xor_single_erasure(*decoded, observed);
  EXPECT_TRUE(recovered);
  expect_same_frame(recovered->frame, frames[1]);
  EXPECT_EQ(recovered->encoded_frame, exact_encoding(frames[1]));
  EXPECT_EQ(recovered->encoded_frame_hash,
            codec::sha256(recovered->encoded_frame));
}

TEST(transport_xor_recovery_create_canonicalizes_unordered_frames) {
  auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  std::swap(frames[0], frames[3]);
  std::swap(frames[1], frames[2]);

  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  EXPECT_EQ(symbol->encoded_frame_sizes.size(), std::size_t{4});
  EXPECT_EQ(symbol->encoded_frame_hashes.size(), std::size_t{4});

  const auto ordered = source_group();
  EXPECT_EQ(symbol->parity.size(), exact_encoding(ordered[3]).size());
  for (std::size_t index = 0; index < ordered.size(); ++index) {
    const auto bytes = exact_encoding(ordered[index]);
    EXPECT_EQ(symbol->encoded_frame_sizes[index], bytes.size());
    EXPECT_EQ(symbol->encoded_frame_hashes[index], codec::sha256(bytes));
  }
}

TEST(transport_xor_recovery_create_rejects_invalid_group_geometry) {
  const auto frames = source_group();

  auto one = descriptor_for(frames);
  one.source_count = 1;
  auto one_result = codec::create_xor_repair_symbol(
      one, std::span<const codec::MultiplexFrame>{frames}.first(1));
  EXPECT_FALSE(one_result);
  EXPECT_EQ(one_result.error().code, codec::ErrorCode::invalid_argument);

  auto mismatch = descriptor_for(frames);
  auto mismatch_result = codec::create_xor_repair_symbol(
      mismatch, std::span<const codec::MultiplexFrame>{frames}.first(3));
  EXPECT_FALSE(mismatch_result);
  EXPECT_EQ(mismatch_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto overflow = descriptor_for(frames);
  overflow.first_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
  auto overflow_result = codec::create_xor_repair_symbol(overflow, frames);
  EXPECT_FALSE(overflow_result);
  EXPECT_EQ(overflow_result.error().code,
            codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_create_rejects_member_namespace_defects) {
  const auto valid = source_group();
  const auto descriptor = descriptor_for(valid);

  auto duplicate = valid;
  duplicate[1] = duplicate[0];
  auto duplicate_result =
      codec::create_xor_repair_symbol(descriptor, duplicate);
  EXPECT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_sequence = valid;
  wrong_sequence[1].sequence = 99;
  auto sequence_result =
      codec::create_xor_repair_symbol(descriptor, wrong_sequence);
  EXPECT_FALSE(sequence_result);
  EXPECT_EQ(sequence_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_stream = valid;
  wrong_stream[1].stream = codec::derive_stream_id("recovery/xor/other");
  auto stream_result =
      codec::create_xor_repair_symbol(descriptor, wrong_stream);
  EXPECT_FALSE(stream_result);
  EXPECT_EQ(stream_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_connection = valid;
  ++wrong_connection[1].epoch.connection;
  auto connection_result =
      codec::create_xor_repair_symbol(descriptor, wrong_connection);
  EXPECT_FALSE(connection_result);
  EXPECT_EQ(connection_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_format = valid;
  ++wrong_format[1].epoch.format;
  auto format_result =
      codec::create_xor_repair_symbol(descriptor, wrong_format);
  EXPECT_FALSE(format_result);
  EXPECT_EQ(format_result.error().code,
            codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_create_rejects_invalid_cmx1_member) {
  const auto valid = source_group();
  const auto descriptor = descriptor_for(valid);

  auto inverted = valid;
  inverted[1].end_ns = inverted[1].start_ns - 1;
  auto inverted_result =
      codec::create_xor_repair_symbol(descriptor, inverted);
  EXPECT_FALSE(inverted_result);
  EXPECT_EQ(inverted_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto bad_timebase = valid;
  bad_timebase[1].clock.source_timebase_denominator = 0;
  auto timebase_result =
      codec::create_xor_repair_symbol(descriptor, bad_timebase);
  EXPECT_FALSE(timebase_result);
  EXPECT_EQ(timebase_result.error().code,
            codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_create_enforces_resource_limits) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);

  codec::XorRepairLimits count_limits;
  count_limits.maximum_source_frames = 3;
  auto count_result =
      codec::create_xor_repair_symbol(descriptor, frames, count_limits);
  EXPECT_FALSE(count_result);
  EXPECT_EQ(count_result.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::XorRepairLimits frame_limits;
  frame_limits.multiplex.maximum_payload_bytes = 16;
  frame_limits.multiplex.maximum_buffered_bytes = 180;
  auto frame_result =
      codec::create_xor_repair_symbol(descriptor, frames, frame_limits);
  EXPECT_FALSE(frame_result);
  EXPECT_EQ(frame_result.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::XorRepairLimits aggregate_limits;
  aggregate_limits.maximum_total_encoded_source_bytes = 600;
  auto aggregate_result =
      codec::create_xor_repair_symbol(descriptor, frames, aggregate_limits);
  EXPECT_FALSE(aggregate_result);
  EXPECT_EQ(aggregate_result.error().code,
            codec::ErrorCode::resource_exhausted);
}

TEST(transport_xor_recovery_create_rejects_zero_limits) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);

  codec::XorRepairLimits limits;
  limits.maximum_symbol_bytes = 0;
  auto result = codec::create_xor_repair_symbol(descriptor, frames, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_xrf1_encoding_is_deterministic_and_versioned) {
  const auto symbol = repair_symbol();
  auto first = codec::encode_xor_repair_symbol(symbol);
  auto second = codec::encode_xor_repair_symbol(symbol);
  EXPECT_TRUE(first);
  EXPECT_TRUE(second);
  EXPECT_EQ(*first, *second);
  EXPECT_EQ((*first)[0], std::byte{'X'});
  EXPECT_EQ((*first)[1], std::byte{'R'});
  EXPECT_EQ((*first)[2], std::byte{'F'});
  EXPECT_EQ((*first)[3], std::byte{'1'});
  EXPECT_EQ(get_u16(*first, 4), codec::xor_repair_symbol_version);
  EXPECT_EQ(get_u16(*first, 6), std::uint16_t{0});
  EXPECT_EQ(get_u32(*first, 8), std::uint32_t{92});
  EXPECT_EQ(get_u64(*first, 12), first->size());
  EXPECT_EQ(get_u32(*first, 84), std::uint32_t{40});
  EXPECT_EQ(get_u32(*first, 88), std::uint32_t{0});

  auto decoded = codec::decode_xor_repair_symbol(*first);
  EXPECT_TRUE(decoded);
  EXPECT_EQ(decoded->descriptor.key, symbol.descriptor.key);
  EXPECT_EQ(decoded->descriptor.first_sequence,
            symbol.descriptor.first_sequence);
  EXPECT_EQ(decoded->descriptor.source_count,
            symbol.descriptor.source_count);
  EXPECT_EQ(decoded->encoded_frame_sizes, symbol.encoded_frame_sizes);
  EXPECT_EQ(decoded->encoded_frame_hashes, symbol.encoded_frame_hashes);
  EXPECT_EQ(decoded->parity, symbol.parity);

  const auto digest_size = codec::Sha256{}.size();
  const auto digest = codec::sha256(
      std::span<const std::byte>{*first}.first(first->size() - digest_size));
  for (std::size_t index = 0; index < digest.size(); ++index) {
    EXPECT_EQ((*first)[first->size() - digest_size + index],
              static_cast<std::byte>(digest[index]));
  }
}

TEST(transport_xor_recovery_xrf1_decoder_rejects_header_variants) {
  auto bytes = encoded_symbol();
  bytes[0] = std::byte{'Y'};
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol);

  bytes = encoded_symbol();
  put_u16(bytes, 4, 2);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u16(bytes, 6, 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u32(bytes, 8, 91);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u32(bytes, 84, 39);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u32(bytes, 88, 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);
}

TEST(transport_xor_recovery_xrf1_decoder_rejects_noncanonical_geometry) {
  auto bytes = encoded_symbol();
  put_u64(bytes, 12, bytes.size() - 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 68, 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 60, std::numeric_limits<std::uint64_t>::max() - 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 76, 0);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 92, 0);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 92, 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);

  bytes = encoded_symbol();
  put_u64(bytes, 92, get_u64(bytes, 76) + 1);
  expect_decode_error(std::move(bytes), codec::ErrorCode::protocol, true);
}

TEST(transport_xor_recovery_xrf1_decoder_rejects_truncation_and_trailing_bytes) {
  auto truncated = encoded_symbol();
  truncated.pop_back();
  expect_decode_error(std::move(truncated), codec::ErrorCode::protocol);

  auto trailing = encoded_symbol();
  trailing.push_back(std::byte{0});
  expect_decode_error(std::move(trailing), codec::ErrorCode::protocol);

  auto corrupt = encoded_symbol();
  corrupt[92] ^= std::byte{1};
  expect_decode_error(std::move(corrupt), codec::ErrorCode::protocol);
}

TEST(transport_xor_recovery_xrf1_decoder_enforces_resource_limits) {
  const auto bytes = encoded_symbol();

  codec::XorRepairLimits symbol_limit;
  symbol_limit.maximum_symbol_bytes = bytes.size() - 1;
  auto symbol_result =
      codec::decode_xor_repair_symbol(bytes, symbol_limit);
  EXPECT_FALSE(symbol_result);
  EXPECT_EQ(symbol_result.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::XorRepairLimits count_limit;
  count_limit.maximum_source_frames = 3;
  auto count_result = codec::decode_xor_repair_symbol(bytes, count_limit);
  EXPECT_FALSE(count_result);
  EXPECT_EQ(count_result.error().code,
            codec::ErrorCode::resource_exhausted);

  codec::XorRepairLimits aggregate_limit;
  aggregate_limit.maximum_total_encoded_source_bytes = 600;
  auto aggregate_result =
      codec::decode_xor_repair_symbol(bytes, aggregate_limit);
  EXPECT_FALSE(aggregate_result);
  EXPECT_EQ(aggregate_result.error().code,
            codec::ErrorCode::resource_exhausted);
}

TEST(transport_xor_recovery_xrf1_encoder_rejects_invalid_in_memory_symbols) {
  auto missing_size = repair_symbol();
  missing_size.encoded_frame_sizes.pop_back();
  auto size_result = codec::encode_xor_repair_symbol(missing_size);
  EXPECT_FALSE(size_result);
  EXPECT_EQ(size_result.error().code, codec::ErrorCode::invalid_argument);

  auto missing_hash = repair_symbol();
  missing_hash.encoded_frame_hashes.pop_back();
  auto hash_result = codec::encode_xor_repair_symbol(missing_hash);
  EXPECT_FALSE(hash_result);
  EXPECT_EQ(hash_result.error().code, codec::ErrorCode::invalid_argument);

  auto wrong_parity = repair_symbol();
  wrong_parity.parity.pop_back();
  auto parity_result = codec::encode_xor_repair_symbol(wrong_parity);
  EXPECT_FALSE(parity_result);
  EXPECT_EQ(parity_result.error().code,
            codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_recovers_every_variable_length_position_exactly) {
  const auto frames = source_group();
  const auto symbol = repair_symbol();
  for (std::size_t missing = 0; missing < frames.size(); ++missing) {
    auto observed = observed_without(frames, missing);
    if ((missing % 2U) != 0U) {
      std::reverse(observed.begin(), observed.end());
    }
    auto recovered =
        codec::recover_xor_single_erasure(symbol, observed);
    EXPECT_TRUE(recovered);
    const auto expected_bytes = exact_encoding(frames[missing]);
    EXPECT_EQ(recovered->encoded_frame, expected_bytes);
    EXPECT_EQ(recovered->encoded_frame_hash,
              codec::sha256(expected_bytes));
    expect_same_frame(recovered->frame, frames[missing]);
  }
}

TEST(transport_xor_recovery_rejects_zero_or_multiple_erasures) {
  const auto frames = source_group();
  const auto symbol = repair_symbol();

  auto none_missing = codec::recover_xor_single_erasure(symbol, frames);
  EXPECT_FALSE(none_missing);
  EXPECT_EQ(none_missing.error().code,
            codec::ErrorCode::invalid_argument);

  const std::vector<codec::MultiplexFrame> two_missing{
      frames[0], frames[3]};
  auto multiple = codec::recover_xor_single_erasure(symbol, two_missing);
  EXPECT_FALSE(multiple);
  EXPECT_EQ(multiple.error().code, codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_rejects_invalid_observed_members) {
  const auto frames = source_group();
  const auto symbol = repair_symbol();

  auto duplicate = observed_without(frames, 1);
  duplicate[1] = duplicate[0];
  auto duplicate_result =
      codec::recover_xor_single_erasure(symbol, duplicate);
  EXPECT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_stream = observed_without(frames, 1);
  wrong_stream[0].stream = codec::derive_stream_id("recovery/xor/other");
  auto stream_result =
      codec::recover_xor_single_erasure(symbol, wrong_stream);
  EXPECT_FALSE(stream_result);
  EXPECT_EQ(stream_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_connection = observed_without(frames, 1);
  ++wrong_connection[0].epoch.connection;
  auto connection_result =
      codec::recover_xor_single_erasure(symbol, wrong_connection);
  EXPECT_FALSE(connection_result);
  EXPECT_EQ(connection_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_format = observed_without(frames, 1);
  ++wrong_format[0].epoch.format;
  auto format_result =
      codec::recover_xor_single_erasure(symbol, wrong_format);
  EXPECT_FALSE(format_result);
  EXPECT_EQ(format_result.error().code,
            codec::ErrorCode::invalid_argument);

  auto wrong_sequence = observed_without(frames, 1);
  wrong_sequence[0].sequence = 99;
  auto sequence_result =
      codec::recover_xor_single_erasure(symbol, wrong_sequence);
  EXPECT_FALSE(sequence_result);
  EXPECT_EQ(sequence_result.error().code,
            codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_rejects_conflicting_observed_content) {
  const auto frames = source_group();
  const auto symbol = repair_symbol();
  auto observed = observed_without(frames, 1);
  observed[0].payload.push_back(std::byte{0x55});

  auto result = codec::recover_xor_single_erasure(symbol, observed);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
}

TEST(transport_xor_recovery_rejects_invalid_observed_cmx1_metadata) {
  const auto frames = source_group();
  const auto symbol = repair_symbol();
  auto observed = observed_without(frames, 1);
  observed[0].clock.source_timebase_denominator = 0;

  auto result = codec::recover_xor_single_erasure(symbol, observed);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
}

TEST(transport_xor_recovery_rejects_corrupt_symbol_commitments) {
  const auto frames = source_group();
  const auto observed = observed_without(frames, 1);

  auto bad_parity = repair_symbol();
  bad_parity.parity[0] ^= std::byte{1};
  auto parity_result =
      codec::recover_xor_single_erasure(bad_parity, observed);
  EXPECT_FALSE(parity_result);
  EXPECT_EQ(parity_result.error().code, codec::ErrorCode::protocol);

  auto bad_size = repair_symbol();
  --bad_size.encoded_frame_sizes[1];
  auto size_result =
      codec::recover_xor_single_erasure(bad_size, observed);
  EXPECT_FALSE(size_result);
  EXPECT_EQ(size_result.error().code, codec::ErrorCode::protocol);

  auto bad_hash = repair_symbol();
  bad_hash.encoded_frame_hashes[1][0] ^= 1U;
  auto hash_result =
      codec::recover_xor_single_erasure(bad_hash, observed);
  EXPECT_FALSE(hash_result);
  EXPECT_EQ(hash_result.error().code, codec::ErrorCode::protocol);
}

TEST(transport_xor_recovery_rejects_hash_valid_non_cmx1_reconstruction) {
  const auto frames = source_group();
  const auto observed = observed_without(frames, 1);
  auto symbol = repair_symbol();
  symbol.parity[0] ^= std::byte{1};

  auto corrupted_missing = exact_encoding(frames[1]);
  corrupted_missing[0] ^= std::byte{1};
  symbol.encoded_frame_hashes[1] = codec::sha256(corrupted_missing);

  auto result = codec::recover_xor_single_erasure(symbol, observed);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
}

TEST(transport_xor_recovery_enforces_recovery_working_byte_limit) {
  const auto frames = source_group();
  const auto observed = observed_without(frames, 1);
  const auto symbol = repair_symbol();
  codec::XorRepairLimits limits;
  limits.maximum_total_encoded_source_bytes = 600;

  auto result =
      codec::recover_xor_single_erasure(symbol, observed, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
}

TEST(transport_xor_recovery_matches_sealed_e2_missing_range) {
  const auto frames = source_group();
  const auto descriptor = descriptor_for(frames);
  codec::RecoveryGroupTracker tracker;
  EXPECT_TRUE(tracker.begin(descriptor));
  for (const auto& frame : observed_without(frames, 1)) {
    EXPECT_TRUE(tracker.observe(frame));
  }
  auto sealed = tracker.seal(descriptor.key);
  EXPECT_TRUE(sealed);
  EXPECT_EQ(sealed->state, codec::RecoveryGroupState::sealed_incomplete);
  EXPECT_EQ(sealed->missing_ranges.size(), std::size_t{1});
  EXPECT_EQ(sealed->missing_ranges[0].begin, std::uint64_t{41});
  EXPECT_EQ(sealed->missing_ranges[0].end, std::uint64_t{42});

  auto symbol = codec::create_xor_repair_symbol(descriptor, frames);
  EXPECT_TRUE(symbol);
  auto recovered = codec::recover_xor_single_erasure(
      *symbol, observed_without(frames, 1));
  EXPECT_TRUE(recovered);
  EXPECT_EQ(recovered->frame.sequence, std::uint64_t{41});
  EXPECT_EQ(recovered->encoded_frame, exact_encoding(frames[1]));
}
