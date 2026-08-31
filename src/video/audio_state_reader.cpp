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
    "codec.video.pcm16.canonicalize";
constexpr std::string_view kHlsOperation =
    "codec.video.pcm16.canonicalize.hls";
constexpr std::string_view kImplementation = "codec.video";
constexpr std::string_view kImplementationVersion = "1";
constexpr std::string_view kDirectDetailsType =
    "application/vnd.codec.video.audio-canonicalization.v1";
constexpr std::string_view kHlsDetailsType =
    "application/vnd.codec.video.hls-audio-canonicalization.v1";
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
                            std::string{"verified video audio "} +
                                std::string{label} + " link is invalid");
  }
  return *found;
}

bool intervals_overlap(const RecordInfo& left, const RecordInfo& right) {
  return left.start_ns < right.end_ns && right.start_ns < left.end_ns;
}

enum class VideoAudioProvenanceContract { direct, hls };

Result<VideoAudioProvenanceContract> classify_process(
    const ProvenanceProcess& process) {
  const bool common = process.implementation_id == kImplementation &&
                      process.implementation_version ==
                          kImplementationVersion &&
                      process.details.size() == 1U &&
                      process.details.front() == std::byte{0x01};
  if (common && process.operation == kDirectOperation &&
      process.details_type == kDirectDetailsType) {
    return VideoAudioProvenanceContract::direct;
  }
  if (common && process.operation == kHlsOperation &&
      process.details_type == kHlsDetailsType) {
    return VideoAudioProvenanceContract::hls;
  }
  return fail<VideoAudioProvenanceContract>(
      ErrorCode::archive_corrupt,
      "verified video audio process violates the H.1 contract");
}

Result<void> validate_pcm_interval(const Pcm16State& state,
                                   const RecordInfo& record) {
  if (state.sample_rate == 0U || state.channels == 0U || state.channels > 2U ||
      state.samples.empty() ||
      state.samples.size() % static_cast<std::size_t>(state.channels) != 0U) {
    return fail(ErrorCode::archive_corrupt,
                "verified video audio PCM16 state is invalid");
  }
  if (record.end_ns <= record.start_ns) {
    return fail(ErrorCode::archive_corrupt,
                "verified video audio interval is invalid");
  }

  const auto frames = state.frames();
  if (frames == 0U ||
      frames > std::numeric_limits<std::uint64_t>::max() /
                   kNanosecondsPerSecond) {
    return fail(ErrorCode::archive_corrupt,
                "verified video audio duration is outside H.1 bounds");
  }
  const auto numerator =
      static_cast<std::uint64_t>(frames) * kNanosecondsPerSecond;
  const auto expected_floor = numerator / state.sample_rate;
  const auto duration = static_cast<std::uint64_t>(record.end_ns - record.start_ns);
  const auto difference = duration > expected_floor ? duration - expected_floor
                                                    : expected_floor - duration;
  if (difference > 1U) {
    return fail(ErrorCode::archive_corrupt,
                "verified video audio interval does not match PCM duration");
  }
  return {};
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
                "verified video audio stream descriptor is invalid");
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
                "verified HLS video audio child descriptor is invalid");
  }
  return {};
}

struct Candidate {
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

}  // namespace

Result<std::vector<VerifiedVideoPcm16Audio>> query_verified_video_pcm16_audio(
    const CodaArchive& archive, const VideoAudioQuery& query) {
  if (query.maximum_results == 0U || query.maximum_encoded_bytes == 0U) {
    return fail<std::vector<VerifiedVideoPcm16Audio>>(
        ErrorCode::invalid_argument,
        "video audio query limits must be non-zero");
  }

  const auto verification = archive.verify();
  if (!verification.ok) {
    return fail<std::vector<VerifiedVideoPcm16Audio>>(
        verification.error_code, verification.message);
  }
  if (!verification.finalized) {
    return fail<std::vector<VerifiedVideoPcm16Audio>>(
        ErrorCode::archive_corrupt,
        "verified video audio query requires a finalized archive");
  }

  const RecordQuery scoped_subject{
      .stream = query.stream,
      .type = video_pcm16_audio_state_record_type,
      .sequence = std::nullopt,
      .time = std::nullopt,
  };
  auto scoped_states = archive.query_records(scoped_subject);
  if (!scoped_states) return scoped_states.error();
  std::vector<StreamId> scoped_streams;
  scoped_streams.reserve(scoped_states->size());
  for (const auto& state : *scoped_states) {
    if (std::find(scoped_streams.begin(), scoped_streams.end(), state.stream) !=
        scoped_streams.end()) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "H.1 v1 permits at most one PCM16 audio state per stream");
    }
    scoped_streams.push_back(state.stream);
  }

  const RecordQuery subject{
      .stream = query.stream,
      .type = video_pcm16_audio_state_record_type,
      .sequence = std::nullopt,
      .time = query.time,
  };
  auto requested_states = archive.query_records(subject);
  if (!requested_states) return requested_states.error();
  if (requested_states->size() > query.maximum_results) {
    return fail<std::vector<VerifiedVideoPcm16Audio>>(
        ErrorCode::resource_exhausted,
        "verified video audio state count exceeds the configured limit");
  }

  const ProvenanceQuery provenance_query{
      .subject_truth = std::nullopt,
      .subject = subject,
      .direct_input = std::nullopt,
  };
  auto available_provenance = archive.query_provenance(provenance_query);
  if (!available_provenance) return available_provenance.error();

  std::vector<StreamProvenance> selected;
  selected.reserve(requested_states->size());
  for (const auto& state : *requested_states) {
    const StreamProvenance* match = nullptr;
    std::size_t match_count = 0U;
    for (const auto& provenance : *available_provenance) {
      if (!matches_link(state, provenance.subject)) continue;
      ++match_count;
      match = &provenance;
    }
    if (match_count != 1U || match == nullptr) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "purported H.1 video audio state lacks unique provenance");
    }
    if (match->subject_truth != TruthClass::state_exact) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "purported H.1 video audio state is not verified S1");
    }
    selected.push_back(*match);
  }

  auto records = archive.records();
  if (!records) return records.error();
  auto descriptors = archive.streams();
  if (!descriptors) return descriptors.error();

  std::vector<Candidate> candidates;
  candidates.reserve(selected.size());
  std::uint64_t encoded_bytes = 0U;

  for (auto provenance : selected) {
    auto contract = classify_process(provenance.process);
    if (!contract) return contract.error();
    auto state_record = resolve_record(*records, provenance.subject, "subject");
    if (!state_record) return state_record.error();
    if (state_record->type_code() != video_pcm16_audio_state_record_type) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "verified video audio subject has the wrong profile type");
    }
    auto video_descriptor =
        require_unique_video_descriptor(*descriptors, state_record->stream);
    if (!video_descriptor) return video_descriptor.error();

    if (*contract == VideoAudioProvenanceContract::direct &&
        provenance.inputs.size() != 1U) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "verified direct video audio requires exactly one primary S0 input");
    }
    if (*contract == VideoAudioProvenanceContract::hls &&
        provenance.inputs.size() < 2U) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          "verified HLS video audio requires primary and child S0 inputs");
    }

    std::vector<RecordInfo> source_records;
    source_records.reserve(provenance.inputs.size());
    std::vector<StreamId> hls_child_streams;
    for (std::size_t input_index = 0; input_index < provenance.inputs.size();
         ++input_index) {
      const auto& input_link = provenance.inputs[input_index];
      if (input_link.stream == provenance.subject.stream &&
          input_link.type == provenance.subject.type &&
          input_link.sequence == provenance.subject.sequence &&
          input_link.hash == provenance.subject.hash) {
        return fail<std::vector<VerifiedVideoPcm16Audio>>(
            ErrorCode::archive_corrupt,
            "verified video audio provenance is self-referential");
      }
      auto source_record = resolve_record(*records, input_link, "source");
      if (!source_record) return source_record.error();
      if (source_record->type != RecordType::source_bytes ||
          !intervals_overlap(*source_record, *state_record)) {
        return fail<std::vector<VerifiedVideoPcm16Audio>>(
            ErrorCode::archive_corrupt,
            "verified video audio source lineage is invalid");
      }

      if (input_index == 0U) {
        if (source_record->stream != state_record->stream) {
          return fail<std::vector<VerifiedVideoPcm16Audio>>(
              ErrorCode::archive_corrupt,
              "verified video audio primary S0 lineage is invalid");
        }
      } else {
        if (*contract != VideoAudioProvenanceContract::hls ||
            source_record->stream == state_record->stream) {
          return fail<std::vector<VerifiedVideoPcm16Audio>>(
              ErrorCode::archive_corrupt,
              "verified HLS video audio child lineage is invalid");
        }
        if (std::find(hls_child_streams.begin(), hls_child_streams.end(),
                      source_record->stream) != hls_child_streams.end()) {
          return fail<std::vector<VerifiedVideoPcm16Audio>>(
              ErrorCode::archive_corrupt,
              "verified HLS video audio repeats a child stream");
        }
        hls_child_streams.push_back(source_record->stream);
        auto child_descriptor =
            require_hls_child_descriptor(*descriptors, source_record->stream);
        if (!child_descriptor) return child_descriptor.error();
      }
      source_records.push_back(*source_record);
    }

    if (encoded_bytes > query.maximum_encoded_bytes ||
        state_record->payload_size >
            query.maximum_encoded_bytes - encoded_bytes) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::resource_exhausted,
          "verified video audio payloads exceed the configured limit");
    }
    encoded_bytes += state_record->payload_size;
    candidates.push_back(Candidate{
        .state_record = *state_record,
        .source_records = std::move(source_records),
        .provenance = std::move(provenance),
    });
  }

  std::vector<VerifiedVideoPcm16Audio> output;
  output.reserve(candidates.size());
  for (auto& candidate : candidates) {
    auto payload = archive.read_payload(candidate.state_record);
    if (!payload) return payload.error();
    auto state = decode_pcm16_state(*payload);
    if (!state) {
      return fail<std::vector<VerifiedVideoPcm16Audio>>(
          ErrorCode::archive_corrupt,
          std::string{"verified video audio PCM16 payload is invalid: "} +
              state.error().message);
    }
    auto valid_interval = validate_pcm_interval(*state, candidate.state_record);
    if (!valid_interval) return valid_interval.error();
    output.push_back(VerifiedVideoPcm16Audio{
        .state = std::move(*state),
        .state_record = candidate.state_record,
        .source_records = std::move(candidate.source_records),
        .provenance = std::move(candidate.provenance),
    });
  }
  return output;
}

}  // namespace codec::profiles::video
