#pragma once

#include <codec/archive.hpp>
#include <codec/audio.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace codec::profiles::video {

inline constexpr RecordTypeCode video_profile_descriptor_record_type = 0x0100;
inline constexpr RecordTypeCode raw_video_frame_state_record_type = 0x0101;
inline constexpr RecordTypeCode video_pcm16_audio_state_record_type = 0x0102;
inline constexpr RecordTypeCode video_encoded_audio_state_record_type = 0x0103;
inline constexpr RecordTypeCode video_encoded_video_state_record_type = 0x0104;

enum class PixelLayout : std::uint8_t {
  gray8 = 1,
  rgb24 = 2,
  rgba32 = 3,
  yuv420p8 = 4,
};

enum class ColorRange : std::uint8_t {
  unspecified = 0,
  limited = 1,
  full = 2,
};

enum class ColorPrimaries : std::uint8_t {
  unspecified = 0,
  bt709 = 1,
  bt2020 = 2,
};

enum class TransferCharacteristics : std::uint8_t {
  unspecified = 0,
  linear = 1,
  srgb = 2,
  bt709 = 3,
  pq = 4,
  hlg = 5,
};

enum class MatrixCoefficients : std::uint8_t {
  unspecified = 0,
  identity = 1,
  bt709 = 2,
  bt2020_ncl = 3,
};

struct VideoDecodeLimits {
  std::uint32_t maximum_width{16384};
  std::uint32_t maximum_height{16384};
  std::uint64_t maximum_pixels{268435456};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VideoProfileDescriptor {
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  PixelLayout pixel_layout{PixelLayout::gray8};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint32_t nominal_frame_rate_numerator{};
  std::uint32_t nominal_frame_rate_denominator{1};
  ColorRange color_range{ColorRange::unspecified};
  ColorPrimaries color_primaries{ColorPrimaries::unspecified};
  TransferCharacteristics transfer{TransferCharacteristics::unspecified};
  MatrixCoefficients matrix{MatrixCoefficients::unspecified};
  auto operator<=>(const VideoProfileDescriptor&) const = default;
};

struct RawVideoFrameState {
  VideoProfileDescriptor descriptor;
  std::vector<std::byte> pixels;
  bool operator==(const RawVideoFrameState&) const = default;
};

enum class EncodedAudioCodec : std::uint16_t {
  aac = 1,
};

struct EncodedAudioPacket {
  std::int64_t pts_offset_ns{};
  std::int64_t dts_offset_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
  bool operator==(const EncodedAudioPacket&) const = default;
};

struct EncodedAudioState {
  EncodedAudioCodec codec{EncodedAudioCodec::aac};
  std::int32_t codec_profile{};
  std::uint32_t sample_rate{};
  std::uint16_t channels{};
  std::uint64_t decoded_frames{};
  std::uint64_t trim_start_frames{};
  std::uint64_t presentation_frames{};
  std::vector<std::byte> decoder_config;
  std::vector<EncodedAudioPacket> packets;
  bool operator==(const EncodedAudioState&) const = default;
};

struct EncodedAudioDecodeLimits {
  std::size_t maximum_packets{1'000'000};
  std::uint64_t maximum_decoder_config_bytes{1024ULL * 1024ULL};
  std::uint64_t maximum_packet_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};

enum class EncodedVideoCodec : std::uint16_t {
  h264 = 1,
};

enum class EncodedVideoPacketFraming : std::uint8_t {
  unknown = 0,
  length_prefixed = 1,
  annex_b = 2,
};

struct EncodedVideoPacket {
  std::int64_t pts_offset_ns{};
  std::int64_t dts_offset_ns{};
  std::uint64_t duration_ns{};
  std::uint32_t flags{};
  std::vector<std::byte> payload;
  bool operator==(const EncodedVideoPacket&) const = default;
};

struct EncodedVideoState {
  EncodedVideoCodec codec{EncodedVideoCodec::h264};
  EncodedVideoPacketFraming framing{EncodedVideoPacketFraming::unknown};
  std::int32_t codec_profile{};
  std::int32_t codec_level{};
  std::uint32_t coded_width{};
  std::uint32_t coded_height{};
  std::uint32_t sample_aspect_ratio_numerator{1};
  std::uint32_t sample_aspect_ratio_denominator{1};
  std::uint64_t validated_frames{};
  std::uint64_t presentation_lead_ns{};
  std::vector<std::byte> decoder_config;
  std::vector<EncodedVideoPacket> packets;
  bool operator==(const EncodedVideoState&) const = default;
};

struct EncodedVideoDecodeLimits {
  std::size_t maximum_packets{1'000'000};
  std::uint64_t maximum_decoder_config_bytes{4ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_packet_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_payload_bytes{1024ULL * 1024ULL * 1024ULL};
};

Result<std::vector<std::byte>> encode_video_profile_descriptor(
    const VideoProfileDescriptor& descriptor);
Result<VideoProfileDescriptor> decode_video_profile_descriptor(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_raw_video_frame_state(
    const RawVideoFrameState& frame);
Result<RawVideoFrameState> decode_raw_video_frame_state(
    std::span<const std::byte> payload, VideoDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_encoded_audio_state(
    const EncodedAudioState& state);
Result<EncodedAudioState> decode_encoded_audio_state(
    std::span<const std::byte> payload,
    EncodedAudioDecodeLimits limits = {});
Result<std::vector<std::byte>> encode_encoded_video_state(
    const EncodedVideoState& state);
Result<EncodedVideoState> decode_encoded_video_state(
    std::span<const std::byte> payload,
    EncodedVideoDecodeLimits limits = {});

struct VideoFrameQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
  VideoDecodeLimits decode_limits{};
};

struct VerifiedRawVideoFrame {
  RawVideoFrameState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedRawVideoFrame>> query_verified_raw_video_frames(
    const CodaArchive& archive, const VideoFrameQuery& query = {});

struct VideoAudioQuery {
  std::optional<StreamId> stream;
  std::optional<RecordTimeRange> time;
  std::size_t maximum_results{1024};
  std::uint64_t maximum_encoded_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct VerifiedVideoPcm16Audio {
  Pcm16State state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

struct VerifiedVideoEncodedAudio {
  EncodedAudioState state;
  RecordInfo state_record;
  std::vector<RecordInfo> source_records;
  StreamProvenance provenance;
};

Result<std::vector<VerifiedVideoEncodedAudio>>
query_verified_video_encoded_audio(
    const CodaArchive& archive, const VideoAudioQuery& query = {});

Result<std::vector<VerifiedVideoPcm16Audio>> query_verified_video_pcm16_audio(
    const CodaArchive& archive, const VideoAudioQuery& query = {});

struct FfmpegVideoIngestRequest {
  std::string source_uri;
  std::filesystem::path archive_path;
  StreamDescriptor descriptor;
  std::int64_t start_ns{};
  std::int64_t end_ns{};
  PixelLayout output_layout{PixelLayout::yuv420p8};
  std::size_t capture_chunk_bytes{256U * 1024U};
  std::uint64_t maximum_source_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_decoded_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_decoded_audio_bytes{1024ULL * 1024ULL * 1024ULL};
  std::size_t maximum_frames{4096};
  std::uint32_t maximum_redirects{5};
  bool deny_private_network{true};
  std::size_t maximum_hls_resources{256};
  std::uint64_t maximum_hls_resource_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint64_t maximum_hls_total_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct FfmpegVideoIngestReport {
  std::filesystem::path archive_path;
  RecordInfo descriptor;
  RecordInfo source;
  std::vector<RecordInfo> states;
  std::vector<RecordInfo> provenance;
  std::optional<Error> profile_error;
  std::vector<RecordInfo> secondary_descriptors{};
  std::vector<RecordInfo> secondary_sources{};
  bool audio_present{};
  std::optional<RecordInfo> audio_state{};
  std::optional<RecordInfo> audio_provenance{};

  bool audio_state_exact() const noexcept {
    return !audio_present ||
           (audio_state.has_value() && audio_provenance.has_value());
  }

  bool state_exact() const noexcept {
    return !states.empty() && states.size() == provenance.size() &&
           audio_state_exact() && !profile_error.has_value();
  }
};

bool ffmpeg_video_ingest_available() noexcept;
Result<FfmpegVideoIngestReport> ingest_video_ffmpeg(
    const FfmpegVideoIngestRequest& request);

}  // namespace codec::profiles::video
