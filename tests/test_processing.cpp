#include "test.hpp"

#include <codec/processing.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

class TelemetryAdapter final : public codec::StreamAdapter {
 public:
  TelemetryAdapter()
      : records_{
            codec::AdapterRecord{
                .stream = stream_id(10),
                .type = 0x7700,
                .start_ns = 100,
                .end_ns = 110,
                .payload = bytes("temperature=21.5"),
            },
            codec::AdapterRecord{
                .stream = stream_id(10),
                .type = 0x7700,
                .start_ns = 110,
                .end_ns = 120,
                .payload = bytes("temperature=21.6"),
            },
        } {}

  std::string name() const override { return "telemetry-test"; }

  codec::Result<std::optional<codec::AdapterRecord>> next() override {
    if (next_ == records_.size()) {
      return std::optional<codec::AdapterRecord>{};
    }
    return std::optional<codec::AdapterRecord>{
        std::move(records_.at(next_++))};
  }

 private:
  std::vector<codec::AdapterRecord> records_;
  std::size_t next_{};
};

codec::AdapterRecord adapter_record() {
  return codec::AdapterRecord{
      .stream = stream_id(20),
      .type = 0x7702,
      .start_ns = 200,
      .end_ns = 210,
      .payload = bytes("ok"),
  };
}

class FixedAdapter final : public codec::StreamAdapter {
 public:
  explicit FixedAdapter(codec::AdapterRecord record)
      : record_(std::move(record)) {}

  std::string name() const override { return "fixed"; }

  codec::Result<std::optional<codec::AdapterRecord>> next() override {
    ++calls;
    return std::optional<codec::AdapterRecord>{record_};
  }

  std::size_t calls{};

 private:
  codec::AdapterRecord record_;
};

class FailingAdapter final : public codec::StreamAdapter {
 public:
  std::string name() const override { return "failing"; }

  codec::Result<std::optional<codec::AdapterRecord>> next() override {
    return codec::Error{
        codec::ErrorCode::network,
        "telemetry transport interrupted",
        true,
    };
  }
};

void expect_adapter_error(codec::AdapterRecord record,
                          codec::AdapterReadLimits limits,
                          codec::ErrorCode expected) {
  FixedAdapter adapter{std::move(record)};
  auto result = codec::pull_adapter_record(adapter, limits);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, expected);
}

codec::ExtractedRecord extracted_record(std::vector<std::byte> payload) {
  codec::RecordInfo record;
  record.type = codec::RecordType::source_bytes;
  record.sequence = 7;
  record.stream = stream_id(30);
  record.start_ns = 300;
  record.end_ns = 310;
  record.payload_size = payload.size();
  return codec::ExtractedRecord{record, std::move(payload)};
}

codec::ProvenanceProcess normalization_process() {
  return codec::ProvenanceProcess{
      .operation = "normalize-temperature",
      .implementation_id = "telemetry-profile",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 320,
      .details_type = {},
      .details = {},
  };
}

class TelemetryNormalizer final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "telemetry-normalizer"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    if (inputs.size() != 1 ||
        inputs.front().payload != bytes("temp_c=21.50")) {
      return codec::Error{codec::ErrorCode::decode,
                          "unexpected telemetry input", false};
    }
    return std::vector<codec::ProcessorOutput>{
        codec::ProcessorOutput{
            .stream = stream_id(31),
            .type = 0x7701,
            .start_ns = 300,
            .end_ns = 310,
            .truth = codec::TruthClass::state_exact,
            .payload = bytes("21.500"),
            .process = normalization_process(),
        },
    };
  }
};

codec::ProcessorOutput processor_output() {
  return codec::ProcessorOutput{
      .stream = stream_id(40),
      .type = 0x7703,
      .start_ns = 400,
      .end_ns = 410,
      .truth = codec::TruthClass::derived,
      .payload = bytes("ab"),
      .process = normalization_process(),
  };
}

class CountingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "counting"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord>) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{};
  }

  std::size_t calls{};
};

class FixedProcessor final : public codec::StreamProcessor {
 public:
  explicit FixedProcessor(std::vector<codec::ProcessorOutput> outputs)
      : outputs_(std::move(outputs)) {}

  std::string name() const override { return "fixed"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord>) override {
    return outputs_;
  }

 private:
  std::vector<codec::ProcessorOutput> outputs_;
};

class FailingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "failing"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord>) override {
    return codec::Error{
        codec::ErrorCode::inference,
        "profile processor unavailable",
        true,
    };
  }
};

void expect_processor_error(codec::ProcessorOutput output,
                            codec::ProcessorRunLimits limits,
                            codec::ErrorCode expected) {
  FixedProcessor processor{{std::move(output)}};
  const std::array inputs{extracted_record(bytes("input"))};
  auto result = codec::invoke_processor(processor, inputs, limits);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, expected);
}

}  // namespace

TEST(generic_adapter_pulls_owned_s0_records_in_order) {
  TelemetryAdapter adapter;
  auto first = codec::pull_adapter_record(adapter);
  auto second = codec::pull_adapter_record(adapter);
  auto eof = codec::pull_adapter_record(adapter);

  EXPECT_TRUE(first && *first);
  EXPECT_TRUE(second && *second);
  EXPECT_TRUE(eof && !*eof);
  EXPECT_EQ((*first)->stream, stream_id(10));
  EXPECT_EQ((*first)->type, codec::RecordTypeCode{0x7700});
  EXPECT_EQ((*first)->start_ns, std::int64_t{100});
  EXPECT_EQ((*first)->end_ns, std::int64_t{110});
  EXPECT_EQ((*first)->payload, bytes("temperature=21.5"));
  EXPECT_EQ((*second)->payload, bytes("temperature=21.6"));
  EXPECT_EQ(adapter.name(), std::string{"telemetry-test"});
}

TEST(generic_adapter_validates_limits_before_provider_invocation) {
  FixedAdapter adapter{adapter_record()};
  auto result = codec::pull_adapter_record(
      adapter, codec::AdapterReadLimits{.maximum_payload_bytes = 0});
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  }
  EXPECT_EQ(adapter.calls, std::size_t{0});
}

TEST(generic_adapter_propagates_provider_failure) {
  FailingAdapter adapter;
  auto result = codec::pull_adapter_record(adapter);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::network);
    EXPECT_EQ(result.error().message,
              std::string{"telemetry transport interrupted"});
    EXPECT_TRUE(result.error().retryable);
  }
}

TEST(generic_adapter_rejects_invalid_records_and_payload_limit) {
  auto inverted = adapter_record();
  inverted.start_ns = 211;
  inverted.end_ns = 210;
  expect_adapter_error(std::move(inverted), {},
                       codec::ErrorCode::invalid_argument);

  auto provenance = adapter_record();
  provenance.type = codec::record_type_code(
      codec::RecordType::stream_provenance);
  expect_adapter_error(std::move(provenance), {},
                       codec::ErrorCode::invalid_argument);

  auto final_index = adapter_record();
  final_index.type = codec::record_type_code(codec::RecordType::final_index);
  expect_adapter_error(std::move(final_index), {},
                       codec::ErrorCode::invalid_argument);

  expect_adapter_error(
      adapter_record(),
      codec::AdapterReadLimits{.maximum_payload_bytes = 1},
      codec::ErrorCode::resource_exhausted);
}

TEST(generic_processor_returns_bounded_s1_with_process_identity) {
  const auto input = extracted_record(bytes("temp_c=21.50"));
  TelemetryNormalizer processor;
  const std::array inputs{input};
  auto outputs = codec::invoke_processor(processor, inputs);

  EXPECT_TRUE(outputs);
  if (outputs) {
    EXPECT_EQ(outputs->size(), std::size_t{1});
    EXPECT_EQ(outputs->front().stream, stream_id(31));
    EXPECT_EQ(outputs->front().truth, codec::TruthClass::state_exact);
    EXPECT_EQ(outputs->front().type, codec::RecordTypeCode{0x7701});
    EXPECT_EQ(outputs->front().start_ns, std::int64_t{300});
    EXPECT_EQ(outputs->front().end_ns, std::int64_t{310});
    EXPECT_EQ(outputs->front().payload, bytes("21.500"));
    EXPECT_EQ(outputs->front().process.operation,
              std::string{"normalize-temperature"});
    EXPECT_EQ(outputs->front().process.implementation_id,
              std::string{"telemetry-profile"});
  }
  EXPECT_EQ(processor.name(), std::string{"telemetry-normalizer"});
}

TEST(generic_processor_validates_inputs_and_limits_before_invocation) {
  CountingProcessor processor;
  auto empty = codec::invoke_processor(
      processor, std::span<const codec::ExtractedRecord>{});
  EXPECT_FALSE(empty);
  if (!empty) {
    EXPECT_EQ(empty.error().code, codec::ErrorCode::invalid_argument);
  }

  std::vector<codec::ExtractedRecord> excessive(
      4097, extracted_record(bytes("input")));
  auto too_many = codec::invoke_processor(processor, excessive);
  EXPECT_FALSE(too_many);
  if (!too_many) {
    EXPECT_EQ(too_many.error().code, codec::ErrorCode::invalid_argument);
  }

  auto mismatched = extracted_record(bytes("input"));
  ++mismatched.record.payload_size;
  const std::array mismatched_inputs{mismatched};
  auto wrong_size = codec::invoke_processor(processor, mismatched_inputs);
  EXPECT_FALSE(wrong_size);
  if (!wrong_size) {
    EXPECT_EQ(wrong_size.error().code,
              codec::ErrorCode::invalid_argument);
  }

  const std::array valid_inputs{extracted_record(bytes("input"))};
  auto zero_count = codec::invoke_processor(
      processor, valid_inputs,
      codec::ProcessorRunLimits{
          .maximum_outputs = 0,
          .maximum_output_bytes = 64,
      });
  EXPECT_FALSE(zero_count);
  auto zero_bytes = codec::invoke_processor(
      processor, valid_inputs,
      codec::ProcessorRunLimits{
          .maximum_outputs = 1,
          .maximum_output_bytes = 0,
      });
  EXPECT_FALSE(zero_bytes);
  EXPECT_EQ(processor.calls, std::size_t{0});
}

TEST(generic_processor_propagates_provider_failure) {
  FailingProcessor processor;
  const std::array inputs{extracted_record(bytes("input"))};
  auto result = codec::invoke_processor(processor, inputs);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::inference);
    EXPECT_EQ(result.error().message,
              std::string{"profile processor unavailable"});
    EXPECT_TRUE(result.error().retryable);
  }
}

TEST(generic_processor_enforces_output_count_and_cumulative_bytes) {
  const std::array inputs{extracted_record(bytes("input"))};
  FixedProcessor too_many{{processor_output(), processor_output()}};
  auto count = codec::invoke_processor(
      too_many, inputs,
      codec::ProcessorRunLimits{
          .maximum_outputs = 1,
          .maximum_output_bytes = 64,
      });
  EXPECT_FALSE(count);
  if (!count) {
    EXPECT_EQ(count.error().code, codec::ErrorCode::resource_exhausted);
  }

  FixedProcessor too_large{{processor_output(), processor_output()}};
  auto bytes_result = codec::invoke_processor(
      too_large, inputs,
      codec::ProcessorRunLimits{
          .maximum_outputs = 2,
          .maximum_output_bytes = 3,
      });
  EXPECT_FALSE(bytes_result);
  if (!bytes_result) {
    EXPECT_EQ(bytes_result.error().code,
              codec::ErrorCode::resource_exhausted);
  }
}

TEST(generic_processor_rejects_invalid_truth_time_type_and_process) {
  auto source_truth = processor_output();
  source_truth.truth = codec::TruthClass::source_exact;
  expect_processor_error(std::move(source_truth), {},
                         codec::ErrorCode::invalid_argument);

  auto unknown_truth = processor_output();
  unknown_truth.truth = static_cast<codec::TruthClass>(0xff);
  expect_processor_error(std::move(unknown_truth), {},
                         codec::ErrorCode::invalid_argument);

  auto inverted = processor_output();
  inverted.start_ns = 411;
  inverted.end_ns = 410;
  expect_processor_error(std::move(inverted), {},
                         codec::ErrorCode::invalid_argument);

  auto provenance = processor_output();
  provenance.type = codec::record_type_code(
      codec::RecordType::stream_provenance);
  expect_processor_error(std::move(provenance), {},
                         codec::ErrorCode::invalid_argument);

  auto final_index = processor_output();
  final_index.type = codec::record_type_code(codec::RecordType::final_index);
  expect_processor_error(std::move(final_index), {},
                         codec::ErrorCode::invalid_argument);

  auto empty_operation = processor_output();
  empty_operation.process.operation.clear();
  expect_processor_error(std::move(empty_operation), {},
                         codec::ErrorCode::invalid_argument);

  auto mismatched_details = processor_output();
  mismatched_details.process.details = bytes("opaque");
  expect_processor_error(std::move(mismatched_details), {},
                         codec::ErrorCode::invalid_argument);
}

TEST(generic_processor_accepts_an_empty_output_set) {
  CountingProcessor processor;
  const std::array inputs{extracted_record(bytes("input"))};
  auto result = codec::invoke_processor(processor, inputs);
  EXPECT_TRUE(result);
  if (result) EXPECT_TRUE(result->empty());
  EXPECT_EQ(processor.calls, std::size_t{1});
}
