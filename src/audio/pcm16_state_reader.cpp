#include <codec/profiles/audio_state_reader.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::audio {
namespace {

bool matches_link(const RecordInfo& record, const ProvenanceRecordLink& link) {
  return record.stream == link.stream && record.type_code() == link.type &&
         record.sequence == link.sequence && record.hash == link.hash;
}

Result<RecordInfo> resolve_record(const std::vector<RecordInfo>& records,
                                  const ProvenanceRecordLink& link,
                                  std::string_view label) {
  const auto match =
      std::find_if(records.begin(), records.end(), [&link](const auto& record) {
        return matches_link(record, link);
      });
  if (match == records.end()) {
    return fail<RecordInfo>(ErrorCode::archive_corrupt,
                            std::string{"verified PCM16 "} +
                                std::string{label} + " link is invalid");
  }
  return *match;
}

struct Candidate {
  RecordInfo state_record;
  RecordInfo source_record;
  StreamProvenance provenance;
};

}  // namespace

Result<std::vector<VerifiedPcm16State>> query_verified_pcm16_states(
    const CodaArchive& archive, const Pcm16StateQuery& query) {
  if (query.maximum_results == 0 || query.maximum_encoded_bytes == 0) {
    return fail<std::vector<VerifiedPcm16State>>(
        ErrorCode::invalid_argument,
        "PCM16 state query limits must be non-zero");
  }

  const auto verification = archive.verify();
  if (!verification.ok) {
    return fail<std::vector<VerifiedPcm16State>>(
        verification.error_code, verification.message);
  }
  if (!verification.finalized) {
    return fail<std::vector<VerifiedPcm16State>>(
        ErrorCode::archive_corrupt,
        "verified PCM16 state query requires a finalized archive");
  }

  const RecordQuery subject{
      .stream = query.stream,
      .type = record_type_code(RecordType::pcm16),
      .sequence = std::nullopt,
      .time = query.time,
  };
  const ProvenanceQuery provenance_query{
      .subject_truth = TruthClass::state_exact,
      .subject = subject,
      .direct_input = std::nullopt,
  };
  auto selected = archive.query_provenance(provenance_query);
  if (!selected) return selected.error();
  if (selected->size() > query.maximum_results) {
    return fail<std::vector<VerifiedPcm16State>>(
        ErrorCode::resource_exhausted,
        "verified PCM16 state count exceeds the configured limit");
  }

  auto records = archive.records();
  if (!records) return records.error();

  std::vector<Candidate> candidates;
  candidates.reserve(selected->size());
  std::uint64_t encoded_bytes = 0;
  for (const auto& provenance : *selected) {
    if (provenance.inputs.size() != 1) {
      return fail<std::vector<VerifiedPcm16State>>(
          ErrorCode::archive_corrupt,
          "verified PCM16 state must have exactly one direct source input");
    }

    auto state_record = resolve_record(*records, provenance.subject, "subject");
    if (!state_record) return state_record.error();
    auto source_record =
        resolve_record(*records, provenance.inputs.front(), "source");
    if (!source_record) return source_record.error();

    if (state_record->type != RecordType::pcm16 ||
        source_record->type != RecordType::source_bytes ||
        state_record->stream != source_record->stream ||
        state_record->start_ns != source_record->start_ns ||
        state_record->end_ns != source_record->end_ns) {
      return fail<std::vector<VerifiedPcm16State>>(
          ErrorCode::archive_corrupt,
          "verified PCM16 state lineage violates the Audio Profile contract");
    }

    if (encoded_bytes > query.maximum_encoded_bytes ||
        state_record->payload_size > query.maximum_encoded_bytes - encoded_bytes) {
      return fail<std::vector<VerifiedPcm16State>>(
          ErrorCode::resource_exhausted,
          "verified PCM16 state payloads exceed the configured limit");
    }
    encoded_bytes += state_record->payload_size;
    candidates.push_back(Candidate{
        .state_record = *state_record,
        .source_record = *source_record,
        .provenance = provenance,
    });
  }

  std::vector<VerifiedPcm16State> output;
  output.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto payload = archive.read_payload(candidate.state_record);
    if (!payload) return payload.error();
    auto state = decode_pcm16_state(*payload);
    if (!state) return state.error();
    output.push_back(VerifiedPcm16State{
        .state = std::move(*state),
        .state_record = candidate.state_record,
        .source_record = candidate.source_record,
        .provenance = std::move(candidate.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::audio
