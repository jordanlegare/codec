#include <codec/distributed.hpp>

#include "internal.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace codec {
namespace {

bool same_link(const ProvenanceRecordLink& lhs,
               const ProvenanceRecordLink& rhs) {
  return lhs.stream == rhs.stream && lhs.type == rhs.type &&
         lhs.sequence == rhs.sequence && lhs.hash == rhs.hash;
}

Result<void> validate_retrieval_limits(
    const DistributedRetrievalLimits& limits) {
  if (limits.maximum_records == 0 || limits.maximum_bytes == 0 ||
      limits.maximum_backend_name_bytes == 0 ||
      limits.maximum_store_bytes == 0 || limits.maximum_key_bytes == 0 ||
      limits.maximum_version_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed retrieval limits must be non-zero");
  }
  return {};
}

}  // namespace

Result<DistributedRetrievalResult> retrieve_partition_records(
    ObjectStoreBackend& backend,
    const DistributedPartition& partition,
    std::span<const DistributedRecordLocation> locations,
    DistributedRetrievalLimits limits) {
  try {
    auto valid_limits = validate_retrieval_limits(limits);
    if (!valid_limits) return valid_limits.error();

    if (partition.records.empty() || locations.empty() ||
        partition.records.size() != locations.size()) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed retrieval requires one non-empty exact location per partition member");
    }
    if (locations.size() > limits.maximum_records) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::resource_exhausted,
          "distributed retrieval record-count limit exceeded");
    }

    std::string backend_name = backend.name();
    if (backend_name.empty()) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "object-store backend name must be non-empty");
    }
    if (backend_name.size() > limits.maximum_backend_name_bytes) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::resource_exhausted,
          "object-store backend name exceeds the configured limit");
    }

    for (const auto& link : partition.records) {
      if (link.stream != partition.stream) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed partition contains a link from another stream");
      }
    }
    if (detail::distributed_partition_identity(partition.stream,
                                               partition.records) !=
        partition.identity) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed partition identity does not match ordered membership");
    }

    std::uint64_t total_bytes = 0;
    for (std::size_t index = 0; index < locations.size(); ++index) {
      const auto& location = locations[index];
      if (location.record.stream != partition.stream ||
          !same_link(detail::distributed_exact_link(location.record),
                     partition.records[index])) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location does not match ordered partition membership");
      }
      if (location.record.end_ns < location.record.start_ns) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location end time precedes start time");
      }
      if (location.length != location.record.payload_size) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location length does not match payload size");
      }
      if (location.offset >
          std::numeric_limits<std::uint64_t>::max() - location.length) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "distributed record location range overflows");
      }
      if (location.object.store.empty() || location.object.key.empty()) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::invalid_argument,
            "object-store location store and key must be non-empty");
      }
      if (location.object.store.size() > limits.maximum_store_bytes ||
          location.object.key.size() > limits.maximum_key_bytes ||
          location.object.version.size() > limits.maximum_version_bytes) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::resource_exhausted,
            "object-store location metadata exceeds configured limits");
      }
      if (location.length > limits.maximum_bytes - total_bytes) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::resource_exhausted,
            "distributed retrieval aggregate byte limit exceeded");
      }
      total_bytes += location.length;
    }
    if (total_bytes != partition.payload_bytes) {
      return fail<DistributedRetrievalResult>(
          ErrorCode::invalid_argument,
          "distributed partition payload-byte total is inconsistent with locations");
    }

    std::vector<ExtractedRecord> records;
    records.reserve(locations.size());
    for (const auto& location : locations) {
      auto payload = backend.read_range(location.object,
                                        location.offset,
                                        location.length);
      if (!payload) return payload.error();
      if (payload->size() != location.length) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::protocol,
            "object-store backend returned a range with the wrong byte count");
      }
      if (sha256(*payload) != location.record.hash) {
        return fail<DistributedRetrievalResult>(
            ErrorCode::archive_corrupt,
            "object-store payload hash does not match exact record identity");
      }
      records.push_back(ExtractedRecord{
          .record = location.record,
          .payload = std::move(*payload),
      });
    }

    return DistributedRetrievalResult{
        .partition_identity = partition.identity,
        .stream = partition.stream,
        .backend_name = std::move(backend_name),
        .records = std::move(records),
    };
  } catch (const std::bad_alloc&) {
    return fail<DistributedRetrievalResult>(
        ErrorCode::resource_exhausted,
        "distributed retrieval allocation failed");
  }
}

}  // namespace codec
