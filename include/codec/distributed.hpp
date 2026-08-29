#pragma once

#include <codec/archive.hpp>
#include <codec/processing.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
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

}  // namespace codec
