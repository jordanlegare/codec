#include <codec/archive_follow.hpp>

#include <limits>
#include <utility>
#include <vector>

namespace codec {

Result<SourceExactFollowBatch> extract_stream_source_exact_prefix(
    const CodaArchive& archive, const StreamId& stream,
    SourceExactCursor cursor, std::size_t maximum_records,
    std::uint64_t maximum_bytes) {
  if (maximum_records == 0 || maximum_bytes == 0) {
    return fail<SourceExactFollowBatch>(
        ErrorCode::invalid_argument,
        "follow extraction limits must be nonzero");
  }

  auto record_list = archive.records(ArchiveReadPolicy::verified_prefix);
  if (!record_list) return record_list.error();

  std::vector<RecordInfo> selected;
  selected.reserve((std::min)(maximum_records, record_list->size()));
  std::uint64_t selected_bytes = 0;
  SourceExactCursor next = cursor;
  bool finalized = false;

  for (const auto& record : *record_list) {
    if (record.sequence < cursor.next_archive_sequence) continue;

    const bool selected_source =
        record.stream == stream && record.type == RecordType::source_bytes;
    if (selected_source) {
      if (selected.size() == maximum_records) break;
      if (record.payload_size > maximum_bytes - selected_bytes) {
        if (selected.empty()) {
          return fail<SourceExactFollowBatch>(
              ErrorCode::resource_exhausted,
              "follow extraction byte limit is smaller than the next source record");
        }
        break;
      }
    }

    if (record.sequence == (std::numeric_limits<std::uint64_t>::max)()) {
      return fail<SourceExactFollowBatch>(
          ErrorCode::resource_exhausted,
          "archive sequence cursor cannot advance beyond uint64 maximum");
    }
    next.next_archive_sequence = record.sequence + 1;

    if (record.type == RecordType::final_index) {
      finalized = true;
      continue;
    }
    if (!selected_source) continue;

    selected_bytes += record.payload_size;
    selected.push_back(record);
    if (selected.size() == maximum_records || selected_bytes == maximum_bytes) {
      break;
    }
  }

  SourceExactFollowBatch output;
  output.cursor = next;
  output.finalized = finalized;
  output.records.reserve(selected.size());
  for (const auto& record : selected) {
    auto payload = archive.read_payload(record);
    if (!payload) return payload.error();
    output.records.push_back(ExtractedRecord{record, std::move(*payload)});
  }
  return output;
}

}  // namespace codec
