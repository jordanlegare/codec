#include <codec/distributed.hpp>

#include "internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace codec {
namespace {

bool same_link(const ProvenanceRecordLink& lhs,
               const ProvenanceRecordLink& rhs) {
  return lhs.stream == rhs.stream && lhs.type == rhs.type &&
         lhs.sequence == rhs.sequence && lhs.hash == rhs.hash;
}

bool less_link(const ProvenanceRecordLink& lhs,
               const ProvenanceRecordLink& rhs) {
  if (lhs.stream != rhs.stream) return lhs.stream < rhs.stream;
  if (lhs.type != rhs.type) return lhs.type < rhs.type;
  if (lhs.sequence != rhs.sequence) return lhs.sequence < rhs.sequence;
  return lhs.hash < rhs.hash;
}

bool same_record_info(const RecordInfo& lhs, const RecordInfo& rhs) {
  return lhs.type_code() == rhs.type_code() &&
         lhs.sequence == rhs.sequence && lhs.stream == rhs.stream &&
         lhs.start_ns == rhs.start_ns && lhs.end_ns == rhs.end_ns &&
         lhs.payload_size == rhs.payload_size &&
         lhs.file_offset == rhs.file_offset && lhs.hash == rhs.hash;
}

bool less_placement(const DistributedRecordLocation& lhs,
                    const DistributedRecordLocation& rhs) {
  if (lhs.object.store != rhs.object.store) {
    return lhs.object.store < rhs.object.store;
  }
  if (lhs.object.key != rhs.object.key) return lhs.object.key < rhs.object.key;
  if (lhs.object.version != rhs.object.version) {
    return lhs.object.version < rhs.object.version;
  }
  if (lhs.offset != rhs.offset) return lhs.offset < rhs.offset;
  return lhs.length < rhs.length;
}

bool same_placement(const DistributedRecordLocation& lhs,
                    const DistributedRecordLocation& rhs) {
  return lhs.object.store == rhs.object.store &&
         lhs.object.key == rhs.object.key &&
         lhs.object.version == rhs.object.version &&
         lhs.offset == rhs.offset && lhs.length == rhs.length;
}

Result<void> validate_index_limits(const DistributedLocationIndexLimits& limits) {
  if (limits.maximum_input_locations == 0 || limits.maximum_records == 0 ||
      limits.maximum_locations_per_record == 0 ||
      limits.maximum_metadata_bytes == 0 || limits.maximum_store_bytes == 0 ||
      limits.maximum_key_bytes == 0 || limits.maximum_version_bytes == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed location index limits must be non-zero");
  }
  return {};
}

Result<void> validate_query_limits(const DistributedLocationQueryLimits& limits) {
  if (limits.maximum_records == 0 ||
      limits.maximum_candidates_per_record == 0 ||
      limits.maximum_candidates == 0) {
    return fail(ErrorCode::invalid_argument,
                "distributed location query limits must be non-zero");
  }
  return {};
}

Result<void> validate_location_shape(
    const DistributedRecordLocation& location,
    const DistributedLocationIndexLimits& limits) {
  if (location.record.end_ns < location.record.start_ns) {
    return fail(ErrorCode::invalid_argument,
                "distributed indexed record end time precedes start time");
  }
  if (location.length != location.record.payload_size) {
    return fail(ErrorCode::invalid_argument,
                "distributed indexed location length does not match payload size");
  }
  if (location.offset >
      std::numeric_limits<std::uint64_t>::max() - location.length) {
    return fail(ErrorCode::invalid_argument,
                "distributed indexed location range overflows");
  }
  if (location.object.store.empty() || location.object.key.empty()) {
    return fail(ErrorCode::invalid_argument,
                "distributed indexed location store and key must be non-empty");
  }
  if (location.object.store.size() > limits.maximum_store_bytes ||
      location.object.key.size() > limits.maximum_key_bytes ||
      location.object.version.size() > limits.maximum_version_bytes) {
    return fail(ErrorCode::resource_exhausted,
                "distributed indexed location metadata exceeds configured limits");
  }
  return {};
}

bool add_metadata_bytes(std::uint64_t& total,
                        std::size_t amount,
                        std::uint64_t maximum) {
  const auto value = static_cast<std::uint64_t>(amount);
  if (value > maximum - total) return false;
  total += value;
  return true;
}

ProvenanceRecordLink link_for(const DistributedRecordLocation& location) {
  return detail::distributed_exact_link(location.record);
}

ProvenanceRecordLink link_for(const DistributedLocationIndexEntry& entry) {
  return detail::distributed_exact_link(entry.record);
}

}  // namespace

std::size_t DistributedLocationIndex::record_count() const noexcept {
  return entries_.size();
}

std::size_t DistributedLocationIndex::location_count() const noexcept {
  return location_count_;
}

std::span<const DistributedLocationIndexEntry>
DistributedLocationIndex::entries() const noexcept {
  return entries_;
}

Result<DistributedLocationIndex> build_distributed_location_index(
    std::span<const DistributedRecordLocation> locations,
    DistributedLocationIndexLimits limits) {
  try {
    auto valid_limits = validate_index_limits(limits);
    if (!valid_limits) return valid_limits.error();
    if (locations.size() > limits.maximum_input_locations) {
      return fail<DistributedLocationIndex>(
          ErrorCode::resource_exhausted,
          "distributed location index input-location limit exceeded");
    }

    std::uint64_t metadata_bytes = 0;
    for (const auto& location : locations) {
      auto valid_location = validate_location_shape(location, limits);
      if (!valid_location) return valid_location.error();
      if (!add_metadata_bytes(metadata_bytes, location.object.store.size(),
                              limits.maximum_metadata_bytes) ||
          !add_metadata_bytes(metadata_bytes, location.object.key.size(),
                              limits.maximum_metadata_bytes) ||
          !add_metadata_bytes(metadata_bytes, location.object.version.size(),
                              limits.maximum_metadata_bytes)) {
        return fail<DistributedLocationIndex>(
            ErrorCode::resource_exhausted,
            "distributed location index aggregate metadata limit exceeded");
      }
    }

    std::vector<DistributedRecordLocation> sorted{locations.begin(),
                                                   locations.end()};
    std::sort(sorted.begin(), sorted.end(),
              [](const DistributedRecordLocation& lhs,
                 const DistributedRecordLocation& rhs) {
                const auto lhs_link = link_for(lhs);
                const auto rhs_link = link_for(rhs);
                if (less_link(lhs_link, rhs_link)) return true;
                if (less_link(rhs_link, lhs_link)) return false;
                return less_placement(lhs, rhs);
              });

    DistributedLocationIndex index;
    index.entries_.reserve(sorted.size());
    for (const auto& location : sorted) {
      const auto location_link = link_for(location);
      if (index.entries_.empty() ||
          !same_link(link_for(index.entries_.back()), location_link)) {
        if (index.entries_.size() >= limits.maximum_records) {
          return fail<DistributedLocationIndex>(
              ErrorCode::resource_exhausted,
              "distributed location index unique-record limit exceeded");
        }
        DistributedLocationIndexEntry entry;
        entry.record = location.record;
        entry.candidates.push_back(location);
        index.entries_.push_back(std::move(entry));
        ++index.location_count_;
        continue;
      }

      auto& entry = index.entries_.back();
      if (!same_record_info(entry.record, location.record)) {
        return fail<DistributedLocationIndex>(
            ErrorCode::invalid_argument,
            "distributed location replicas disagree on original record metadata");
      }
      if (same_placement(entry.candidates.back(), location)) continue;
      if (entry.candidates.size() >= limits.maximum_locations_per_record) {
        return fail<DistributedLocationIndex>(
            ErrorCode::resource_exhausted,
            "distributed location index per-record candidate limit exceeded");
      }
      entry.candidates.push_back(location);
      ++index.location_count_;
    }

    return index;
  } catch (const std::bad_alloc&) {
    return fail<DistributedLocationIndex>(
        ErrorCode::resource_exhausted,
        "distributed location index allocation failed");
  }
}

Result<DistributedPartitionLocationCandidates>
resolve_partition_location_candidates(
    const DistributedLocationIndex& index,
    const DistributedPartition& partition,
    DistributedLocationQueryLimits limits) {
  try {
    auto valid_limits = validate_query_limits(limits);
    if (!valid_limits) return valid_limits.error();
    if (partition.records.empty()) {
      return fail<DistributedPartitionLocationCandidates>(
          ErrorCode::invalid_argument,
          "distributed location query requires a non-empty partition");
    }
    if (partition.records.size() > limits.maximum_records) {
      return fail<DistributedPartitionLocationCandidates>(
          ErrorCode::resource_exhausted,
          "distributed location query record-count limit exceeded");
    }
    for (const auto& link : partition.records) {
      if (link.stream != partition.stream) {
        return fail<DistributedPartitionLocationCandidates>(
            ErrorCode::invalid_argument,
            "distributed location query partition contains another stream");
      }
    }
    if (detail::distributed_partition_identity(partition.stream,
                                               partition.records) !=
        partition.identity) {
      return fail<DistributedPartitionLocationCandidates>(
          ErrorCode::invalid_argument,
          "distributed location query partition identity does not match membership");
    }

    DistributedPartitionLocationCandidates result{
        .partition_identity = partition.identity,
        .stream = partition.stream,
        .complete = true,
        .records = {},
    };
    result.records.reserve(partition.records.size());
    std::size_t candidate_count = 0;

    for (const auto& link : partition.records) {
      const auto found = std::lower_bound(
          index.entries_.begin(), index.entries_.end(), link,
          [](const DistributedLocationIndexEntry& entry,
             const ProvenanceRecordLink& target) {
            return less_link(link_for(entry), target);
          });

      DistributedLocationCandidateSet set;
      set.record = link;
      if (found != index.entries_.end() && same_link(link_for(*found), link)) {
        if (found->candidates.size() > limits.maximum_candidates_per_record) {
          return fail<DistributedPartitionLocationCandidates>(
              ErrorCode::resource_exhausted,
              "distributed location query per-record candidate limit exceeded");
        }
        if (found->candidates.size() >
            limits.maximum_candidates - candidate_count) {
          return fail<DistributedPartitionLocationCandidates>(
              ErrorCode::resource_exhausted,
              "distributed location query aggregate candidate limit exceeded");
        }
        candidate_count += found->candidates.size();
        set.candidates = found->candidates;
      } else {
        result.complete = false;
      }
      result.records.push_back(std::move(set));
    }

    return result;
  } catch (const std::bad_alloc&) {
    return fail<DistributedPartitionLocationCandidates>(
        ErrorCode::resource_exhausted,
        "distributed location query allocation failed");
  }
}

}  // namespace codec
