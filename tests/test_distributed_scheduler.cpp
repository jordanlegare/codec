#include "test.hpp"

#include <codec/distributed.hpp>
#include <codec/integrity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
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
  out.record.file_offset = 5000 + sequence;
  out.payload = bytes(payload);
  out.record.payload_size = static_cast<std::uint64_t>(out.payload.size());
  out.record.hash = codec::sha256(out.payload);
  return out;
}

codec::ProvenanceProcess process_identity() {
  return codec::ProvenanceProcess{
      .operation = "f5-test",
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
                                    std::int64_t start_ns,
                                    std::int64_t end_ns) {
  return codec::ProcessorOutput{
      .stream = stream,
      .type = 0x7c01,
      .start_ns = start_ns,
      .end_ns = end_ns,
      .truth = codec::TruthClass::derived,
      .payload = bytes("scheduled"),
      .process = process_identity(),
  };
}

std::vector<codec::DistributedPartition> single_record_partitions(
    const std::vector<codec::ExtractedRecord>& inputs) {
  codec::DistributedPartitionLimits limits;
  limits.maximum_records_per_partition = 1;
  auto partitions = codec::partition_exact_records(inputs, limits);
  EXPECT_TRUE(partitions);
  EXPECT_EQ(partitions->size(), inputs.size());
  return *partitions;
}

codec::DistributedRecordLocation location_for(
    const codec::ExtractedRecord& input,
    std::string key) {
  return codec::DistributedRecordLocation{
      .record = input.record,
      .object = {.store = "f5-store", .key = std::move(key), .version = "v1"},
      .offset = 0,
      .length = input.record.payload_size,
  };
}

std::string object_id(const codec::ObjectStoreObjectRef& object) {
  return object.store + "\n" + object.key + "\n" + object.version;
}

class MapBackend final : public codec::ObjectStoreBackend {
 public:
  std::string name() const override { return "f5-backend"; }

  codec::Result<std::vector<std::byte>> read_range(
      const codec::ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) override {
    ++calls;
    if (fail_key.has_value() && object.key == *fail_key) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "configured retrieval failure", true);
    }
    const auto found = objects.find(object_id(object));
    if (found == objects.end()) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::network, "object missing", true);
    }
    const auto size = static_cast<std::uint64_t>(found->second.size());
    if (offset > size || length > size - offset) {
      return codec::fail<std::vector<std::byte>>(
          codec::ErrorCode::protocol, "range unavailable");
    }
    const auto begin =
        found->second.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    return std::vector<std::byte>{begin, end};
  }

  void put(const codec::ObjectStoreObjectRef& object,
           const std::vector<std::byte>& payload) {
    objects[object_id(object)] = payload;
  }

  std::map<std::string, std::vector<std::byte>> objects;
  std::optional<std::string> fail_key;
  std::size_t calls{};
};

class RecordingWorker final : public codec::DistributedWorker {
 public:
  RecordingWorker(std::string worker_name, std::string processor_name)
      : worker_name_(std::move(worker_name)),
        processor_name_(std::move(processor_name)) {}

  std::string name() const override { return worker_name_; }
  std::string processor_name() const override { return processor_name_; }

  codec::Result<std::vector<codec::ProcessorOutput>> execute(
      std::span<const codec::ExtractedRecord> inputs) override {
    ++calls;
    if (fail) {
      return codec::fail<std::vector<codec::ProcessorOutput>>(
          codec::ErrorCode::inference, "configured worker failure", true);
    }
    for (const auto& input : inputs) seen_hashes.push_back(input.record.hash);
    return std::vector<codec::ProcessorOutput>{valid_output(
        inputs.front().record.stream,
        inputs.front().record.start_ns,
        inputs.back().record.end_ns)};
  }

  std::size_t calls{};
  std::vector<codec::Sha256> seen_hashes;
  bool fail{};

 private:
  std::string worker_name_;
  std::string processor_name_;
};

std::vector<codec::DistributedRecordLocation> locations_for(
    const std::vector<codec::ExtractedRecord>& inputs,
    MapBackend& backend,
    bool include_all = true) {
  std::vector<codec::DistributedRecordLocation> locations;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    if (!include_all && index == 0) continue;
    auto z = location_for(inputs[index], "z-" + std::to_string(index));
    auto a = location_for(inputs[index], "a-" + std::to_string(index));
    backend.put(z.object, inputs[index].payload);
    backend.put(a.object, inputs[index].payload);
    locations.push_back(std::move(z));
    locations.push_back(std::move(a));
  }
  return locations;
}

void expect_schedule_preflight_error(
    std::span<codec::DistributedWorker* const> workers,
    MapBackend& backend,
    const codec::DistributedLocationIndex& index,
    std::span<const codec::DistributedPartition> partitions,
    codec::DistributedSchedulingLimits limits,
    codec::ErrorCode expected) {
  const auto before_backend = backend.calls;
  std::vector<std::size_t> before_workers;
  before_workers.reserve(workers.size());
  for (auto* worker : workers) {
    before_workers.push_back(worker == nullptr ? 0 :
        static_cast<RecordingWorker*>(worker)->calls);
  }

  auto scheduled = codec::schedule_partitions(
      workers, backend, index, partitions, limits);
  EXPECT_FALSE(scheduled);
  EXPECT_EQ(scheduled.error().code, expected);
  EXPECT_EQ(backend.calls, before_backend);
  for (std::size_t index_value = 0; index_value < workers.size(); ++index_value) {
    if (workers[index_value] != nullptr) {
      EXPECT_EQ(static_cast<RecordingWorker*>(workers[index_value])->calls,
                before_workers[index_value]);
    }
  }
}

}  // namespace

TEST(distributed_scheduler_composes_f1_through_f4_f3_f2_deterministically) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f5/success", 1, "alpha"),
      exact_record("f5/success", 2, "beta"),
      exact_record("f5/success", 3, "gamma"),
      exact_record("f5/success", 4, "delta")};
  const auto partitions = single_record_partitions(inputs);

  MapBackend backend;
  auto locations = locations_for(inputs, backend);
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);

  RecordingWorker worker_a{"worker-a", "processor-a"};
  RecordingWorker worker_b{"worker-b", "processor-b"};
  std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};

  auto scheduled = codec::schedule_partitions(
      workers, backend, *index, partitions);
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->partitions.size(), partitions.size());
  EXPECT_EQ(scheduled->succeeded, partitions.size());
  EXPECT_EQ(worker_a.calls, std::size_t{2});
  EXPECT_EQ(worker_b.calls, std::size_t{2});
  EXPECT_EQ(backend.calls, inputs.size());

  for (std::size_t index_value = 0;
       index_value < scheduled->partitions.size(); ++index_value) {
    const auto& outcome = scheduled->partitions[index_value];
    EXPECT_EQ(outcome.partition_identity, partitions[index_value].identity);
    EXPECT_EQ(outcome.stream, partitions[index_value].stream);
    EXPECT_EQ(outcome.partition_index, index_value);
    EXPECT_EQ(outcome.worker_index, index_value % workers.size());
    EXPECT_EQ(outcome.status,
              codec::DistributedPartitionOutcomeStatus::succeeded);
    EXPECT_TRUE(outcome.execution.has_value());
    EXPECT_FALSE(outcome.error.has_value());
    EXPECT_EQ(outcome.selected_locations.size(), std::size_t{1});
    EXPECT_EQ(outcome.selected_locations.front().object.key,
              "a-" + std::to_string(index_value));
    EXPECT_EQ(outcome.execution->worker_name,
              index_value % 2 == 0 ? std::string{"worker-a"}
                                   : std::string{"worker-b"});
  }
}

TEST(distributed_scheduler_reports_missing_location_and_continues_in_input_order) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f5/missing", 1, "one"),
      exact_record("f5/missing", 2, "two"),
      exact_record("f5/missing", 3, "three")};
  const auto partitions = single_record_partitions(inputs);

  MapBackend backend;
  auto locations = locations_for(inputs, backend, false);
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);

  RecordingWorker worker_a{"worker-a", "processor-a"};
  RecordingWorker worker_b{"worker-b", "processor-b"};
  std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};

  auto scheduled = codec::schedule_partitions(
      workers, backend, *index, partitions);
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->succeeded, std::size_t{2});
  EXPECT_EQ(scheduled->partitions.size(), std::size_t{3});
  EXPECT_EQ(scheduled->partitions[0].status,
            codec::DistributedPartitionOutcomeStatus::location_unavailable);
  EXPECT_TRUE(scheduled->partitions[0].error.has_value());
  EXPECT_EQ(scheduled->partitions[0].error->code, codec::ErrorCode::network);
  EXPECT_FALSE(scheduled->partitions[0].error->retryable);
  EXPECT_EQ(scheduled->partitions[1].worker_index, std::size_t{1});
  EXPECT_EQ(scheduled->partitions[2].worker_index, std::size_t{0});
  EXPECT_EQ(worker_a.calls, std::size_t{1});
  EXPECT_EQ(worker_b.calls, std::size_t{1});
  EXPECT_EQ(backend.calls, std::size_t{2});
}

TEST(distributed_scheduler_reports_retrieval_failure_without_worker_retry) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f5/retrieval", 1, "one"),
      exact_record("f5/retrieval", 2, "two"),
      exact_record("f5/retrieval", 3, "three")};
  const auto partitions = single_record_partitions(inputs);

  MapBackend backend;
  auto locations = locations_for(inputs, backend);
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);
  backend.fail_key = "a-1";

  RecordingWorker worker_a{"worker-a", "processor-a"};
  RecordingWorker worker_b{"worker-b", "processor-b"};
  std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};

  auto scheduled = codec::schedule_partitions(
      workers, backend, *index, partitions);
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->succeeded, std::size_t{2});
  EXPECT_EQ(scheduled->partitions[1].status,
            codec::DistributedPartitionOutcomeStatus::retrieval_failed);
  EXPECT_TRUE(scheduled->partitions[1].error.has_value());
  EXPECT_EQ(scheduled->partitions[1].error->code, codec::ErrorCode::network);
  EXPECT_TRUE(scheduled->partitions[1].error->retryable);
  EXPECT_EQ(worker_a.calls, std::size_t{2});
  EXPECT_EQ(worker_b.calls, std::size_t{0});
  EXPECT_EQ(backend.calls, std::size_t{3});
}

TEST(distributed_scheduler_reports_execution_failure_and_keeps_round_robin_slot) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f5/execution", 1, "one"),
      exact_record("f5/execution", 2, "two"),
      exact_record("f5/execution", 3, "three")};
  const auto partitions = single_record_partitions(inputs);

  MapBackend backend;
  auto locations = locations_for(inputs, backend);
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);

  RecordingWorker worker_a{"worker-a", "processor-a"};
  RecordingWorker worker_b{"worker-b", "processor-b"};
  worker_b.fail = true;
  std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};

  auto scheduled = codec::schedule_partitions(
      workers, backend, *index, partitions);
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->succeeded, std::size_t{2});
  EXPECT_EQ(scheduled->partitions[1].worker_index, std::size_t{1});
  EXPECT_EQ(scheduled->partitions[1].status,
            codec::DistributedPartitionOutcomeStatus::execution_failed);
  EXPECT_TRUE(scheduled->partitions[1].error.has_value());
  EXPECT_EQ(scheduled->partitions[1].error->code, codec::ErrorCode::inference);
  EXPECT_TRUE(scheduled->partitions[1].error->retryable);
  EXPECT_EQ(scheduled->partitions[2].worker_index, std::size_t{0});
  EXPECT_EQ(scheduled->partitions[2].status,
            codec::DistributedPartitionOutcomeStatus::succeeded);
  EXPECT_EQ(worker_a.calls, std::size_t{2});
  EXPECT_EQ(worker_b.calls, std::size_t{1});
  EXPECT_EQ(backend.calls, std::size_t{3});
}

TEST(distributed_scheduler_rejects_invalid_batch_before_side_effects) {
  const std::vector<codec::ExtractedRecord> inputs{
      exact_record("f5/preflight", 1, "one"),
      exact_record("f5/preflight", 2, "two")};
  const auto partitions = single_record_partitions(inputs);

  MapBackend backend;
  auto locations = locations_for(inputs, backend);
  auto index = codec::build_distributed_location_index(locations);
  EXPECT_TRUE(index);

  RecordingWorker worker_a{"worker-a", "processor-a"};
  RecordingWorker worker_b{"worker-b", "processor-b"};
  std::array<codec::DistributedWorker*, 2> workers{&worker_a, &worker_b};

  {
    std::span<codec::DistributedWorker* const> no_workers;
    expect_schedule_preflight_error(no_workers, backend, *index, partitions,
                                    {}, codec::ErrorCode::invalid_argument);
  }
  {
    std::array<codec::DistributedWorker*, 2> with_null{&worker_a, nullptr};
    expect_schedule_preflight_error(with_null, backend, *index, partitions,
                                    {}, codec::ErrorCode::invalid_argument);
  }
  {
    auto limits = codec::DistributedSchedulingLimits{};
    limits.maximum_partitions = 0;
    expect_schedule_preflight_error(workers, backend, *index, partitions,
                                    limits, codec::ErrorCode::invalid_argument);
  }
  {
    auto limits = codec::DistributedSchedulingLimits{};
    limits.maximum_workers = 1;
    expect_schedule_preflight_error(workers, backend, *index, partitions,
                                    limits, codec::ErrorCode::resource_exhausted);
  }
  {
    auto limits = codec::DistributedSchedulingLimits{};
    limits.maximum_total_records = 1;
    expect_schedule_preflight_error(workers, backend, *index, partitions,
                                    limits, codec::ErrorCode::resource_exhausted);
  }
  {
    auto limits = codec::DistributedSchedulingLimits{};
    limits.maximum_total_payload_bytes = 1;
    expect_schedule_preflight_error(workers, backend, *index, partitions,
                                    limits, codec::ErrorCode::resource_exhausted);
  }
  {
    auto tampered = partitions;
    tampered[1].identity[0] ^= 0x01U;
    expect_schedule_preflight_error(workers, backend, *index, tampered,
                                    {}, codec::ErrorCode::invalid_argument);
  }
  {
    auto wrong_stream = partitions;
    wrong_stream[1].records.front().stream =
        codec::derive_stream_id("f5/other");
    expect_schedule_preflight_error(workers, backend, *index, wrong_stream,
                                    {}, codec::ErrorCode::invalid_argument);
  }
  {
    const std::vector<codec::DistributedPartition> duplicate{
        partitions.front(), partitions.front()};
    expect_schedule_preflight_error(workers, backend, *index, duplicate,
                                    {}, codec::ErrorCode::invalid_argument);
  }
}

TEST(distributed_scheduler_empty_batch_requires_no_worker_or_backend_io) {
  MapBackend backend;
  auto index = codec::build_distributed_location_index(
      std::span<const codec::DistributedRecordLocation>{});
  EXPECT_TRUE(index);

  auto scheduled = codec::schedule_partitions(
      std::span<codec::DistributedWorker* const>{},
      backend,
      *index,
      std::span<const codec::DistributedPartition>{});
  EXPECT_TRUE(scheduled);
  EXPECT_EQ(scheduled->succeeded, std::size_t{0});
  EXPECT_TRUE(scheduled->partitions.empty());
  EXPECT_EQ(backend.calls, std::size_t{0});
}
