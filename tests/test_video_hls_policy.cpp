#include "test.hpp"

#include <codec/profiles/video.hpp>

#include "../src/video/hls_policy.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace video = codec::profiles::video;
namespace hls = codec::profiles::video::detail;

namespace {

std::vector<std::byte> bytes(std::string_view text) {
  const auto view = std::as_bytes(std::span{text.data(), text.size()});
  return {view.begin(), view.end()};
}

video::FfmpegVideoIngestRequest valid_request(
    const std::filesystem::path& archive) {
  return video::FfmpegVideoIngestRequest{
      .source_uri = "fixture.mp4",
      .archive_path = archive,
      .descriptor = codec::StreamDescriptor{
          .id = codec::derive_stream_id("video-hls-policy-request"),
          .type = codec::StreamType::video,
          .label = "video",
          .source_id = "fixture",
          .payload_type = "video/mp4",
      },
      .start_ns = 0,
      .end_ns = 1'000'000'000,
      .maximum_frames = 4,
  };
}

}  // namespace

TEST(video_hls_policy_detects_manifest_after_bom_and_ascii_whitespace) {
  auto manifest = bytes("\xEF\xBB\xBF \t\r\n#EXTM3U\n#EXT-X-VERSION:3\n#EXTINF:1,\nseg0.ts\n");
  EXPECT_TRUE(hls::looks_like_hls_manifest(manifest));
}

TEST(video_hls_policy_does_not_treat_extension_only_as_hls) {
  const auto not_hls = bytes("plain bytes with no HLS tags\n");
  EXPECT_FALSE(hls::looks_like_hls_manifest(not_hls));
}

TEST(video_hls_policy_rejects_encrypted_key_methods_before_uri_use) {
  const auto encrypted = bytes(
      "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"secret.key\"\n"
      "#EXTINF:1,\nseg0.ts\n");
  auto result = hls::validate_hls_manifest_security(encrypted);
  EXPECT_FALSE(result);
  if (!result) EXPECT_EQ(result.error().code, codec::ErrorCode::model_incompatible);
}

TEST(video_hls_policy_allows_method_none) {
  const auto manifest = bytes(
      "#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:1,\nseg0.ts\n");
  EXPECT_TRUE(hls::validate_hls_manifest_security(manifest));
}

TEST(video_hls_policy_normalizes_default_ports_and_host_case) {
  auto origin = hls::parse_hls_http_origin("HTTPS://Example.COM/path.m3u8");
  EXPECT_TRUE(origin);
  if (!origin) return;
  EXPECT_EQ(origin->scheme, std::string{"https"});
  EXPECT_EQ(origin->host, std::string{"example.com"});
  EXPECT_EQ(origin->port, std::uint16_t{443});
  EXPECT_TRUE(hls::require_same_hls_origin(
      *origin, "https://example.com:443/seg.ts"));
}

TEST(video_hls_policy_rejects_cross_origin_and_non_http_children) {
  auto origin = hls::parse_hls_http_origin("https://example.com/live/a.m3u8");
  EXPECT_TRUE(origin);
  if (!origin) return;

  for (const std::string_view child : {
           "https://example.com:444/a.ts",
           "http://example.com/a.ts",
           "https://other.example/a.ts",
           "file:///tmp/a.ts",
           "crypto:https://example.com/a.ts",
       }) {
    EXPECT_FALSE(hls::require_same_hls_origin(*origin, child));
  }
}

TEST(video_hls_policy_child_identity_is_deterministic_without_url_material) {
  const auto parent = codec::derive_stream_id("parent-video");
  const auto first = hls::derive_hls_child_stream_id(parent, 7);
  const auto second = hls::derive_hls_child_stream_id(parent, 7);
  const auto next = hls::derive_hls_child_stream_id(parent, 8);
  EXPECT_EQ(first, second);
  EXPECT_FALSE(first == next);
  EXPECT_EQ(hls::hls_child_label("NS", 7),
            std::string{"NS:hls-resource-0007"});
}

TEST(video_hls_request_rejects_zero_hls_limits_before_archive_mutation) {
  const auto root = std::filesystem::temp_directory_path();
  for (int field = 0; field < 3; ++field) {
    const auto archive = root / ("codec-video-hls-zero-" +
                                 std::to_string(field) + ".coda");
    std::filesystem::remove(archive);
    auto request = valid_request(archive);
    if (field == 0) request.maximum_hls_resources = 0;
    if (field == 1) request.maximum_hls_resource_bytes = 0;
    if (field == 2) request.maximum_hls_total_bytes = 0;

    auto result = video::ingest_video_ffmpeg(request);
    EXPECT_FALSE(result);
    if (!result) {
      EXPECT_EQ(result.error().code, codec::ErrorCode::invalid_argument);
    }
    EXPECT_FALSE(std::filesystem::exists(archive));
  }
}

TEST(video_hls_request_rejects_unrepresentable_hls_limits_before_archive_mutation) {
  const auto root = std::filesystem::temp_directory_path();
  const auto ordinal_archive = root / "codec-video-hls-ordinal-overflow.coda";
  std::filesystem::remove(ordinal_archive);
  auto ordinal_request = valid_request(ordinal_archive);
  ordinal_request.maximum_hls_resources = 1'000'000U;
  auto ordinal_result = video::ingest_video_ffmpeg(ordinal_request);
  EXPECT_FALSE(ordinal_result);
  if (!ordinal_result) {
    EXPECT_EQ(ordinal_result.error().code, codec::ErrorCode::invalid_argument);
  }
  EXPECT_FALSE(std::filesystem::exists(ordinal_archive));

  if constexpr (std::numeric_limits<std::uint64_t>::max() >
                std::numeric_limits<std::size_t>::max()) {
    const auto per_resource_archive =
        root / "codec-video-hls-resource-byte-overflow.coda";
    std::filesystem::remove(per_resource_archive);
    auto per_resource_request = valid_request(per_resource_archive);
    per_resource_request.maximum_hls_resource_bytes =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) + 1U;
    auto per_resource_result = video::ingest_video_ffmpeg(per_resource_request);
    EXPECT_FALSE(per_resource_result);
    if (!per_resource_result) {
      EXPECT_EQ(per_resource_result.error().code,
                codec::ErrorCode::invalid_argument);
    }
    EXPECT_FALSE(std::filesystem::exists(per_resource_archive));

    const auto total_archive = root / "codec-video-hls-total-byte-overflow.coda";
    std::filesystem::remove(total_archive);
    auto total_request = valid_request(total_archive);
    total_request.maximum_hls_total_bytes =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) + 1U;
    auto total_result = video::ingest_video_ffmpeg(total_request);
    EXPECT_FALSE(total_result);
    if (!total_result) {
      EXPECT_EQ(total_result.error().code, codec::ErrorCode::invalid_argument);
    }
    EXPECT_FALSE(std::filesystem::exists(total_archive));
  }
}
