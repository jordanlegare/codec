#include "internal.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <type_traits>

namespace codec::detail {
namespace {

constexpr std::size_t timing_payload_size = 120;
constexpr std::size_t gap_payload_size = 40;

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

bool has_prefix(std::span<const std::byte> payload, const char* magic) {
  return std::memcmp(payload.data(), magic, 4) == 0;
}

}  // namespace

std::vector<std::byte> encode_stream_timing(const StreamTiming& timing) {
  std::vector<std::byte> output(timing_payload_size);
  std::memcpy(output.data(), "STM1", 4);
  put_le<std::uint16_t>(output, 4, 1);
  put_le<std::uint16_t>(output, 6, 0);
  put_le<std::uint64_t>(output, 8, timing.sequence);
  put_le<std::uint64_t>(output, 16, timing.epoch.connection);
  put_le<std::uint64_t>(output, 24, timing.epoch.format);
  put_le<std::int64_t>(output, 32, timing.clock.monotonic_ns);
  put_le<std::int64_t>(output, 40, timing.clock.observed_utc_ns);
  put_le<std::uint64_t>(output, 48,
                        timing.clock.observed_utc_uncertainty_ns);
  put_le<std::int64_t>(output, 56, timing.clock.source_timestamp);
  put_le<std::int64_t>(output, 64,
                       timing.clock.source_timebase_numerator);
  put_le<std::int64_t>(output, 72,
                       timing.clock.source_timebase_denominator);
  put_le<std::uint64_t>(output, 80, timing.source_record_sequence);
  for (std::size_t index = 0; index < timing.source_record_hash.size();
       ++index) {
    output[88 + index] =
        static_cast<std::byte>(timing.source_record_hash[index]);
  }
  return output;
}

Result<StreamTiming> decode_stream_timing(
    std::span<const std::byte> payload, const StreamId& stream) {
  if (payload.size() != timing_payload_size || !has_prefix(payload, "STM1") ||
      get_le<std::uint16_t>(payload, 4) != 1 ||
      get_le<std::uint16_t>(payload, 6) != 0) {
    return fail<StreamTiming>(ErrorCode::archive_corrupt,
                              "invalid stream timing record");
  }
  StreamTiming timing;
  timing.stream = stream;
  timing.sequence = get_le<std::uint64_t>(payload, 8);
  timing.epoch.connection = get_le<std::uint64_t>(payload, 16);
  timing.epoch.format = get_le<std::uint64_t>(payload, 24);
  timing.clock.monotonic_ns = get_le<std::int64_t>(payload, 32);
  timing.clock.observed_utc_ns = get_le<std::int64_t>(payload, 40);
  timing.clock.observed_utc_uncertainty_ns =
      get_le<std::uint64_t>(payload, 48);
  timing.clock.source_timestamp = get_le<std::int64_t>(payload, 56);
  timing.clock.source_timebase_numerator =
      get_le<std::int64_t>(payload, 64);
  timing.clock.source_timebase_denominator =
      get_le<std::int64_t>(payload, 72);
  timing.source_record_sequence = get_le<std::uint64_t>(payload, 80);
  for (std::size_t index = 0; index < timing.source_record_hash.size();
       ++index) {
    timing.source_record_hash[index] =
        static_cast<std::uint8_t>(payload[88 + index]);
  }
  return timing;
}

std::vector<std::byte> encode_stream_gap(const StreamGap& gap) {
  std::vector<std::byte> output(gap_payload_size);
  std::memcpy(output.data(), "SGP1", 4);
  put_le<std::uint16_t>(output, 4, 1);
  put_le<std::uint16_t>(output, 6, 0);
  put_le<std::uint64_t>(output, 8, gap.first_sequence);
  put_le<std::uint64_t>(output, 16, gap.missing_count);
  put_le<std::uint64_t>(output, 24, gap.epoch.connection);
  put_le<std::uint64_t>(output, 32, gap.epoch.format);
  return output;
}

Result<StreamGap> decode_stream_gap(std::span<const std::byte> payload,
                                    const StreamId& stream) {
  if (payload.size() != gap_payload_size || !has_prefix(payload, "SGP1") ||
      get_le<std::uint16_t>(payload, 4) != 1 ||
      get_le<std::uint16_t>(payload, 6) != 0) {
    return fail<StreamGap>(ErrorCode::archive_corrupt,
                           "invalid stream gap record");
  }
  StreamGap gap;
  gap.stream = stream;
  gap.first_sequence = get_le<std::uint64_t>(payload, 8);
  gap.missing_count = get_le<std::uint64_t>(payload, 16);
  gap.epoch.connection = get_le<std::uint64_t>(payload, 24);
  gap.epoch.format = get_le<std::uint64_t>(payload, 32);
  return gap;
}

Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamTiming& timing) {
  if (timing.clock.source_timebase_numerator <= 0 ||
      timing.clock.source_timebase_denominator <= 0) {
    return fail(ErrorCode::invalid_argument,
                "stream timing source timebase must be positive");
  }
  if (timing.sequence != state.next_sequence) {
    return fail(ErrorCode::invalid_argument,
                "stream timing sequence is not contiguous");
  }
  if (timing.sequence == std::numeric_limits<std::uint64_t>::max()) {
    return fail(ErrorCode::invalid_argument,
                "stream timing sequence would overflow");
  }
  if (state.initialized &&
      (timing.epoch.connection < state.epoch.connection ||
       timing.epoch.format < state.epoch.format)) {
    return fail(ErrorCode::invalid_argument,
                "stream timing epoch regressed");
  }
  const bool same_connection =
      state.initialized &&
      timing.epoch.connection == state.epoch.connection;
  if (same_connection && state.has_monotonic &&
      timing.clock.monotonic_ns < state.last_monotonic_ns) {
    return fail(ErrorCode::invalid_argument,
                "stream monotonic clock regressed within a connection");
  }

  state.next_sequence = timing.sequence + 1;
  state.epoch = timing.epoch;
  state.last_monotonic_ns = timing.clock.monotonic_ns;
  state.initialized = true;
  state.has_monotonic = true;
  return {};
}

Result<void> validate_and_advance(StreamContinuityState& state,
                                  const StreamGap& gap) {
  if (gap.missing_count == 0) {
    return fail(ErrorCode::invalid_argument,
                "stream gap must contain at least one sequence");
  }
  if (gap.first_sequence != state.next_sequence) {
    return fail(ErrorCode::invalid_argument,
                "stream gap does not start at the next sequence");
  }
  if (gap.missing_count >
      std::numeric_limits<std::uint64_t>::max() - gap.first_sequence) {
    return fail(ErrorCode::invalid_argument,
                "stream gap sequence range would overflow");
  }
  if (state.initialized &&
      (gap.epoch.connection < state.epoch.connection ||
       gap.epoch.format < state.epoch.format)) {
    return fail(ErrorCode::invalid_argument, "stream gap epoch regressed");
  }

  if (state.initialized && gap.epoch.connection > state.epoch.connection) {
    state.has_monotonic = false;
  }
  state.next_sequence = gap.first_sequence + gap.missing_count;
  state.epoch = gap.epoch;
  state.initialized = true;
  return {};
}

}  // namespace codec::detail
