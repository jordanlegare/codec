#include <codec/profiles/video.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::video {
namespace {

constexpr std::string_view kDirectOperation =
    "codec.video.encoded-video.preserve";
constexpr std::string_view kHlsOperation =
    "codec.video.encoded-video.preserve.hls";
constexpr std::string_view kImplementation = "codec.video";
constexpr std::string_view kImplementationVersion = "1";
constexpr std::string_view kDirectDetailsType =
    "application/vnd.codec.video.encoded-video.v1";
constexpr std::string_view kHlsDetailsType =
    "application/vnd.codec.video.hls-encoded-video.v1";

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
                            std::string{"verified encoded video "} +
                                std::string{label} + " link is invalid");
  }
  return *found;
}

bool intervals_overlap(const RecordInfo& left, const RecordInfo& right) {
  return left.start_ns < right.end_ns && right.start_ns < left.end_ns;
}

enum class ProvenanceContract { direct, hls };

Result<ProvenanceContract> classify_process(const ProvenanceProcess& process) {
  const bool common = process.implementation_id == kImplementation &&
                      process.implementation_version ==
                          kImplementationVersion &&
                      process.details.size() == 1U &&
                      process.details.front() == std::byte{0x01};
  if (common && process.operation == kDirectOperation &&
      process.details_type == kDirectDetailsType) {
    return ProvenanceContract::direct;
  }
  if (common && process.operation == kHlsOperation &&
      process.details_type == kHlsDetailsType) {
    return ProvenanceContract::hls;
  }
  return fail<ProvenanceContract>(
      ErrorCode::archive_corrupt,
      "verified encoded video process violates the Video Profile contract");
}

struct Candidate {
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<void> validate_record_interval(const RecordInfo& record,
                                      const EncodedVideoState& state) {
  if (record.end_ns <= record.start_ns) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video state interval is invalid");
  }
  const auto duration = static_cast<std::uint64_t>(record.end_ns -
                                                   record.start_ns);
  if (state.presentation_lead_ns >= duration) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video presentation lead exceeds its interval");
  }
  return {};
}

}  // namespace

Result<std::vector<VerifiedVideoEncodedVideo>>
query_verified_video_encoded_video(const CodaArchive& archive,
                                   const VideoFrameQuery& query) {
  if (query.maximum_results == 0U || query.maximum_encoded_bytes == 0U) {
    return fail<std::vector<VerifiedVideoEncodedVideo>>(
        ErrorCode::invalid_argument,
        "encoded video query limits must be non-zero");
  }

  const auto verification = archive.verify();
  if (!verification.ok) {
    return fail<std::vector<VerifiedVideoEncodedVideo>>(
        verification.error_code, verification.message);
  }
  if (!verification.finalized) {
    return fail<std::vector<VerifiedVideoEncodedVideo>>(
        ErrorCode::archive_corrupt,
        "verified encoded video query requires a finalized archive");
  }

  const RecordQuery subject{
      .stream = query.stream,
      .type = video_encoded_video_state_record_type,
      .sequence = std::nullopt,
      .time = query.time,
  };
  auto selected = archive.query_provenance(ProvenanceQuery{
      .subject_truth = TruthClass::state_exact,
      .subject = subject,
      .direct_input = std::nullopt,
  });
  if (!selected) return selected.error();
  if (selected->size() > query.maximum_results) {
    return fail<std::vector<VerifiedVideoEncodedVideo>>(
        ErrorCode::resource_exhausted,
        "verified encoded video count exceeds the configured limit");
  }

  for (std::size_t index = 0U; index < selected->size(); ++index) {
    for (std::size_t prior = 0U; prior < index; ++prior) {
      const auto& left = (*selected)[index].subject;
      const auto& right = (*selected)[prior].subject;
      if (left.stream == right.stream && left.type == right.type &&
          left.sequence == right.sequence && left.hash == right.hash) {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            "verified encoded video has duplicate S1 provenance");
      }
    }
  }

  auto records = archive.records();
  if (!records) return records.error();
  auto descriptors = archive.streams();
  if (!descriptors) return descriptors.error();

  std::vector<Candidate> candidates;
  candidates.reserve(selected->size());
  std::uint64_t encoded_bytes = 0U;
  for (auto provenance : *selected) {
    if (provenance.inputs.empty()) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::archive_corrupt,
          "verified encoded video must have direct S0 input");
    }
    auto contract = classify_process(provenance.process);
    if (!contract) return contract.error();
    if (*contract == ProvenanceContract::direct &&
        provenance.inputs.size() != 1U) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::archive_corrupt,
          "verified direct encoded video must have exactly one S0 input");
    }
    if (*contract == ProvenanceContract::hls &&
        provenance.inputs.size() < 2U) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::archive_corrupt,
          "verified HLS encoded video requires primary and child S0 inputs");
    }

    auto state_record = resolve_record(*records, provenance.subject, "subject");
    if (!state_record) return state_record.error();
    if (state_record->type_code() != video_encoded_video_state_record_type) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::archive_corrupt,
          "verified encoded video subject has the wrong profile type");
    }

    std::size_t parent_descriptor_count = 0U;
    const StreamDescriptor* parent_descriptor = nullptr;
    for (const auto& descriptor : *descriptors) {
      if (descriptor.id != state_record->stream) continue;
      ++parent_descriptor_count;
      parent_descriptor = &descriptor;
    }
    if (parent_descriptor_count != 1U || parent_descriptor == nullptr ||
        parent_descriptor->type != StreamType::video) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::archive_corrupt,
          "verified encoded video stream descriptor is invalid");
    }

    std::vector<RecordInfo> source_records;
    source_records.reserve(provenance.inputs.size());
    std::vector<StreamId> hls_child_streams;
    for (std::size_t input_index = 0U;
         input_index < provenance.inputs.size(); ++input_index) {
      const auto& link = provenance.inputs[input_index];
      if (link.stream == provenance.subject.stream &&
          link.type == provenance.subject.type &&
          link.sequence == provenance.subject.sequence &&
          link.hash == provenance.subject.hash) {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            "verified encoded video provenance is self-referential");
      }
      auto source = resolve_record(*records, link, "source");
      if (!source) return source.error();
      if (source->type != RecordType::source_bytes ||
          !intervals_overlap(*source, *state_record)) {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            "verified encoded video source lineage is invalid");
      }

      if (input_index == 0U) {
        if (source->stream != state_record->stream) {
          return fail<std::vector<VerifiedVideoEncodedVideo>>(
              ErrorCode::archive_corrupt,
              "verified encoded video primary source stream is invalid");
        }
        source_records.push_back(*source);
        continue;
      }

      if (*contract != ProvenanceContract::hls ||
          source->stream == state_record->stream ||
          std::find(hls_child_streams.begin(), hls_child_streams.end(),
                    source->stream) != hls_child_streams.end()) {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            "verified encoded video child lineage is invalid");
      }
      hls_child_streams.push_back(source->stream);

      std::size_t child_descriptor_count = 0U;
      const StreamDescriptor* child_descriptor = nullptr;
      for (const auto& descriptor : *descriptors) {
        if (descriptor.id != source->stream) continue;
        ++child_descriptor_count;
        child_descriptor = &descriptor;
      }
      if (child_descriptor_count != 1U || child_descriptor == nullptr ||
          child_descriptor->type != StreamType::opaque ||
          child_descriptor->source_id != "codec.video.hls-resource") {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            "verified encoded video HLS child descriptor is invalid");
      }
      source_records.push_back(*source);
    }

    if (encoded_bytes > query.maximum_encoded_bytes ||
        state_record->payload_size >
            query.maximum_encoded_bytes - encoded_bytes) {
      return fail<std::vector<VerifiedVideoEncodedVideo>>(
          ErrorCode::resource_exhausted,
          "verified encoded video payloads exceed the configured limit");
    }
    encoded_bytes += state_record->payload_size;
    candidates.push_back(Candidate{.state_record = *state_record,
                                   .source_records = std::move(source_records),
                                   .provenance = std::move(provenance)});
  }

  std::vector<VerifiedVideoEncodedVideo> output;
  output.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto payload = archive.read_payload(candidate.state_record);
    if (!payload) return payload.error();
    EncodedVideoDecodeLimits limits{};
    limits.maximum_payload_bytes = query.maximum_encoded_bytes;
    limits.maximum_decoder_config_bytes =
        std::min(limits.maximum_decoder_config_bytes,
                 query.maximum_encoded_bytes);
    limits.maximum_packet_bytes =
        std::min(limits.maximum_packet_bytes, query.maximum_encoded_bytes);
    auto state = decode_encoded_video_state(*payload, limits);
    if (!state) {
      if (state.error().code == ErrorCode::decode) {
        return fail<std::vector<VerifiedVideoEncodedVideo>>(
            ErrorCode::archive_corrupt,
            std::string{"verified encoded video payload is invalid: "} +
                state.error().message);
      }
      return state.error();
    }
    auto interval = validate_record_interval(candidate.state_record, *state);
    if (!interval) return interval.error();
    output.push_back(VerifiedVideoEncodedVideo{
        .state = std::move(*state),
        .state_record = candidate.state_record,
        .source_records = std::move(candidate.source_records),
        .provenance = std::move(candidate.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::video
