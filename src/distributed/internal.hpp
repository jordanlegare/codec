#pragma once

#include <codec/archive.hpp>
#include <codec/integrity.hpp>

#include <span>

namespace codec::detail {

ProvenanceRecordLink distributed_exact_link(const ExtractedRecord& input);
Sha256 distributed_partition_identity(
    StreamId stream,
    std::span<const ProvenanceRecordLink> records);

}  // namespace codec::detail
