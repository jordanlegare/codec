#pragma once

#include <codec/archive.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
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

}  // namespace codec
