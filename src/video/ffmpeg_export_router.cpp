#include "ffmpeg_aac_trim_mux.hpp"
#include "ffmpeg_packet_mux.hpp"

#include "../archive/verified_snapshot_scope.hpp"

#define export_verified_video_mp4 export_verified_video_mp4_video_only
#include "ffmpeg_export.cpp"
#undef export_verified_video_mp4

namespace codec::profiles::video {
namespace {

bool overlaps_time_filter(const RecordInfo& record,
                          const std::optional<RecordTimeRange>& time) {
  return !time.has_value() ||
         (record.start_ns < time->end_ns && time->begin_ns < record.end_ns);
}

bool encoded_audio_trim_has_preroll(const EncodedAudioState& state) {
  if (state.trim_start_frames == 0U) return true;
  for (const auto& packet : state.packets) {
    if (packet.pts_offset_ns < 0 || packet.dts_offset_ns < 0) return true;
  }
  return false;
}

ProvenanceRecordLink record_link(const RecordInfo& record) {
  return ProvenanceRecordLink{
      .stream = record.stream,
      .type = record.type_code(),
      .sequence = record.sequence,
      .hash = record.hash,
  };
}

Result<VerifiedVideoMp4Export> export_video_only(
    const CodaArchive& archive, const VideoFrameQuery& query,
    VideoMp4ExportLimits limits) {
  auto encoded = query_verified_video_encoded_video(archive, query);
  if (!encoded) return encoded.error();
  auto raw = query_verified_raw_video_frames(archive, query);
  if (!raw) return raw.error();

  if (!encoded->empty() && !raw->empty()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::archive_corrupt,
        "H.1 MP4 export found contradictory encoded and raw video states");
  }
  if (encoded->empty()) {
    return export_verified_video_mp4_video_only(archive, query, limits);
  }
  if (encoded->size() != 1U) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::archive_corrupt,
        "H.1 v1 MP4 export requires exactly one verified encoded video state");
  }

  auto payload =
      mux_verified_encoded_video_packets(encoded->front(),
                                         limits.maximum_output_bytes);
  if (!payload) return payload.error();
  const auto& state_record = encoded->front().state_record;
  return VerifiedVideoMp4Export{
      .output = ExportResult{
          .payload_type = "video/mp4",
          .payload = std::move(*payload),
          .supporting_records = {record_link(state_record)},
      },
      .state_records = {state_record},
      .provenance = {encoded->front().provenance},
      .audio_state_record = std::nullopt,
      .audio_provenance = std::nullopt,
      .video_packet_passthrough = true,
      .audio_packet_passthrough = false,
  };
}

}  // namespace

Result<VerifiedVideoMp4Export> export_verified_video_mp4(
    const CodaArchive& archive, const VideoFrameQuery& query,
    VideoMp4ExportLimits limits) {
  if (limits.maximum_output_bytes == 0U) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::invalid_argument,
        "video MP4 export output limit must be non-zero");
  }

  auto video_only = export_video_only(archive, query, limits);
  if (!video_only) return video_only.error();
  if (video_only->state_records.empty()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::internal,
        "verified MP4 export returned no video state evidence");
  }

  const auto stream = video_only->state_records.front().stream;
  auto encoded_audio = query_verified_video_encoded_audio(
      archive,
      VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1,
          .maximum_encoded_bytes = query.maximum_encoded_bytes,
      });
  if (!encoded_audio) return encoded_audio.error();
  auto pcm16_audio = query_verified_video_pcm16_audio(
      archive,
      VideoAudioQuery{
          .stream = stream,
          .time = std::nullopt,
          .maximum_results = 1,
          .maximum_encoded_bytes = query.maximum_encoded_bytes,
      });
  if (!pcm16_audio) return pcm16_audio.error();

  if (!encoded_audio->empty() && !pcm16_audio->empty()) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::archive_corrupt,
        "H.1 MP4 export found contradictory encoded and PCM16 audio states");
  }
  if (!encoded_audio->empty() &&
      !overlaps_time_filter(encoded_audio->front().state_record, query.time)) {
    encoded_audio->clear();
  }
  if (!pcm16_audio->empty() &&
      !overlaps_time_filter(pcm16_audio->front().state_record, query.time)) {
    pcm16_audio->clear();
  }
  if (encoded_audio->empty() && pcm16_audio->empty()) {
    return video_only;
  }
  if (encoded_audio->size() > 1U || pcm16_audio->size() > 1U) {
    return fail<VerifiedVideoMp4Export>(
        ErrorCode::archive_corrupt,
        "H.1 v1 MP4 export requires at most one verified audio state");
  }

  const bool packet_passthrough = !encoded_audio->empty();
  const auto muxed = [&]() -> Result<std::vector<std::byte>> {
    if (packet_passthrough) {
      if (!encoded_audio_trim_has_preroll(encoded_audio->front().state)) {
        return fail<std::vector<std::byte>>(
            ErrorCode::model_incompatible,
            "encoded AAC leading trim has no compressed preroll timestamps");
      }
      return mux_verified_encoded_audio_trim_aware(
          video_only->output.payload, encoded_audio->front(),
          video_only->state_records.front().start_ns,
          limits.maximum_output_bytes);
    }
    return mux_verified_pcm16_audio(
        video_only->output.payload, pcm16_audio->front(),
        video_only->state_records.front().start_ns,
        limits.maximum_output_bytes);
  }();
  if (!muxed) return muxed.error();

  video_only->output.payload = std::move(*muxed);
  const auto& state_record = packet_passthrough
                                 ? encoded_audio->front().state_record
                                 : pcm16_audio->front().state_record;
  video_only->output.supporting_records.push_back(record_link(state_record));
  video_only->audio_state_record = state_record;
  video_only->audio_provenance = packet_passthrough
                                     ? encoded_audio->front().provenance
                                     : pcm16_audio->front().provenance;
  video_only->audio_packet_passthrough = packet_passthrough;
  return video_only;
}

Result<VerifiedVideoMp4Export> export_verified_video_mp4(
    const CodaArchive& archive, const VerifiedArchiveSnapshot& snapshot,
    const VideoFrameQuery& query, VideoMp4ExportLimits limits) {
  auto scope = codec::detail::activate_verified_archive_snapshot(archive,
                                                                 snapshot);
  if (!scope) return scope.error();
  return export_verified_video_mp4(archive, query, limits);
}

}  // namespace codec::profiles::video
