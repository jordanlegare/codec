#pragma once

#include <codec/archive.hpp>
#include <codec/result.hpp>
#include <codec/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace codec {

struct SourceExactCursor {
  std::uint64_t next_archive_sequence{};
};

struct SourceExactFollowBatch {
  std::vector<ExtractedRecord> records;
  SourceExactCursor cursor;
  bool finalized{false};
};

Result<SourceExactFollowBatch> extract_stream_source_exact_prefix(
    const CodaArchive& archive, const StreamId& stream,
    SourceExactCursor cursor = {}, std::size_t maximum_records = 1024,
    std::uint64_t maximum_bytes = 64ULL * 1024ULL * 1024ULL);

}  // namespace codec
