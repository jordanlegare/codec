#include "test.hpp"

#include <codec/processing.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto characters = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(characters);
  return {raw.begin(), raw.end()};
}

codec::StreamId stream_id(std::uint8_t seed) {
  codec::StreamId id{};
  for (std::size_t index = 0; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return id;
}

codec::ExtractedRecord extracted_record(std::uint8_t seed,
                                        std::uint64_t sequence,
                                        codec::RecordTypeCode type,
                                        std::string_view payload_text) {
  auto payload = bytes(payload_text);
  codec::RecordInfo record;
  record.type = static_cast<codec::RecordType>(type);
  record.sequence = sequence;
  record.stream = stream_id(seed);
  record.start_ns = static_cast<std::int64_t>(sequence * 10);
  record.end_ns = record.start_ns + 10;
  record.payload_size = payload.size();
  record.hash = codec::sha256(payload);
  return codec::ExtractedRecord{record, std::move(payload)};
}

class TelemetryCsvExporter final : public codec::StreamExporter {
 public:
  std::string name() const override { return "telemetry-csv"; }

  codec::Result<codec::ExporterOutput> export_records(
      std::span<const codec::ExtractedRecord> inputs) override {
    if (inputs.size() != 2 ||
        inputs[0].payload != bytes("temperature=21.5") ||
        inputs[1].payload != bytes("temperature=21.6")) {
      return codec::Error{codec::ErrorCode::decode,
                          "unexpected telemetry export input", false};
    }
    return codec::ExporterOutput{
        .payload_type = "text/csv",
        .payload = bytes("temperature_c\n21.5\n21.6\n"),
    };
  }
};

class CountingExporter final : public codec::StreamExporter {
 public:
  explicit CountingExporter(codec::ExporterOutput output = {
                                .payload_type = "application/octet-stream",
                                .payload = bytes("ok"),
                            })
      : output_(std::move(output)) {}

  std::string name() const override { return "counting-exporter"; }

  codec::Result<codec::ExporterOutput> export_records(
      std::span<const codec::ExtractedRecord>) override {
    ++calls;
    return output_;
  }

  std::size_t calls{};

 private:
  codec::ExporterOutput output_;
};

class FailingExporter final : public codec::StreamExporter {
 public:
  std::string name() const override { return "failing-exporter"; }

  codec::Result<codec::ExporterOutput> export_records(
      std::span<const codec::ExtractedRecord>) override {
    return codec::Error{codec::ErrorCode::network,
                        "profile export backend unavailable", true};
  }
};

}  // namespace

TEST(generic_exporter_returns_exact_typed_bytes_and_ordered_support_links) {
  const auto first = extracted_record(
      60, 41, codec::record_type_code(codec::RecordType::source_bytes),
      "temperature=21.5");
  const auto second =
      extracted_record(61, 42, codec::RecordTypeCode{0x7700},
                       "temperature=21.6");
  const std::array inputs{first, second};
  TelemetryCsvExporter exporter;

  auto result = codec::invoke_exporter(exporter, inputs);

  EXPECT_TRUE(result);
  if (result) {
    EXPECT_EQ(result->payload_type, std::string{"text/csv"});
    EXPECT_EQ(result->payload, bytes("temperature_c\n21.5\n21.6\n"));
    EXPECT_EQ(result->supporting_records.size(), std::size_t{2});
    EXPECT_EQ(result->supporting_records[0].stream, first.record.stream);
    EXPECT_EQ(result->supporting_records[0].type,
              first.record.type_code());
    EXPECT_EQ(result->supporting_records[0].sequence,
              first.record.sequence);
    EXPECT_EQ(result->supporting_records[0].hash, first.record.hash);
    EXPECT_EQ(result->supporting_records[1].stream, second.record.stream);
    EXPECT_EQ(result->supporting_records[1].type,
              second.record.type_code());
    EXPECT_EQ(result->supporting_records[1].sequence,
              second.record.sequence);
    EXPECT_EQ(result->supporting_records[1].hash, second.record.hash);
  }
  EXPECT_EQ(exporter.name(), std::string{"telemetry-csv"});
}

TEST(generic_exporter_validates_inputs_and_limits_before_invocation) {
  CountingExporter exporter;
  auto empty = codec::invoke_exporter(
      exporter, std::span<const codec::ExtractedRecord>{});
  EXPECT_FALSE(empty);
  if (!empty) {
    EXPECT_EQ(empty.error().code, codec::ErrorCode::invalid_argument);
  }

  const auto one = extracted_record(
      70, 1, codec::record_type_code(codec::RecordType::source_bytes),
      "abcd");
  const std::array one_input{one};

  auto zero_count = codec::invoke_exporter(
      exporter, one_input,
      codec::ExporterRunLimits{
          .maximum_inputs = 0,
          .maximum_input_bytes = 8,
          .maximum_output_bytes = 8,
          .maximum_payload_type_bytes = 8,
      });
  EXPECT_FALSE(zero_count);
  if (!zero_count) {
    EXPECT_EQ(zero_count.error().code, codec::ErrorCode::invalid_argument);
  }

  auto zero_input_bytes = codec::invoke_exporter(
      exporter, one_input,
      codec::ExporterRunLimits{
          .maximum_inputs = 1,
          .maximum_input_bytes = 0,
          .maximum_output_bytes = 8,
          .maximum_payload_type_bytes = 8,
      });
  EXPECT_FALSE(zero_input_bytes);

  auto zero_output_bytes = codec::invoke_exporter(
      exporter, one_input,
      codec::ExporterRunLimits{
          .maximum_inputs = 1,
          .maximum_input_bytes = 8,
          .maximum_output_bytes = 0,
          .maximum_payload_type_bytes = 8,
      });
  EXPECT_FALSE(zero_output_bytes);

  auto zero_type_bytes = codec::invoke_exporter(
      exporter, one_input,
      codec::ExporterRunLimits{
          .maximum_inputs = 1,
          .maximum_input_bytes = 8,
          .maximum_output_bytes = 8,
          .maximum_payload_type_bytes = 0,
      });
  EXPECT_FALSE(zero_type_bytes);

  const std::array excessive_inputs{one, one, one};
  auto too_many = codec::invoke_exporter(
      exporter, excessive_inputs,
      codec::ExporterRunLimits{
          .maximum_inputs = 2,
          .maximum_input_bytes = 64,
          .maximum_output_bytes = 8,
          .maximum_payload_type_bytes = 64,
      });
  EXPECT_FALSE(too_many);
  if (!too_many) {
    EXPECT_EQ(too_many.error().code, codec::ErrorCode::resource_exhausted);
  }

  auto mismatched = one;
  ++mismatched.record.payload_size;
  const std::array mismatched_inputs{mismatched};
  auto wrong_size = codec::invoke_exporter(exporter, mismatched_inputs);
  EXPECT_FALSE(wrong_size);
  if (!wrong_size) {
    EXPECT_EQ(wrong_size.error().code, codec::ErrorCode::invalid_argument);
  }

  const std::array two_inputs{one, one};
  auto too_many_input_bytes = codec::invoke_exporter(
      exporter, two_inputs,
      codec::ExporterRunLimits{
          .maximum_inputs = 2,
          .maximum_input_bytes = 7,
          .maximum_output_bytes = 8,
          .maximum_payload_type_bytes = 64,
      });
  EXPECT_FALSE(too_many_input_bytes);
  if (!too_many_input_bytes) {
    EXPECT_EQ(too_many_input_bytes.error().code,
              codec::ErrorCode::resource_exhausted);
  }

  EXPECT_EQ(exporter.calls, std::size_t{0});
}

TEST(generic_exporter_validates_provider_output_bounds) {
  const std::array inputs{extracted_record(
      80, 3, codec::record_type_code(codec::RecordType::source_bytes),
      "input")};

  CountingExporter empty_type{codec::ExporterOutput{
      .payload_type = {},
      .payload = bytes("ok"),
  }};
  auto no_type = codec::invoke_exporter(empty_type, inputs);
  EXPECT_FALSE(no_type);
  if (!no_type) {
    EXPECT_EQ(no_type.error().code, codec::ErrorCode::invalid_argument);
  }

  CountingExporter long_type{codec::ExporterOutput{
      .payload_type = "text/csv",
      .payload = bytes("ok"),
  }};
  auto type_limit = codec::invoke_exporter(
      long_type, inputs,
      codec::ExporterRunLimits{
          .maximum_inputs = 1,
          .maximum_input_bytes = 64,
          .maximum_output_bytes = 64,
          .maximum_payload_type_bytes = 4,
      });
  EXPECT_FALSE(type_limit);
  if (!type_limit) {
    EXPECT_EQ(type_limit.error().code,
              codec::ErrorCode::resource_exhausted);
  }

  CountingExporter large_output{codec::ExporterOutput{
      .payload_type = "text/plain",
      .payload = bytes("abcd"),
  }};
  auto output_limit = codec::invoke_exporter(
      large_output, inputs,
      codec::ExporterRunLimits{
          .maximum_inputs = 1,
          .maximum_input_bytes = 64,
          .maximum_output_bytes = 3,
          .maximum_payload_type_bytes = 64,
      });
  EXPECT_FALSE(output_limit);
  if (!output_limit) {
    EXPECT_EQ(output_limit.error().code,
              codec::ErrorCode::resource_exhausted);
  }
}

TEST(generic_exporter_propagates_provider_failure_unchanged) {
  const std::array inputs{extracted_record(
      90, 5, codec::record_type_code(codec::RecordType::source_bytes),
      "input")};
  FailingExporter exporter;

  auto result = codec::invoke_exporter(exporter, inputs);

  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::network);
    EXPECT_EQ(result.error().message,
              std::string{"profile export backend unavailable"});
    EXPECT_TRUE(result.error().retryable);
  }
}
