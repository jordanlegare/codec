#include <codec/profiles/video.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::video {
namespace {

constexpr std::string_view kDirectOperation =
    "codec.video.raw-frame.canonicalize";
constexpr std::string_view kHlsOperation =
    "codec.video.raw-frame.canonicalize.hls";
constexpr std::string_view kImplementation = "codec.video";
constexpr std::string_view kImplementationVersion = "1";
constexpr std::string_view kDirectDetailsType =
    "application/vnd.codec.video.canonicalization.v1";
constexpr std::string_view kHlsDetailsType =
    "application/vnd.codec.video.hls-canonicalization.v1";

bool matches_link(const RecordInfo& record, const ProvenanceRecordLink& link) {
  return record.stream == link.stream && record.type_code() == link.type &&
         record.sequence == link.sequence && record.hash == link.hash;
}

Result<RecordInfo> resolve_record(const std::vector<RecordInfo>& records,
                                  const ProvenanceRecordLink& link,
                                  std::string_view label) {
  const auto found =
      std::find_if(records.begin(), records.end(), [&link](const auto& record) {
        return matches_link(record, link);
      });
  if (found == records.end()) {
    return fail<RecordInfo>(ErrorCode::archive_corrupt,
                            std::string{"verified video frame "} +
                                std::string{label} + " link is invalid");
  }
  return *found;
}

bool intervals_overlap(const RecordInfo& left, const RecordInfo& right) {
  return left.start_ns < right.end_ns && right.start_ns < left.end_ns;
}

enum class VideoProvenanceContract { direct, hls };

Result<VideoProvenanceContract> classify_video_process(
    const ProvenanceProcess& process) {
  const bool common = process.implementation_id == kImplementation &&
                      process.implementation_version ==
                          kImplementationVersion &&
                      process.details.size() == 1 &&
                      process.details.front() == std::byte{0x01};
  if (common && process.operation == kDirectOperation &&
      process.details_type == kDirectDetailsType) {
    return VideoProvenanceContract::direct;
  }
  if (common && process.operation == kHlsOperation &&
      process.details_type == kHlsDetailsType) {
    return VideoProvenanceContract::hls;
  }
  return fail<VideoProvenanceContract>(
      ErrorCode::archive_corrupt,
      "verified video frame process violates the Video Profile contract");
}

struct Candidate {
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

}  // namespace

Result<std::vector<VerifiedRawVideoFrame>> query_verified_raw_video_frames(
    const CodaArchive& archive, const VideoFrameQuery& query) {
  if (query.maximum_results == 0 || query.maximum_encoded_bytes == 0 ||
      query.decode_limits.maximum_width == 0 ||
      query.decode_limits.maximum_height == 0 ||
      query.decode_limits.maximum_pixels == 0 ||
      query.decode_limits.maximum_payload_bytes == 0) {
    return fail<std::vector<VerifiedRawVideoFrame>>(
        ErrorCode::invalid_argument,
        "video frame query limits must all be non-zero");
  }

  const auto verification = archive.verify();
  if (!verification.ok) {
    return fail<std::vector<VerifiedRawVideoFrame>>(
        verification.error_code, verification.message);
  }
  if (!verification.finalized) {
    return fail<std::vector<VerifiedRawVideoFrame>>(
        ErrorCode::archive_corrupt,
        "verified video frame query requires a finalized archive");
  }

  const RecordQuery subject{
      .stream = query.stream,
      .type = raw_video_frame_state_record_type,
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
    return fail<std::vector<VerifiedRawVideoFrame>>(
        ErrorCode::resource_exhausted,
        "verified video frame count exceeds the configured limit");
  }

  for (std::size_t index = 0; index < selected->size(); ++index) {
    for (std::size_t prior = 0; prior < index; ++prior) {
      if ((*selected)[index].subject.stream ==
              (*selected)[prior].subject.stream &&
          (*selected)[index].subject.type == (*selected)[prior].subject.type &&
          (*selected)[index].subject.sequence ==
              (*selected)[prior].subject.sequence &&
          (*selected)[index].subject.hash == (*selected)[prior].subject.hash) {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            "verified video frame has duplicate S1 provenance");
      }
    }
  }

  auto records = archive.records();
  if (!records) return records.error();
  std::optional<std::vector<StreamDescriptor>> stream_descriptors;

  std::vector<Candidate> candidates;
  candidates.reserve(selected->size());
  std::uint64_t encoded_bytes = 0;
  for (auto provenance : *selected) {
    if (provenance.inputs.empty()) {
      return fail<std::vector<VerifiedRawVideoFrame>>(
          ErrorCode::archive_corrupt,
          "verified video frame must have at least one direct S0 input");
    }
    auto contract = classify_video_process(provenance.process);
    if (!contract) return contract.error();

    auto state_record = resolve_record(*records, provenance.subject, "subject");
    if (!state_record) return state_record.error();
    if (state_record->type_code() != raw_video_frame_state_record_type) {
      return fail<std::vector<VerifiedRawVideoFrame>>(
          ErrorCode::archive_corrupt,
          "verified video frame subject has the wrong profile type");
    }

    std::vector<RecordInfo> source_records;
    source_records.reserve(provenance.inputs.size());
    if (*contract == VideoProvenanceContract::hls &&
        provenance.inputs.size() < 2) {
      return fail<std::vector<VerifiedRawVideoFrame>>(
          ErrorCode::archive_corrupt,
          "verified HLS video frame requires a primary and child S0 input");
    }
    if (*contract == VideoProvenanceContract::hls &&
        !stream_descriptors.has_value()) {
      auto loaded = archive.streams();
      if (!loaded) return loaded.error();
      stream_descriptors = std::move(*loaded);
    }

    std::vector<StreamId> hls_child_streams;
    for (std::size_t input_index = 0;
         input_index < provenance.inputs.size(); ++input_index) {
      const auto& input_link = provenance.inputs[input_index];
      if (input_link.stream == provenance.subject.stream &&
          input_link.type == provenance.subject.type &&
          input_link.sequence == provenance.subject.sequence &&
          input_link.hash == provenance.subject.hash) {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            "verified video frame provenance is self-referential");
      }
      auto source_record = resolve_record(*records, input_link, "source");
      if (!source_record) return source_record.error();

      if (*contract == VideoProvenanceContract::direct) {
        if (source_record->type != RecordType::source_bytes ||
            source_record->stream != state_record->stream ||
            !intervals_overlap(*source_record, *state_record)) {
          return fail<std::vector<VerifiedRawVideoFrame>>(
              ErrorCode::archive_corrupt,
              "verified video frame lineage violates the Video Profile contract");
        }
        source_records.push_back(*source_record);
        continue;
      }

      if (input_index == 0U) {
        if (source_record->type != RecordType::source_bytes ||
            source_record->stream != state_record->stream ||
            !intervals_overlap(*source_record, *state_record)) {
          return fail<std::vector<VerifiedRawVideoFrame>>(
              ErrorCode::archive_corrupt,
              "verified HLS video frame primary lineage is invalid");
        }
        source_records.push_back(*source_record);
        continue;
      }

      if (source_record->type != RecordType::source_bytes ||
          source_record->stream == state_record->stream ||
          !intervals_overlap(*source_record, *state_record)) {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            "verified HLS video frame child lineage is invalid");
      }

      if (std::find(hls_child_streams.begin(), hls_child_streams.end(),
                    source_record->stream) != hls_child_streams.end()) {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            "verified HLS video frame repeats a child stream");
      }
      hls_child_streams.push_back(source_record->stream);

      std::size_t descriptor_count = 0;
      const StreamDescriptor* child_descriptor = nullptr;
      for (const auto& descriptor : *stream_descriptors) {
        if (descriptor.id != source_record->stream) continue;
        ++descriptor_count;
        child_descriptor = &descriptor;
      }
      if (descriptor_count != 1U || child_descriptor == nullptr ||
          child_descriptor->type != StreamType::opaque ||
          child_descriptor->source_id != "codec.video.hls-resource") {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            "verified HLS video frame child descriptor is invalid");
      }
      source_records.push_back(*source_record);
    }

    if (encoded_bytes > query.maximum_encoded_bytes ||
        state_record->payload_size >
            query.maximum_encoded_bytes - encoded_bytes) {
      return fail<std::vector<VerifiedRawVideoFrame>>(
          ErrorCode::resource_exhausted,
          "verified video frame payloads exceed the configured limit");
    }
    encoded_bytes += state_record->payload_size;
    candidates.push_back(Candidate{
        .state_record = *state_record,
        .source_records = std::move(source_records),
        .provenance = std::move(provenance),
    });
  }

  std::vector<VerifiedRawVideoFrame> output;
  output.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto payload = archive.read_payload(candidate.state_record);
    if (!payload) return payload.error();
    auto state = decode_raw_video_frame_state(*payload, query.decode_limits);
    if (!state) {
      if (state.error().code == ErrorCode::decode) {
        return fail<std::vector<VerifiedRawVideoFrame>>(
            ErrorCode::archive_corrupt,
            std::string{"verified video frame payload is invalid: "} +
                state.error().message);
      }
      return state.error();
    }
    output.push_back(VerifiedRawVideoFrame{
        .state = std::move(*state),
        .state_record = candidate.state_record,
        .source_records = std::move(candidate.source_records),
        .provenance = std::move(candidate.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::video
