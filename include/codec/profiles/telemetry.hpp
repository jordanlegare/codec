#pragma once

#include <codec/archive.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codec::profiles::telemetry {

inline constexpr RecordTypeCode telemetry_profile_descriptor_record_type = 0x0110;
inline constexpr RecordTypeCode telemetry_sample_state_record_type = 0x0111;

enum class TelemetryScalarType : std::uint8_t {
  signed_integer = 1,
  unsigned_integer = 2,
  float64_bits = 3,
  boolean = 4,
};

struct TelemetryValue {
  TelemetryScalarType scalar_type{TelemetryScalarType::unsigned_integer};
  std::uint64_t raw_bits{};

  static TelemetryValue from_signed(std::int64_t value) noexcept;
  static TelemetryValue from_unsigned(std::uint64_t value) noexcept;
  static TelemetryValue from_float64_bits(std::uint64_t bits) noexcept;
  static TelemetryValue from_boolean(bool value) noexcept;

  bool operator==(const TelemetryValue&) const = default;
};

struct TelemetryMetricDescriptor {
  std::string name;
  std::string unit;
  TelemetryScalarType scalar_type{TelemetryScalarType::unsigned_integer};
  auto operator<=>(const TelemetryMetricDescriptor&) const = default;
};

struct TelemetryProfileDescriptor {
  std::vector<TelemetryMetricDescriptor> metrics;
  bool operator==(const TelemetryProfileDescriptor&) const = default;
};

struct TelemetrySampleState {
  TelemetryProfileDescriptor descriptor;
  std::vector<TelemetryValue> values;
  bool operator==(const TelemetrySampleState&) const = default;
};

struct TelemetryDecodeLimits {
  std::uint32_t maximum_metrics{1024};
  std::uint32_t maximum_metric_name_bytes{128};
  std::uint32_t maximum_unit_bytes{64};
  std::uint64_t maximum_descriptor_bytes{1024ULL * 1024ULL};
  std::uint64_t maximum_state_bytes{16ULL * 1024ULL * 1024ULL};
};

Result<std::vector<std::byte>> encode_telemetry_profile_descriptor(
    const TelemetryProfileDescriptor& descriptor);
Result<TelemetryProfileDescriptor> decode_telemetry_profile_descriptor(
    std::span<const std::byte> payload, TelemetryDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_telemetry_sample_state(
    const TelemetrySampleState& state);
Result<TelemetrySampleState> decode_telemetry_sample_state(
    std::span<const std::byte> payload, TelemetryDecodeLimits limits = {});

struct TelemetrySampleQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{4096};
  std::uint64_t maximum_encoded_bytes{64ULL * 1024ULL * 1024ULL};
  TelemetryDecodeLimits decode_limits{};
};

struct VerifiedTelemetrySample {
  TelemetrySampleState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedTelemetrySample>> query_verified_telemetry_samples(
    const CodaArchive& archive, const TelemetrySampleQuery& query = {});

}  // namespace codec::profiles::telemetry
