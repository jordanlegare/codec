#include <codec/profiles/video.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::profiles::video {
namespace {

constexpr std::string_view kDirectOperation =
    "codec.video.encoded-audio.preserve";
constexpr std::string_view kHlsOperation =
    "codec.video.encoded-audio.preserve.hls";
constexpr std::string_view kImplementation = "codec.video";
constexpr std::string_view kImplementationVersion = "1";
constexpr std::string_view kDirectDetailsType =
    "application/vnd.codec.video.encoded-audio.v1";
constexpr std::string_view kHlsDetailsType =
    "application/vnd.codec.video.hls-encoded-audio.v1";
constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

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
                            std::string{"verified encoded video audio "} +
                                std::string{label} + " link is invalid");
  }
  return *found;
}

bool intervals_overlap(const RecordInfo& left, const RecordInfo& right) {
  return left.start_ns < right.end_ns && right.start_ns < left.end_ns;
}

enum class ProvenanceContract { direct, hls };

Result<ProvenanceContract> classify_process(
    const ProvenanceProcess& process) {
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
      "verified encoded video audio process violates the H.1 contract");
}

Result<void> require_unique_video_descriptor(
    const std::vector<StreamDescriptor>& descriptors, const StreamId& stream) {
  std::size_t count = 0U;
  const StreamDescriptor* match = nullptr;
  for (const auto& descriptor : descriptors) {
    if (descriptor.id != stream) continue;
    ++count;
    match = &descriptor;
  }
  if (count != 1U || match == nullptr || match->type != StreamType::video) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video audio stream descriptor is invalid");
  }
  return {};
}

Result<void> require_hls_child_descriptor(
    const std::vector<StreamDescriptor>& descriptors, const StreamId& stream) {
  std::size_t count = 0U;
  const StreamDescriptor* match = nullptr;
  for (const auto& descriptor : descriptors) {
    if (descriptor.id != stream) continue;
    ++count;
    match = &descriptor;
  }
  if (count != 1U || match == nullptr || match->type != StreamType::opaque ||
      match->source_id != "codec.video.hls-resource") {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded HLS audio child descriptor is invalid");
  }
  return {};
}

Result<void> validate_interval(const EncodedAudioState& state,
                               const RecordInfo& record) {
  if (record.end_ns <= record.start_ns || state.sample_rate == 0U ||
      state.presentation_frames == 0U) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video audio interval is invalid");
  }
  const auto seconds = state.presentation_frames / state.sample_rate;
  const auto remainder = state.presentation_frames % state.sample_rate;
  if (seconds > std::numeric_limits<std::uint64_t>::max() /
                    kNanosecondsPerSecond) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video audio duration exceeds bounds");
  }
  const auto expected =
      seconds * kNanosecondsPerSecond +
      (remainder * kNanosecondsPerSecond) / state.sample_rate;
  const auto duration =
      static_cast<std::uint64_t>(record.end_ns - record.start_ns);
  const auto difference =
      duration > expected ? duration - expected : expected - duration;
  if (difference > 1U) {
    return fail(ErrorCode::archive_corrupt,
                "verified encoded video audio interval does not match its presentation");
  }
  return {};
}

struct Candidate {
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

}  // namespace

Result<std::vector<VerifiedVideoEncodedAudio>>
query_verified_video_encoded_audio(const CodaArchive& archive,
                                   const VideoAudioQuery& query) {
  if (query.maximum_results == 0U || query.maximum_encoded_bytes == 0U) {
    return fail<std::vector<VerifiedVideoEncodedAudio>>(
        ErrorCode::invalid_argument,
        "encoded video audio query limits must be non-zero");
  }
  const auto verification = archive.verify();
  if (!verification.ok) {
    return fail<std::vector<VerifiedVideoEncodedAudio>>(
        verification.error_code, verification.message);
  }
  if (!verification.finalized) {
    return fail<std::vector<VerifiedVideoEncodedAudio>>(
        ErrorCode::archive_corrupt,
        "verified encoded video audio query requires a finalized archive");
  }

  const RecordQuery scoped_subject{
      .stream = query.stream,
      .type = video_encoded_audio_state_record_type,
      .sequence = std::nullopt,
      .time = std::nullopt,
  };
  auto scoped_states = archive.query_records(scoped_subject);
  if (!scoped_states) return scoped_states.error();
  std::vector<StreamId> scoped_streams;
  for (const auto& state : *scoped_states) {
    if (std::find(scoped_streams.begin(), scoped_streams.end(), state.stream) !=
        scoped_streams.end()) {
      return fail<std::vector<VerifiedVideoEncodedAudio>>(
          ErrorCode::archive_corrupt,
          "H.1 v1 permits at most one encoded audio state per stream");
    }
    scoped_streams.push_back(state.stream);
  }

  const RecordQuery subject{
      .stream = query.stream,
      .type = video_encoded_audio_state_record_type,
      .sequence = std::nullopt,
      .time = query.time,
  };
  auto requested_states = archive.query_records(subject);
  if (!requested_states) return requested_states.error();
  if (requested_states->size() > query.maximum_results) {
    return fail<std::vector<VerifiedVideoEncodedAudio>>(
        ErrorCode::resource_exhausted,
        "verified encoded video audio count exceeds configured limit");
  }
  auto provenance = archive.query_provenance(ProvenanceQuery{
      .subject_truth = std::nullopt,
      .subject = subject,
      .direct_input = std::nullopt,
  });
  if (!provenance) return provenance.error();
  auto records = archive.records();
  if (!records) return records.error();
  auto descriptors = archive.streams();
  if (!descriptors) return descriptors.error();

  std::vector<Candidate> candidates;
  candidates.reserve(requested_states->size());
  std::uint64_t encoded_bytes = 0U;
  for (const auto& state : *requested_states) {
    const StreamProvenance* match = nullptr;
    std::size_t match_count = 0U;
    for (const auto& candidate : *provenance) {
      if (!matches_link(state, candidate.subject)) continue;
      ++match_count;
      match = &candidate;
    }
    if (match_count != 1U || match == nullptr ||
        match->subject_truth != TruthClass::state_exact) {
      return fail<std::vector<VerifiedVideoEncodedAudio>>(
          ErrorCode::archive_corrupt,
          "purported encoded H.1 audio lacks unique exact provenance");
    }
    auto contract = classify_process(match->process);
    if (!contract) return contract.error();
    if ((*contract == ProvenanceContract::direct &&
         match->inputs.size() != 1U) ||
        (*contract == ProvenanceContract::hls &&
         match->inputs.size() < 2U)) {
      return fail<std::vector<VerifiedVideoEncodedAudio>>(
          ErrorCode::archive_corrupt,
          "verified encoded H.1 audio source frontier is invalid");
    }
    auto descriptor = require_unique_video_descriptor(*descriptors, state.stream);
    if (!descriptor) return descriptor.error();

    std::vector<RecordInfo> sources;
    sources.reserve(match->inputs.size());
    std::vector<StreamId> child_streams;
    for (std::size_t index = 0U; index < match->inputs.size(); ++index) {
      const auto& link = match->inputs[index];
      if (link.stream == match->subject.stream &&
          link.type == match->subject.type &&
          link.sequence == match->subject.sequence &&
          link.hash == match->subject.hash) {
        return fail<std::vector<VerifiedVideoEncodedAudio>>(
            ErrorCode::archive_corrupt,
            "verified encoded video audio provenance is self-referential");
      }
      auto source = resolve_record(*records, link, "source");
      if (!source) return source.error();
      if (source->type != RecordType::source_bytes ||
          !intervals_overlap(*source, state)) {
        return fail<std::vector<VerifiedVideoEncodedAudio>>(
            ErrorCode::archive_corrupt,
            "verified encoded video audio source lineage is invalid");
      }
      if (index == 0U) {
        if (source->stream != state.stream) {
          return fail<std::vector<VerifiedVideoEncodedAudio>>(
              ErrorCode::archive_corrupt,
              "verified encoded video audio primary lineage is invalid");
        }
      } else {
        if (*contract != ProvenanceContract::hls ||
            source->stream == state.stream ||
            std::find(child_streams.begin(), child_streams.end(),
                      source->stream) != child_streams.end()) {
          return fail<std::vector<VerifiedVideoEncodedAudio>>(
              ErrorCode::archive_corrupt,
              "verified encoded HLS audio child lineage is invalid");
        }
        auto child_descriptor =
            require_hls_child_descriptor(*descriptors, source->stream);
        if (!child_descriptor) return child_descriptor.error();
        child_streams.push_back(source->stream);
      }
      sources.push_back(*source);
    }
    if (encoded_bytes > query.maximum_encoded_bytes ||
        state.payload_size > query.maximum_encoded_bytes - encoded_bytes) {
      return fail<std::vector<VerifiedVideoEncodedAudio>>(
          ErrorCode::resource_exhausted,
          "verified encoded video audio payloads exceed configured limit");
    }
    encoded_bytes += state.payload_size;
    candidates.push_back(Candidate{.state_record = state,
                                   .source_records = std::move(sources),
                                   .provenance = *match});
  }

  std::vector<VerifiedVideoEncodedAudio> output;
  output.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto payload = archive.read_payload(candidate.state_record);
    if (!payload) return payload.error();
    auto state = decode_encoded_audio_state(
        *payload,
        EncodedAudioDecodeLimits{
            .maximum_packets = 1'000'000,
            .maximum_decoder_config_bytes = query.maximum_encoded_bytes,
            .maximum_packet_bytes = query.maximum_encoded_bytes,
            .maximum_payload_bytes = query.maximum_encoded_bytes,
        });
    if (!state) {
      return fail<std::vector<VerifiedVideoEncodedAudio>>(
          ErrorCode::archive_corrupt,
          std::string{"verified encoded video audio payload is invalid: "} +
              state.error().message);
    }
    auto interval = validate_interval(*state, candidate.state_record);
    if (!interval) return interval.error();
    output.push_back(VerifiedVideoEncodedAudio{
        .state = std::move(*state),
        .state_record = candidate.state_record,
        .source_records = std::move(candidate.source_records),
        .provenance = std::move(candidate.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::video
