#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

namespace codec {

struct StreamId {
  std::array<std::uint8_t, 16> bytes{};
  auto operator<=>(const StreamId&) const = default;
};

StreamId derive_stream_id(std::string_view value);
std::string to_string(const StreamId& value);

enum class StreamType : std::uint16_t {
  opaque = 0,
  audio = 1,
  video = 2,
  image = 3,
  telemetry = 4,
  sensor = 5,
  navigation = 6,
  document_event = 7,
  network_system = 8,
  model_artifact = 9,
  identity_evidence = 10,
  recovery = 11,
  audit = 12,
  authorization_policy = 13,
  custom = 0xfffe,
};

enum class TruthClass : std::uint8_t {
  source_exact = 0,
  state_exact = 1,
  derived = 2,
};

struct StreamEpoch {
  std::uint64_t connection{};
  std::uint64_t format{};
};

struct StreamClock {
  std::int64_t monotonic_ns{};
  std::int64_t observed_utc_ns{};
  std::uint64_t observed_utc_uncertainty_ns{};
  std::int64_t source_timestamp{};
  std::int64_t source_timebase_numerator{1};
  std::int64_t source_timebase_denominator{1};
};

struct StreamDescriptor {
  StreamId id{};
  StreamType type{StreamType::opaque};
  std::string label;
  std::string source_id;
  std::string payload_type;
};

}  // namespace codec
