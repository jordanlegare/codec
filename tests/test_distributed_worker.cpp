#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::byte> bytes(std::string_view value) {
  const auto chars = std::span{value.data(), value.size()};
  const auto raw = std::as_bytes(chars);
  return {raw.begin(), raw.end()};
}

codec::ExtractedRecord exact_record(std::string_view stream_name,
                                    std::uint64_t sequence,
                                    std::string_view payload) {
  codec::ExtractedRecord out;
  out.record.type = codec::RecordType::source_bytes;
  out.record.sequence = sequence;
  out.record.stream = codec::derive_stream_id(stream_name);
  out.record.start_ns = static_cast<std::int64_t>(sequence * 10);
  out.record.end_ns = out.record.start_ns + 10;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "distributed-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 100,
      .details_type = {},
      .details = {},
  };
}

codec::ProcessorOutput valid_output(codec::StreamId stream) {
  return codec::ProcessorOutput{
      .stream = stream,
      .type = 0x7a01,
      .start_ns = 10,
      .end_ns = 20,
      .truth = codec::TruthClass::derived,
      .payload = bytes("derived"),
      .process = process_identity(),
  };
}

codec::DistributedPartition one_partition(
    const std::vector<codec::ExtractedRecord>& inputs) {
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});
  return partitions->front();
}

class CountingProcessor final : public codec::StreamProcessor {
 public:
  std::string name() const override { return "counting-processor"; }

  codec::Result<std::vector<codec::ProcessorOutput>> process(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    return std::vector<codec::ProcessorOutput>{
        valid_output(inputs.front().record.stream)};
  }

  std::size_t calls{};
};

class CountingWorker final : public codec::DistributedWorker {
 public:
  std::string name() const override { return worker_name; }
  std::string processor_name() const override { return processor_label; }

  codec::Result<std::vector<codec::ProcessorOutput>> execute(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    if (failure.has_value()) return *failure;
    if (!outputs.empty()) return outputs;
    return std::vector<codec::ProcessorOutput>{
        valid_output(inputs.front().record.stream)};
  }

  std::string worker_name{"worker-a"};
  std::string processor_label{"processor-a"};
  std::vector<codec::ProcessorOutput> outputs;
  std::optional<codec::Error> failure;
  std::size_t calls{};
};

void expect_pre_execution_error(CountingWorker& worker,
                                const codec::DistributedPartition& partition,
                                std::span<const codec::ExtractedRecord> inputs,
                                codec::ErrorCode expected,
                                codec::DistributedExecutionLimits limits = {}) {
  const auto before = worker.calls;
  auto result = codec::execute_partition(worker, partition, inputs, limits);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, expected);
  EXPECT_EQ(worker.calls, before);
}

}  // namespace

TEST(distributed_worker_executes_one_verified_partition) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/valid", 1, "alpha"),
      exact_record("f2/valid", 2, "beta")};
  const auto partition = one_partition(inputs);
  CountingProcessor processor;
  codec::LocalProcessorWorker worker{processor, "local-worker"};

  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_TRUE(result);
  EXPECT_EQ(processor.calls, std::size_t{1});
  EXPECT_EQ(result->partition_identity, partition.identity);
  EXPECT_EQ(result->stream, partition.stream);
  EXPECT_EQ(result->worker_name, std::string{"local-worker"});
  EXPECT_EQ(result->processor_name, std::string{"counting-processor"});
  EXPECT_EQ(result->outputs.size(), std::size_t{1});
  EXPECT_EQ(result->outputs.front().truth, codec::TruthClass::derived);
  EXPECT_EQ(result->outputs.front().payload, bytes("derived"));
}

TEST(distributed_worker_rejects_empty_partition_or_inputs) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/empty", 1, "one")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  codec::DistributedPartition empty_partition;
  expect_pre_execution_error(worker, empty_partition, inputs,
                             codec::ErrorCode::invalid_argument);
  expect_pre_execution_error(
      worker, partition, std::span<const codec::ExtractedRecord>{},
      codec::ErrorCode::invalid_argument);
}

TEST(distributed_worker_rejects_tampered_partition_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/tamper", 1, "one"),
      exact_record("f2/tamper", 2, "two")};
  const auto original = one_partition(inputs);
  CountingWorker worker;

  auto wrong_identity = original;
  wrong_identity.identity[0] ^= 0x01U;
  expect_pre_execution_error(worker, wrong_identity, inputs,
                             codec::ErrorCode::invalid_argument);

  auto wrong_bytes = original;
  ++wrong_bytes.payload_bytes;
  expect_pre_execution_error(worker, wrong_bytes, inputs,
                             codec::ErrorCode::invalid_argument);

  auto wrong_link = original;
  ++wrong_link.records.front().sequence;
  expect_pre_execution_error(worker, wrong_link, inputs,
                             codec::ErrorCode::invalid_argument);

  auto wrong_stream = original;
  wrong_stream.records.front().stream = codec::derive_stream_id("f2/other");
  expect_pre_execution_error(worker, wrong_stream, inputs,
                             codec::ErrorCode::invalid_argument);
}

TEST(distributed_worker_rejects_wrong_or_reordered_inputs_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/order", 1, "one"),
      exact_record("f2/order", 2, "two")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  auto reordered = inputs;
  std::reverse(reordered.begin(), reordered.end());
  expect_pre_execution_error(worker, partition, reordered,
                             codec::ErrorCode::invalid_argument);

  std::vector<codec::ExtractedRecord> missing{inputs.front()};
  expect_pre_execution_error(worker, partition, missing,
                             codec::ErrorCode::invalid_argument);

  auto extra = inputs;
  extra.push_back(exact_record("f2/order", 3, "three"));
  expect_pre_execution_error(worker, partition, extra,
                             codec::ErrorCode::invalid_argument);

  auto wrong_stream = inputs;
  wrong_stream.front().record.stream = codec::derive_stream_id("f2/other");
  expect_pre_execution_error(worker, partition, wrong_stream,
                             codec::ErrorCode::invalid_argument);
}

TEST(distributed_worker_verifies_exact_payload_integrity_before_execution) {
  std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/hash", 1, "one")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;

  auto corrupt = inputs;
  corrupt.front().payload.front() ^= std::byte{0x01};
  expect_pre_execution_error(worker, partition, corrupt,
                             codec::ErrorCode::archive_corrupt);

  auto wrong_size = inputs;
  ++wrong_size.front().record.payload_size;
  expect_pre_execution_error(worker, partition, wrong_size,
                             codec::ErrorCode::invalid_argument);
}

TEST(distributed_worker_validates_limits_and_labels_before_execution) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/limits", 1, "abcd")};
  const auto partition = one_partition(inputs);

  {
    CountingWorker worker;
    codec::DistributedExecutionLimits limits;
    limits.maximum_input_records = 0;
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::invalid_argument, limits);
  }
  {
    CountingWorker worker;
    codec::DistributedExecutionLimits limits;
    limits.processor.maximum_outputs = 0;
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::invalid_argument, limits);
  }
  {
    const std::vector<codec::ExtractedRecord> two_inputs{
        exact_record("f2/two", 1, "a"), exact_record("f2/two", 2, "b")};
    const auto two_partition = one_partition(two_inputs);
    CountingWorker worker;
    codec::DistributedExecutionLimits limits;
    limits.maximum_input_records = 1;
    expect_pre_execution_error(worker, two_partition, two_inputs,
                               codec::ErrorCode::resource_exhausted, limits);
  }
  {
    CountingWorker worker;
    codec::DistributedExecutionLimits limits;
    limits.maximum_input_bytes = 3;
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::resource_exhausted, limits);
  }
  {
    CountingWorker worker;
    worker.worker_name.clear();
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::invalid_argument);
  }
  {
    CountingWorker worker;
    worker.processor_label.clear();
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::invalid_argument);
  }
  {
    CountingWorker worker;
    worker.worker_name = "worker-name-too-long";
    codec::DistributedExecutionLimits limits;
    limits.maximum_worker_name_bytes = 4;
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::resource_exhausted, limits);
  }
  {
    CountingWorker worker;
    worker.processor_label = "processor-name-too-long";
    codec::DistributedExecutionLimits limits;
    limits.maximum_processor_name_bytes = 4;
    expect_pre_execution_error(worker, partition, inputs,
                               codec::ErrorCode::resource_exhausted, limits);
  }
}

TEST(distributed_worker_propagates_one_provider_error_without_retry) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/failure", 1, "input")};
  const auto partition = one_partition(inputs);
  CountingWorker worker;
  worker.failure = codec::Error{codec::ErrorCode::inference,
                                "worker processor unavailable", true};

  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::inference);
  EXPECT_EQ(result.error().message,
            std::string{"worker processor unavailable"});
  EXPECT_TRUE(result.error().retryable);
  EXPECT_EQ(worker.calls, std::size_t{1});
}

TEST(distributed_worker_reuses_generic_processor_output_validation) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f2/output", 1, "input")};
  const auto partition = one_partition(inputs);

  {
    CountingWorker worker;
    auto output = valid_output(partition.stream);
    output.truth = codec::TruthClass::source_exact;
    worker.outputs = {output};
    auto result = codec::execute_partition(worker, partition, inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
  {
    CountingWorker worker;
    auto output = valid_output(partition.stream);
    output.start_ns = 21;
    output.end_ns = 20;
    worker.outputs = {output};
    auto result = codec::execute_partition(worker, partition, inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
  {
    CountingWorker worker;
    auto output = valid_output(partition.stream);
    output.type = codec::record_type_code(codec::RecordType::stream_provenance);
    worker.outputs = {output};
    auto result = codec::execute_partition(worker, partition, inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
  {
    CountingWorker worker;
    auto output = valid_output(partition.stream);
    output.process.operation.clear();
    worker.outputs = {output};
    auto result = codec::execute_partition(worker, partition, inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
  {
    CountingWorker worker;
    worker.outputs = {valid_output(partition.stream), valid_output(partition.stream)};
    codec::DistributedExecutionLimits limits;
    limits.processor.maximum_outputs = 1;
    auto result = codec::execute_partition(worker, partition, inputs, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
  {
    CountingWorker worker;
    worker.outputs = {valid_output(partition.stream)};
    codec::DistributedExecutionLimits limits;
    limits.processor.maximum_output_bytes = 1;
    auto result = codec::execute_partition(worker, partition, inputs, limits);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
    EXPECT_EQ(worker.calls, std::size_t{1});
  }
}
