#pragma once

#include <codec/archive.hpp>
#include <codec/processing.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace codec {

struct DistributedPartitionLimits {
  std::size_t maximum_input_records{16384};
  std::uint64_t maximum_input_bytes{256ULL * 1024ULL * 1024ULL};
  std::size_t maximum_partitions{4096};
  std::size_t maximum_records_per_partition{1024};
  std::uint64_t maximum_payload_bytes_per_partition{
      64ULL * 1024ULL * 1024ULL};
};

struct DistributedPartition {
  Sha256 identity{};
  StreamId stream{};
  std::vector<ProvenanceRecordLink> records;
  std::uint64_t payload_bytes{};
};

Result<std::vector<DistributedPartition>> partition_exact_records(
    std::span<const ExtractedRecord> inputs,
    DistributedPartitionLimits limits = {});

struct DistributedExecutionLimits {
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_worker_name_bytes{256};
  std::size_t maximum_processor_name_bytes{256};
  ProcessorRunLimits processor{};
};

class DistributedWorker {
 public:
  virtual ~DistributedWorker() = default;
  virtual std::string name() const = 0;
  virtual std::string processor_name() const = 0;
  virtual Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) = 0;
};

class LocalProcessorWorker final : public DistributedWorker {
 public:
  LocalProcessorWorker(StreamProcessor& processor, std::string worker_name);

  std::string name() const override;
  std::string processor_name() const override;
  Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) override;

 private:
  StreamProcessor* processor_{};
  std::string worker_name_;
};

struct DistributedRemoteWorkerLimits {
  std::size_t maximum_input_records{1024};
  std::uint64_t maximum_input_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_transport_name_bytes{256};
  std::size_t maximum_worker_name_bytes{256};
  std::size_t maximum_processor_name_bytes{256};
  ProcessorRunLimits processor{};
};

struct DistributedRemoteExecutionResponse {
  std::string worker_name;
  std::string processor_name;
  std::vector<ProcessorOutput> outputs;
};

class DistributedWorkerTransport {
 public:
  virtual ~DistributedWorkerTransport() = default;
  virtual std::string name() const = 0;
  virtual Result<DistributedRemoteExecutionResponse> dispatch(
      std::string_view worker_name,
      std::string_view processor_name,
      std::span<const ExtractedRecord> inputs) = 0;
};

class RemoteDistributedWorker final : public DistributedWorker {
 public:
  RemoteDistributedWorker(DistributedWorkerTransport& transport,
                          std::string worker_name,
                          std::string processor_name,
                          DistributedRemoteWorkerLimits limits = {});

  std::string name() const override;
  std::string processor_name() const override;
  Result<std::vector<ProcessorOutput>> execute(
      std::span<const ExtractedRecord> inputs) override;

 private:
  DistributedWorkerTransport* transport_{};
  std::string worker_name_;
  std::string processor_name_;
  DistributedRemoteWorkerLimits limits_{};
};

struct DistributedExecutionResult {
  Sha256 partition_identity{};
  StreamId stream{};
  std::string worker_name;
  std::string processor_name;
  std::vector<ProcessorOutput> outputs;
};

Result<DistributedExecutionResult> execute_partition(
    DistributedWorker& worker,
    const DistributedPartition& partition,
    std::span<const ExtractedRecord> inputs,
    DistributedExecutionLimits limits = {});

struct ObjectStoreObjectRef {
  std::string store;
  std::string key;
  std::string version;
};

struct DistributedRecordLocation {
  RecordInfo record{};
  ObjectStoreObjectRef object;
  std::uint64_t offset{};
  std::uint64_t length{};
};

struct DistributedRetrievalLimits {
  std::size_t maximum_records{1024};
  std::uint64_t maximum_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_backend_name_bytes{256};
  std::size_t maximum_store_bytes{512};
  std::size_t maximum_key_bytes{4096};
  std::size_t maximum_version_bytes{512};
};

class ObjectStoreBackend {
 public:
  virtual ~ObjectStoreBackend() = default;
  virtual std::string name() const = 0;
  virtual Result<std::vector<std::byte>> read_range(
      const ObjectStoreObjectRef& object,
      std::uint64_t offset,
      std::uint64_t length) = 0;
};

struct DistributedRetrievalResult {
  Sha256 partition_identity{};
  StreamId stream{};
  std::string backend_name;
  std::vector<ExtractedRecord> records;
};

Result<DistributedRetrievalResult> retrieve_partition_records(
    ObjectStoreBackend& backend,
    const DistributedPartition& partition,
    std::span<const DistributedRecordLocation> locations,
    DistributedRetrievalLimits limits = {});

struct DistributedLocationIndexLimits {
  std::size_t maximum_input_locations{65536};
  std::size_t maximum_records{16384};
  std::size_t maximum_locations_per_record{16};
  std::uint64_t maximum_metadata_bytes{64ULL * 1024ULL * 1024ULL};
  std::size_t maximum_store_bytes{512};
  std::size_t maximum_key_bytes{4096};
  std::size_t maximum_version_bytes{512};
};

struct DistributedLocationQueryLimits {
  std::size_t maximum_records{1024};
  std::size_t maximum_candidates_per_record{16};
  std::size_t maximum_candidates{4096};
};

struct DistributedLocationIndexEntry {
  RecordInfo record{};
  std::vector<DistributedRecordLocation> candidates;
};

struct DistributedLocationCandidateSet {
  ProvenanceRecordLink record{};
  std::vector<DistributedRecordLocation> candidates;
};

struct DistributedPartitionLocationCandidates {
  Sha256 partition_identity{};
  StreamId stream{};
  bool complete{};
  std::vector<DistributedLocationCandidateSet> records;
};

class DistributedLocationIndex {
 public:
  DistributedLocationIndex() = default;

  std::size_t record_count() const noexcept;
  std::size_t location_count() const noexcept;
  std::span<const DistributedLocationIndexEntry> entries() const noexcept;

 private:
  std::vector<DistributedLocationIndexEntry> entries_;
  std::size_t location_count_{};

  friend Result<DistributedLocationIndex> build_distributed_location_index(
      std::span<const DistributedRecordLocation> locations,
      DistributedLocationIndexLimits limits);
  friend Result<DistributedPartitionLocationCandidates>
  resolve_partition_location_candidates(
      const DistributedLocationIndex& index,
      const DistributedPartition& partition,
      DistributedLocationQueryLimits limits);
};

Result<DistributedLocationIndex> build_distributed_location_index(
    std::span<const DistributedRecordLocation> locations,
    DistributedLocationIndexLimits limits = {});

Result<DistributedPartitionLocationCandidates>
resolve_partition_location_candidates(
    const DistributedLocationIndex& index,
    const DistributedPartition& partition,
    DistributedLocationQueryLimits limits = {});

enum class DistributedPartitionOutcomeStatus {
  succeeded,
  location_unavailable,
  retrieval_failed,
  execution_failed,
};

struct DistributedSchedulingLimits {
  std::size_t maximum_partitions{4096};
  std::size_t maximum_workers{256};
  std::size_t maximum_total_records{16384};
  std::uint64_t maximum_total_payload_bytes{256ULL * 1024ULL * 1024ULL};
  DistributedLocationQueryLimits location_query{};
  DistributedRetrievalLimits retrieval{};
  DistributedExecutionLimits execution{};
};

struct DistributedPartitionOutcome {
  Sha256 partition_identity{};
  StreamId stream{};
  std::size_t partition_index{};
  std::size_t worker_index{};
  DistributedPartitionOutcomeStatus status{
      DistributedPartitionOutcomeStatus::location_unavailable};
  std::vector<DistributedRecordLocation> selected_locations;
  std::optional<DistributedExecutionResult> execution;
  std::optional<Error> error;
};

struct DistributedScheduleResult {
  std::size_t succeeded{};
  std::vector<DistributedPartitionOutcome> partitions;
};

Result<DistributedScheduleResult> schedule_partitions(
    std::span<DistributedWorker* const> workers,
    ObjectStoreBackend& backend,
    const DistributedLocationIndex& index,
    std::span<const DistributedPartition> partitions,
    DistributedSchedulingLimits limits = {});

}  // namespace codec