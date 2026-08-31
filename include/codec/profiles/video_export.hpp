#pragma once

#include <codec/processing.hpp>
#include <codec/profiles/video.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace codec::profiles::video {

struct VideoMp4ExportLimits {
  std::uint64_t maximum_output_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VerifiedVideoMp4Export {
  ExportResult output;
  std::vector<RecordInfo> state_records;
  std::vector<StreamProvenance> provenance;
  std::optional<RecordInfo> audio_state_record{};
  std::optional<StreamProvenance> audio_provenance{};
};

bool ffmpeg_video_export_available() noexcept;

Result<VerifiedVideoMp4Export> export_verified_video_mp4(
    const CodaArchive& archive, const VideoFrameQuery& query = {},
    VideoMp4ExportLimits limits = {});

}  // namespace codec::profiles::video
