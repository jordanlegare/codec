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
read_required("src/archive/archive.cpp" archive_source_contents)
read_required("src/core/sha256.cpp" core_source_contents)
read_required("src/capi/codec_c.cpp" capi_source_contents)
read_required("src/distributed/wire.cpp" wire_source_contents)

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
    "tests/test_statement.cpp")
  if(EXISTS "${CODEC_SOURCE_DIR}/${retired_file}")
    message(FATAL_ERROR "Retired watermark file remains: ${retired_file}")
  endif()
endforeach()

foreach(retired_build_text IN ITEMS
    "src/watermark/carrier.cpp"
    "src/watermark/statement.cpp"
    "tests/test_watermark.cpp"
    "tests/test_statement.cpp")
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
