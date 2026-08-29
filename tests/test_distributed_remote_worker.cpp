#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

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
  out.record.file_offset = 6000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "f6-test",
      .implementation_id = "codec-test",
      .implementation_version = "1",
      .implementation_hash = std::nullopt,
      .configuration_hash = std::nullopt,
      .created_utc_ns = 1,
      .details_type = {},
      .details = {},
  };
}

codec::ProcessorOutput valid_output(codec::StreamId stream,
                                    std::string_view payload = "remote") {
  return codec::ProcessorOutput{
      .stream = stream,
      .type = 0x7d01,
      .start_ns = 10,
      .end_ns = 20,
      .truth = codec::TruthClass::derived,
      .payload = bytes(payload),
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

class RecordingTransport final : public codec::DistributedWorkerTransport {
 public:
  std::string name() const override { return transport_name; }

  codec::Result<codec::DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    seen_worker = std::string{worker_name};
    seen_processor = std::string{processor_name};
    seen_records.assign(inputs.begin(), inputs.end());
    if (failure.has_value()) return *failure;
    if (response.has_value()) return *response;
    return codec::DistributedRemoteExecutionResponse{
        .worker_name = seen_worker,
        .processor_name = seen_processor,
        .outputs = {valid_output(inputs.front().record.stream)},
    };
  }

  std::string transport_name{"recording-transport"};
  std::string seen_worker;
  std::string seen_processor;
  std::vector<codec::ExtractedRecord> seen_records;
  std::optional<codec::DistributedRemoteExecutionResponse> response;
  std::optional<codec::Error> failure;
  std::size_t calls{};
};

class SingleObjectStore final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return "f6-object-store"; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    ++calls;
    if (object.store != "f6" || object.key != "record" ||
        object.version != "v1" || offset != 0 || length != payload.size()) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "f6 object range unavailable");
    }
    return payload;
  }

  std::vector<std::byte> payload;
  std::size_t calls{};
};

void expect_preflight_error(codec::RemoteDistributedWorker& worker,
                            RecordingTransport& transport,
                            std::span<const codec::ExtractedRecord> inputs,
                            codec::ErrorCode expected) {
  const auto before = transport.calls;
  auto result = worker.execute(inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, expected);
  EXPECT_EQ(transport.calls, before);
}

}  // namespace

TEST(distributed_remote_worker_executes_verified_partition_once) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/valid", 1, "alpha"),
      exact_record("f6/valid", 2, "beta")};
  const auto partition = one_partition(inputs);
  RecordingTransport transport;
  codec::RemoteDistributedWorker worker{
      transport, "remote-a", "processor-a"};

  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_TRUE(result);
  EXPECT_EQ(transport.calls, std::size_t{1});
  EXPECT_EQ(transport.seen_worker, std::string{"remote-a"});
  EXPECT_EQ(transport.seen_processor, std::string{"processor-a"});
  EXPECT_EQ(transport.seen_records.size(), inputs.size());
  EXPECT_EQ(transport.seen_records[0].record.hash, inputs[0].record.hash);
  EXPECT_EQ(transport.seen_records[0].payload, inputs[0].payload);
  EXPECT_EQ(transport.seen_records[1].record.hash, inputs[1].record.hash);
  EXPECT_EQ(transport.seen_records[1].payload, inputs[1].payload);
  EXPECT_EQ(result->worker_name, std::string{"remote-a"});
  EXPECT_EQ(result->processor_name, std::string{"processor-a"});
  EXPECT_EQ(result->outputs.size(), std::size_t{1});
}

TEST(distributed_remote_worker_rejects_request_before_transport_dispatch) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/preflight", 1, "one"),
      exact_record("f6/preflight", 2, "two")};

  {
    RecordingTransport transport;
    codec::RemoteDistributedWorker worker{transport, "", "processor-a"};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::invalid_argument);
  }
  {
    RecordingTransport transport;
    codec::RemoteDistributedWorker worker{transport, "remote-a", ""};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::invalid_argument);
  }
  {
    RecordingTransport transport;
    transport.transport_name.clear();
    codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::invalid_argument);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_input_records = 1;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::resource_exhausted);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_input_bytes = 1;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::resource_exhausted);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_transport_name_bytes = 4;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::resource_exhausted);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_worker_name_bytes = 4;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::resource_exhausted);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_processor_name_bytes = 4;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::resource_exhausted);
  }
  {
    RecordingTransport transport;
    codec::DistributedRemoteWorkerLimits limits;
    limits.maximum_input_records = 0;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    expect_preflight_error(worker, transport, inputs,
                           codec::ErrorCode::invalid_argument);
  }
  {
    RecordingTransport transport;
    auto malformed = inputs;
    ++malformed.front().record.payload_size;
    codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};
    expect_preflight_error(worker, transport, malformed,
                           codec::ErrorCode::invalid_argument);
  }
  {
    RecordingTransport transport;
    codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};
    expect_preflight_error(worker, transport,
                           std::span<const codec::ExtractedRecord>{},
                           codec::ErrorCode::invalid_argument);
  }
}

TEST(distributed_remote_worker_preserves_transport_error_without_retry) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/error", 1, "input")};
  RecordingTransport transport;
  transport.failure = codec::Error{
      codec::ErrorCode::network, "remote unavailable", true};
  codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};

  auto result = worker.execute(inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::network);
  EXPECT_EQ(result.error().message, std::string{"remote unavailable"});
  EXPECT_TRUE(result.error().retryable);
  EXPECT_EQ(transport.calls, std::size_t{1});
}

TEST(distributed_remote_worker_rejects_response_identity_mismatch) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/identity", 1, "input")};

  {
    RecordingTransport transport;
    transport.response = codec::DistributedRemoteExecutionResponse{
        .worker_name = "other-worker",
        .processor_name = "processor-a",
        .outputs = {valid_output(inputs.front().record.stream)},
    };
    codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};
    auto result = worker.execute(inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
    EXPECT_EQ(transport.calls, std::size_t{1});
  }
  {
    RecordingTransport transport;
    transport.response = codec::DistributedRemoteExecutionResponse{
        .worker_name = "remote-a",
        .processor_name = "other-processor",
        .outputs = {valid_output(inputs.front().record.stream)},
    };
    codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};
    auto result = worker.execute(inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::protocol);
    EXPECT_EQ(transport.calls, std::size_t{1});
  }
}

TEST(distributed_remote_worker_bounds_response_outputs) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/outputs", 1, "input")};

  {
    RecordingTransport transport;
    transport.response = codec::DistributedRemoteExecutionResponse{
        .worker_name = "remote-a",
        .processor_name = "processor-a",
        .outputs = {valid_output(inputs.front().record.stream),
                    valid_output(inputs.front().record.stream)},
    };
    codec::DistributedRemoteWorkerLimits limits;
    limits.processor.maximum_outputs = 1;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    auto result = worker.execute(inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
    EXPECT_EQ(transport.calls, std::size_t{1});
  }
  {
    RecordingTransport transport;
    transport.response = codec::DistributedRemoteExecutionResponse{
        .worker_name = "remote-a",
        .processor_name = "processor-a",
        .outputs = {valid_output(inputs.front().record.stream, "too-large")},
    };
    codec::DistributedRemoteWorkerLimits limits;
    limits.processor.maximum_output_bytes = 1;
    codec::RemoteDistributedWorker worker{
        transport, "remote-a", "processor-a", limits};
    auto result = worker.execute(inputs);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
    EXPECT_EQ(transport.calls, std::size_t{1});
  }
}

TEST(distributed_remote_worker_leaves_processor_semantics_to_f2) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/f2-validation", 1, "input")};
  const auto partition = one_partition(inputs);
  RecordingTransport transport;
  auto invalid = valid_output(partition.stream);
  invalid.truth = codec::TruthClass::source_exact;
  transport.response = codec::DistributedRemoteExecutionResponse{
      .worker_name = "remote-a",
      .processor_name = "processor-a",
      .outputs = {std::move(invalid)},
  };
  codec::RemoteDistributedWorker worker{transport, "remote-a", "processor-a"};

  auto result = codec::execute_partition(worker, partition, inputs);
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
  EXPECT_EQ(transport.calls, std::size_t{1});
}

TEST(distributed_scheduler_accepts_remote_worker_without_scheduler_changes) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f6/scheduler", 1, "payload")};
  auto partitions = codec::partition_exact_records(inputs);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), std::size_t{1});

  SingleObjectStore backend;
  backend.payload = inputs.front().payload;
  const std::vector<codec::DistributedRecordLocation> locations{
      codec::DistributedRecordLocation{
          .record = inputs.front().record,
          .object = {.store = "f6", .key = "record", .version = "v1"},
          .offset = 0,
          .length = inputs.front().record.payload_size,
      },
  };
  auto location_index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(location_index);

  RecordingTransport transport;
  codec::RemoteDistributedWorker remote_worker{
      transport, "remote-a", "processor-a"};
  std::array<codec::DistributedWorker*, 1> workers{&remote_worker};

  auto scheduled = codec::schedule_partitions(
      workers, backend, *location_index, *partitions);
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->succeeded, std::size_t{1});
  EXPECT_EQ(scheduled->partitions.size(), std::size_t{1});
  EXPECT_EQ(scheduled->partitions.front().status,
            codec::DistributedPartitionOutcomeStatus::succeeded);
  EXPECT_EQ(scheduled->partitions.front().worker_index, std::size_t{0});
  EXPECT_TRUE(scheduled->partitions.front().execution.has_value());
  EXPECT_EQ(scheduled->partitions.front().execution->worker_name,
            std::string{"remote-a"});
  EXPECT_EQ(transport.calls, std::size_t{1});
  EXPECT_EQ(backend.calls, std::size_t{1});
}
