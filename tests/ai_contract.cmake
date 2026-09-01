if(NOT DEFINED CODEC_SOURCE_DIR)
  message(FATAL_ERROR "CODEC_SOURCE_DIR is required")
endif()

function(read_required relative_path output_variable)
  set(absolute_path "${CODEC_SOURCE_DIR}/${relative_path}")
  if(NOT EXISTS "${absolute_path}")
    message(FATAL_ERROR "Required AI contract file is missing: ${relative_path}")
  endif()
  file(READ "${absolute_path}" contents)
  set(${output_variable} "${contents}" PARENT_SCOPE)
endfunction()

function(require_reference source_name source_contents target)
  string(FIND "${source_contents}" "(${target})" reference_offset)
  if(reference_offset EQUAL -1)
    message(FATAL_ERROR "${source_name} must link to ${target}")
  endif()
  if(NOT EXISTS "${CODEC_SOURCE_DIR}/${target}")
    message(FATAL_ERROR "${source_name} links to missing path: ${target}")
  endif()
endfunction()

function(reject_runtime_symbol source_name source_contents symbol)
  string(FIND "${source_contents}" "${symbol}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "${source_name} still contains retired runtime symbol: ${symbol}")
  endif()
endfunction()

read_required("CMakeLists.txt" cmake_contents)
read_required("AGENTS.md" agents_contents)
read_required("README.md" readme_contents)
read_required("AI_WORKSHEET.md" worksheet_contents)
read_required("CONTRIBUTING.md" contributing_contents)
read_required("CHANGELOG.md" changelog_contents)
read_required("include/codec/archive.hpp" archive_header_contents)
read_required("include/codec/result.hpp" result_header_contents)
read_required("include/codec/codec_c.h" c_header_contents)
read_required("include/codec/profiles/video.hpp" video_header_contents)
read_required("include/codec/profiles/video_export.hpp" video_export_header_contents)
read_required("src/archive/archive.cpp" archive_source_contents)
read_required("src/core/sha256.cpp" core_source_contents)
read_required("src/capi/codec_c.cpp" capi_source_contents)
read_required("src/distributed/wire.cpp" wire_source_contents)
read_required("src/video/frame_state.cpp" video_frame_state_contents)
read_required("src/video/frame_state_reader.cpp" video_frame_reader_contents)
read_required("src/video/encoded_video_state_reader.cpp" encoded_video_reader_contents)
read_required("src/video/ffmpeg_audio_mux.cpp" video_audio_mux_contents)
read_required("src/video/ffmpeg_export_router.cpp" video_export_router_contents)
read_required("src/video/ffmpeg_packet_mux.cpp" video_packet_mux_contents)

foreach(retired_symbol IN ITEMS
    "watermark_statement"
    "watermark_observation"
    "watermark_model_missing"
    "watermark_code_ambiguous"
    "watermark_signature_invalid"
    "watermark_replay_suspected"
    "watermark_path_unqualified"
    "CODEC_STATUS_WATERMARK")
  reject_runtime_symbol("include/codec/archive.hpp"
    "${archive_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("include/codec/result.hpp"
    "${result_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("include/codec/codec_c.h"
    "${c_header_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/archive/archive.cpp"
    "${archive_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/core/sha256.cpp"
    "${core_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/capi/codec_c.cpp"
    "${capi_source_contents}" "${retired_symbol}")
  reject_runtime_symbol("src/distributed/wire.cpp"
    "${wire_source_contents}" "${retired_symbol}")
endforeach()

foreach(retired_file IN ITEMS
    "include/codec/watermark.hpp"
    "include/codec/statement.hpp"
    "src/watermark/carrier.cpp"
    "src/watermark/statement.cpp"
    "tests/test_watermark.cpp"
    "tests/test_statement.cpp"
    "src/video/ffmpeg_export_dispatch.cpp")
  if(EXISTS "${CODEC_SOURCE_DIR}/${retired_file}")
    message(FATAL_ERROR "Retired file remains: ${retired_file}")
  endif()
endforeach()

foreach(retired_build_text IN ITEMS
    "src/watermark/carrier.cpp"
    "src/watermark/statement.cpp"
    "tests/test_watermark.cpp"
    "tests/test_statement.cpp"
    "src/video/ffmpeg_export_dispatch.cpp")
  string(FIND "${cmake_contents}" "${retired_build_text}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "CMakeLists.txt still references ${retired_build_text}")
  endif()
endforeach()

string(REGEX MATCH
  "project[ \t\r\n]*\\([ \t\r\n]*CODEC[ \t\r\n]+VERSION[ \t\r\n]+([0-9]+\\.[0-9]+\\.[0-9]+)"
  project_declaration "${cmake_contents}")
if(NOT project_declaration)
  message(FATAL_ERROR "Cannot determine CODEC version from CMakeLists.txt")
endif()
set(project_version "${CMAKE_MATCH_1}")

string(REGEX MATCH
  "(^|\n)[ \t]+version:[ \t]*([^ \t\r\n]+)[ \t]*(\r?\n|$)"
  manifest_version_line "${readme_contents}")
if(NOT manifest_version_line)
  message(FATAL_ERROR "Cannot determine version from the README AI manifest")
endif()
set(manifest_version "${CMAKE_MATCH_2}")
if(NOT manifest_version STREQUAL project_version)
  message(FATAL_ERROR
    "README AI manifest version ${manifest_version} does not match CMake project version ${project_version}")
endif()

require_reference("AGENTS.md" "${agents_contents}" "README.md")
require_reference("AGENTS.md" "${agents_contents}" "AI_WORKSHEET.md")
require_reference("README.md" "${readme_contents}" "AGENTS.md")
require_reference("README.md" "${readme_contents}" "AI_WORKSHEET.md")
require_reference("README.md" "${readme_contents}"
  "docs/superpowers/specs/2026-08-18-generalized-coda-direction-design.md")
require_reference("README.md" "${readme_contents}"
  "docs/superpowers/specs/2026-08-30-stage-h1-video-profile-design.md")
require_reference("README.md" "${readme_contents}"
  "docs/superpowers/plans/2026-08-30-stage-h1-video-profile.md")
require_reference("README.md" "${readme_contents}"
  "docs/superpowers/specs/2026-08-30-video-hls-ingest-design.md")
require_reference("README.md" "${readme_contents}"
  "docs/superpowers/plans/2026-08-30-video-hls-ingest.md")
require_reference("CONTRIBUTING.md" "${contributing_contents}" "README.md")
require_reference("CONTRIBUTING.md" "${contributing_contents}" "AI_WORKSHEET.md")
require_reference("CONTRIBUTING.md" "${contributing_contents}" "AGENTS.md")

foreach(required_contract IN ITEMS
    "contract_version: 1"
    "truth_classes: [S0, S1, D]"
    "core_direction: stream-first"
    "profile_boundary: profile-specific semantics stay outside generic core"
    "continuity_evidence: [git_head, open_prs, exact_head_ci, roadmap_issue]"
    "roadmap_issue_title: CODEC v1.0 roadmap execution log")
  string(FIND "${agents_contents}" "${required_contract}" contract_offset)
  if(contract_offset EQUAL -1)
    message(FATAL_ERROR
      "AGENTS.md is missing required machine-readable contract: ${required_contract}")
  endif()
endforeach()

foreach(required_contribution_term IN ITEMS
    "stream-first" "S0" "S1" "D" "Audio Stream Profile")
  string(FIND "${contributing_contents}"
    "${required_contribution_term}" contribution_offset)
  if(contribution_offset EQUAL -1)
    message(FATAL_ERROR
      "CONTRIBUTING.md is missing agent-contract term: ${required_contribution_term}")
  endif()
endforeach()

foreach(retired_cli_text IN ITEMS
    "codec watermark"
    "codec list streams"
    "--stream STREAM_ID"
    "w0_ed25519"
    "w1_reference"
    "w2_reference"
    "w2_policy")
  string(FIND "${readme_contents}" "${retired_cli_text}" retired_offset)
  if(NOT retired_offset EQUAL -1)
    message(FATAL_ERROR
      "README.md still advertises retired surface: ${retired_cli_text}")
  endif()
endforeach()

foreach(required_video_contract IN ITEMS
    "video_profile_descriptor_record_type = 0x0100"
    "raw_video_frame_state_record_type = 0x0101"
    "video_encoded_video_state_record_type = 0x0104"
    "query_verified_raw_video_frames"
    "query_verified_video_encoded_video"
    "ffmpeg_video_ingest_available"
    "codec.video.raw-frame.canonicalize.hls"
    "codec.video.encoded-video.preserve"
    "same-origin unencrypted HTTP/HTTPS HLS"
    "Video Stream Profile — Stage H.1"
    "Stage G trust/selective-disclosure work is explicitly deferred")
  string(FIND
    "${video_header_contents}\n${video_frame_reader_contents}\n${encoded_video_reader_contents}\n${readme_contents}"
    "${required_video_contract}" video_contract_offset)
  if(video_contract_offset EQUAL -1)
    message(FATAL_ERROR
      "Stage H.1/video integration contract is missing: ${required_video_contract}")
  endif()
endforeach()

set(video_foundation_contents
  "${video_header_contents}\n${video_frame_state_contents}\n${video_frame_reader_contents}\n${encoded_video_reader_contents}")
foreach(forbidden_video_foundation_dependency IN ITEMS
    "#include <libav"
    "AVCodecContext"
    "AVFormatContext"
    "#include <gst/")
  string(FIND "${video_foundation_contents}"
    "${forbidden_video_foundation_dependency}" dependency_offset)
  if(NOT dependency_offset EQUAL -1)
    message(FATAL_ERROR
      "Stage H.1 foundation must remain media-library independent: ${forbidden_video_foundation_dependency}")
  endif()
endforeach()

foreach(required_optional_video_build_contract IN ITEMS
    "CODEC_ENABLE_FFMPEG_VIDEO"
    "libavformat"
    "libavcodec"
    "libavutil"
    "libswscale"
    "libswresample")
  string(FIND "${cmake_contents}" "${required_optional_video_build_contract}"
    optional_video_offset)
  if(optional_video_offset EQUAL -1)
    message(FATAL_ERROR
      "Optional FFmpeg video build contract is missing: ${required_optional_video_build_contract}")
  endif()
endforeach()

foreach(required_ffmpeg_audio_config_contract IN ITEMS
    "LIBAVCODEC_VERSION_INT"
    "AV_VERSION_INT(61, 12, 100)"
    "avcodec_get_supported_config"
    "AV_CODEC_CONFIG_SAMPLE_RATE"
    "AV_CODEC_CONFIG_SAMPLE_FORMAT")
  string(FIND "${video_audio_mux_contents}"
    "${required_ffmpeg_audio_config_contract}" ffmpeg_config_offset)
  if(ffmpeg_config_offset EQUAL -1)
    message(FATAL_ERROR
      "FFmpeg audio export compatibility contract is missing: ${required_ffmpeg_audio_config_contract}")
  endif()
endforeach()

set(video_packet_export_contents
  "${video_export_header_contents}\n${video_export_router_contents}\n${video_packet_mux_contents}\n${video_audio_mux_contents}")
foreach(required_packet_export_contract IN ITEMS
    "video_packet_passthrough"
    "mux_verified_encoded_video_packets"
    "mux_verified_encoded_audio_packets"
    "aac_adtstoasc"
    "extract_extradata"
    "H.1 MP4 export found contradictory encoded and raw video states")
  string(FIND "${video_packet_export_contents}"
    "${required_packet_export_contract}" packet_export_offset)
  if(packet_export_offset EQUAL -1)
    message(FATAL_ERROR
      "Encoded-video packet export contract is missing: ${required_packet_export_contract}")
  endif()
endforeach()

string(FIND "${agents_contents}" "watermark" agents_watermark_offset)
if(NOT agents_watermark_offset EQUAL -1)
  message(FATAL_ERROR "AGENTS.md still requires watermarking")
endif()
string(FIND "${contributing_contents}" "watermark"
  contributing_watermark_offset)
if(NOT contributing_watermark_offset EQUAL -1)
  message(FATAL_ERROR "CONTRIBUTING.md still requires watermarking")
endif()

foreach(required_field IN ITEMS base_head_sha active_roadmap_stage continuity_evidence)
  string(FIND "${worksheet_contents}" "${required_field}:" field_offset)
  if(field_offset EQUAL -1)
    message(FATAL_ERROR
      "AI_WORKSHEET.md is missing cold-start field: ${required_field}")
  endif()
endforeach()

message(STATUS "CODEC AI contract verified for version ${project_version}")
