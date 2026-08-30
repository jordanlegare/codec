#pragma once

#include <codec/archive.hpp>
#include <codec/profiles/video.hpp>

#include "../core/internal.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace codec::cli {
namespace detail {

struct GroupedVideoIngest {
  profiles::video::FfmpegVideoIngestRequest request;
  profiles::video::PixelLayout layout{profiles::video::PixelLayout::yuv420p8};
};

inline int print_grouped_error(const Error& error) {
  std::cerr << "codec: " << error_code_name(error.code) << ": "
            << error.message << '\n';
  return 1;
}

template <typename Integer>
inline bool parse_grouped_decimal(std::string_view text, Integer& value) {
  if (text.empty()) return false;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [next, error] = std::from_chars(begin, end, value, 10);
  return error == std::errc{} && next == end;
}

inline std::optional<profiles::video::PixelLayout> parse_grouped_layout(
    std::string_view text) {
  using profiles::video::PixelLayout;
  if (text == "gray8") return PixelLayout::gray8;
  if (text == "rgb24") return PixelLayout::rgb24;
  if (text == "rgba32") return PixelLayout::rgba32;
  if (text == "yuv420p8") return PixelLayout::yuv420p8;
  return std::nullopt;
}

inline std::string_view grouped_layout_name(
    profiles::video::PixelLayout layout) {
  using profiles::video::PixelLayout;
  switch (layout) {
    case PixelLayout::gray8: return "gray8";
    case PixelLayout::rgb24: return "rgb24";
    case PixelLayout::rgba32: return "rgba32";
    case PixelLayout::yuv420p8: return "yuv420p8";
  }
  return "unknown";
}

inline Result<void> require_new_archive_path(const std::filesystem::path& path) {
  if (path.empty() || path.filename().empty()) {
    return fail(ErrorCode::invalid_argument,
                "grouped video ingest requires an archive file path");
  }
  std::error_code inspection_error;
  const auto status = std::filesystem::symlink_status(path, inspection_error);
  if (inspection_error == std::errc::no_such_file_or_directory) {
    inspection_error.clear();
  } else if (inspection_error) {
    return fail(ErrorCode::archive_io,
                "cannot inspect grouped video ingest archive output: " +
                    inspection_error.message());
  }
  if (!inspection_error &&
      status.type() != std::filesystem::file_type::not_found &&
      status.type() != std::filesystem::file_type::none) {
    return fail(ErrorCode::archive_io,
                "grouped video ingest refuses to replace an existing archive path");
  }
  return {};
}

inline Result<std::vector<GroupedVideoIngest>> parse_grouped_video_ingests(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& archive_path) {
  using profiles::video::PixelLayout;

  if (arguments.empty()) {
    return fail<std::vector<GroupedVideoIngest>>(
        ErrorCode::invalid_argument,
        "grouped video ingest requires at least one --video group");
  }

  std::vector<GroupedVideoIngest> groups;
  std::set<StreamId> stream_ids;
  std::size_t cursor = 0U;
  while (cursor < arguments.size()) {
    if (arguments[cursor] != "--video") {
      return fail<std::vector<GroupedVideoIngest>>(
          ErrorCode::invalid_argument,
          "grouped video ingest options must be introduced by --video");
    }
    ++cursor;
    const auto begin = cursor;
    while (cursor < arguments.size() && arguments[cursor] != "--video") {
      ++cursor;
    }
    const auto group = arguments.subspan(begin, cursor - begin);
    if (group.empty()) {
      return fail<std::vector<GroupedVideoIngest>>(
          ErrorCode::invalid_argument,
          "grouped video ingest --video group must not be empty");
    }

    std::map<std::string_view, std::string_view> values;
    for (std::size_t index = 0; index < group.size();) {
      const auto name = group[index];
      if (name == "--archive") {
        return fail<std::vector<GroupedVideoIngest>>(
            ErrorCode::invalid_argument,
            "grouped video ingest uses one command-level --archive");
      }
      if (name.empty() || name.rfind("--", 0) != 0 || index + 1 >= group.size() ||
          group[index + 1].rfind("--", 0) == 0) {
        return fail<std::vector<GroupedVideoIngest>>(
            ErrorCode::invalid_argument,
            "grouped video ingest options require NAME VALUE pairs");
      }
      const auto supported =
          name == "--source" || name == "--label" || name == "--start-ns" ||
          name == "--end-ns" || name == "--layout" ||
          name == "--maximum-source-bytes" ||
          name == "--maximum-decoded-bytes" || name == "--maximum-frames" ||
          name == "--maximum-hls-resources" ||
          name == "--maximum-hls-resource-bytes" ||
          name == "--maximum-hls-total-bytes";
      if (!supported) {
        return fail<std::vector<GroupedVideoIngest>>(
            ErrorCode::invalid_argument,
            "unknown grouped video ingest option: " + std::string{name});
      }
      if (!values.emplace(name, group[index + 1]).second) {
        return fail<std::vector<GroupedVideoIngest>>(
            ErrorCode::invalid_argument,
            "duplicate grouped video ingest option: " + std::string{name});
      }
      index += 2U;
    }

    const auto source = values.find("--source");
    const auto label = values.find("--label");
    const auto start = values.find("--start-ns");
    const auto end = values.find("--end-ns");
    if (source == values.end() || label == values.end() ||
        start == values.end() || end == values.end() || source->second.empty() ||
        label->second.empty()) {
      return fail<std::vector<GroupedVideoIngest>>(
          ErrorCode::invalid_argument,
          "each --video group requires --source, --label, --start-ns, and --end-ns");
    }

    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    if (!parse_grouped_decimal(start->second, start_ns) ||
        !parse_grouped_decimal(end->second, end_ns) || end_ns <= start_ns) {
      return fail<std::vector<GroupedVideoIngest>>(
          ErrorCode::invalid_argument,
          "each --video group requires a valid positive start/end interval");
    }

    PixelLayout layout = PixelLayout::yuv420p8;
    if (const auto found = values.find("--layout"); found != values.end()) {
      const auto parsed = parse_grouped_layout(found->second);
      if (!parsed) {
        return fail<std::vector<GroupedVideoIngest>>(
            ErrorCode::invalid_argument,
            "grouped video ingest --layout must be gray8, rgb24, rgba32, or yuv420p8");
      }
      layout = *parsed;
    }

    std::uint64_t maximum_source_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_decoded_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::size_t maximum_frames = 4096U;
    std::size_t maximum_hls_resources = 256U;
    std::uint64_t maximum_hls_resource_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t maximum_hls_total_bytes = 1024ULL * 1024ULL * 1024ULL;

    const auto parse_u64 = [&values](std::string_view name,
                                     std::uint64_t& output) -> Result<void> {
      const auto found = values.find(name);
      if (found == values.end()) return {};
      if (!parse_grouped_decimal(found->second, output) || output == 0U) {
        return fail(ErrorCode::invalid_argument,
                    std::string{name} + " must be a positive integer");
      }
      return {};
    };
    auto parsed_source = parse_u64("--maximum-source-bytes", maximum_source_bytes);
    if (!parsed_source) return parsed_source.error();
    auto parsed_decoded =
        parse_u64("--maximum-decoded-bytes", maximum_decoded_bytes);
    if (!parsed_decoded) return parsed_decoded.error();
    auto parsed_hls_resource =
        parse_u64("--maximum-hls-resource-bytes", maximum_hls_resource_bytes);
    if (!parsed_hls_resource) return parsed_hls_resource.error();
    auto parsed_hls_total =
        parse_u64("--maximum-hls-total-bytes", maximum_hls_total_bytes);
    if (!parsed_hls_total) return parsed_hls_total.error();

    const auto parse_size = [&values](std::string_view name,
                                      std::size_t& output) -> Result<void> {
      const auto found = values.find(name);
      if (found == values.end()) return {};
      std::uint64_t parsed = 0U;
      if (!parse_grouped_decimal(found->second, parsed) || parsed == 0U ||
          parsed > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
        return fail(ErrorCode::invalid_argument,
                    std::string{name} +
                        " must be a positive process-sized integer");
      }
      output = static_cast<std::size_t>(parsed);
      return {};
    };
    auto parsed_frames = parse_size("--maximum-frames", maximum_frames);
    if (!parsed_frames) return parsed_frames.error();
    auto parsed_hls_resources =
        parse_size("--maximum-hls-resources", maximum_hls_resources);
    if (!parsed_hls_resources) return parsed_hls_resources.error();

    std::string identity = "codec.video.cli\n";
    identity += label->second;
    identity += '\n';
    identity += source->second;
    const auto stream = derive_stream_id(identity);
    if (!stream_ids.insert(stream).second) {
      return fail<std::vector<GroupedVideoIngest>>(
          ErrorCode::invalid_argument,
          "grouped video ingest contains duplicate derived stream identities");
    }

    groups.push_back(GroupedVideoIngest{
        .request = profiles::video::FfmpegVideoIngestRequest{
            .source_uri = std::string{source->second},
            .archive_path = archive_path,
            .descriptor = StreamDescriptor{
                .id = stream,
                .type = StreamType::video,
                .label = std::string{label->second},
                .source_id = "codec.video.cli",
                .payload_type = "application/octet-stream",
            },
            .start_ns = start_ns,
            .end_ns = end_ns,
            .output_layout = layout,
            .maximum_source_bytes = maximum_source_bytes,
            .maximum_decoded_bytes = maximum_decoded_bytes,
            .maximum_frames = maximum_frames,
            .maximum_hls_resources = maximum_hls_resources,
            .maximum_hls_resource_bytes = maximum_hls_resource_bytes,
            .maximum_hls_total_bytes = maximum_hls_total_bytes,
        },
        .layout = layout,
    });
  }
  return groups;
}

inline bool exact_link_matches(const RecordInfo& record,
                               const ProvenanceRecordLink& link) {
  return record.sequence == link.sequence && record.stream == link.stream &&
         record.type_code() == link.type && record.hash == link.hash;
}

inline Result<void> merge_group_archive(CodaWriter& writer,
                                        const std::filesystem::path& source_path) {
  auto archive = CodaArchive::open(source_path);
  if (!archive) return archive.error();
  const auto verified = archive->verify();
  if (!verified.ok || !verified.finalized) {
    return fail(ErrorCode::archive_corrupt,
                "temporary grouped video archive did not verify as finalized");
  }
  auto records = archive->records();
  if (!records) return records.error();
  auto provenance = archive->provenance();
  if (!provenance) return provenance.error();

  std::map<std::uint64_t, RecordInfo> old_records;
  for (const auto& record : *records) old_records.emplace(record.sequence, record);
  std::map<std::uint64_t, RecordInfo> rewritten;
  std::size_t provenance_index = 0U;

  for (const auto& record : *records) {
    if (record.type == RecordType::final_index) continue;
    if (record.type != RecordType::stream_provenance) {
      auto payload = archive->read_payload(record);
      if (!payload) return payload.error();
      auto appended = writer.append_raw(record.type_code(), record.stream,
                                        record.start_ns, record.end_ns, *payload);
      if (!appended) return appended.error();
      rewritten.emplace(record.sequence, *appended);
      continue;
    }

    if (provenance_index >= provenance->size()) {
      return fail(ErrorCode::archive_corrupt,
                  "temporary grouped video archive has missing provenance metadata");
    }
    const auto& sidecar = (*provenance)[provenance_index++];
    const auto old_subject = old_records.find(sidecar.subject.sequence);
    const auto new_subject = rewritten.find(sidecar.subject.sequence);
    if (old_subject == old_records.end() || new_subject == rewritten.end() ||
        !exact_link_matches(old_subject->second, sidecar.subject)) {
      return fail(ErrorCode::archive_corrupt,
                  "temporary grouped video provenance subject cannot be rewritten");
    }

    std::vector<RecordInfo> inputs;
    inputs.reserve(sidecar.inputs.size());
    for (const auto& input : sidecar.inputs) {
      const auto old_input = old_records.find(input.sequence);
      const auto new_input = rewritten.find(input.sequence);
      if (old_input == old_records.end() || new_input == rewritten.end() ||
          !exact_link_matches(old_input->second, input)) {
        return fail(ErrorCode::archive_corrupt,
                    "temporary grouped video provenance input cannot be rewritten");
      }
      inputs.push_back(new_input->second);
    }
    auto appended = writer.append_stream_provenance(
        new_subject->second, sidecar.subject_truth, inputs, sidecar.process);
    if (!appended) return appended.error();
    rewritten.emplace(record.sequence, *appended);
  }

  if (provenance_index != provenance->size()) {
    return fail(ErrorCode::archive_corrupt,
                "temporary grouped video archive has excess provenance metadata");
  }
  return {};
}

inline Result<void> merge_group_archives(
    const std::filesystem::path& destination,
    std::span<const std::filesystem::path> sources) {
  auto created = CodaWriter::create(destination);
  if (!created) return created.error();
  auto writer = std::move(*created);
  for (const auto& source : sources) {
    auto merged = merge_group_archive(writer, source);
    if (!merged) return merged.error();
  }
  return writer.finalize();
}

struct TemporaryVideoArchives {
  std::vector<std::filesystem::path> paths;
  ~TemporaryVideoArchives() {
    for (const auto& path : paths) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }
};

inline void print_group_report(
    const profiles::video::FfmpegVideoIngestReport& report,
    const GroupedVideoIngest& group,
    const std::filesystem::path& shared_archive) {
  std::uint64_t secondary_source_bytes = 0U;
  for (const auto& secondary : report.secondary_sources) {
    secondary_source_bytes += secondary.payload_size;
  }
  std::cout << "{\"archive\":\""
            << ::codec::detail::json_escape(shared_archive.string())
            << "\",\"stream_id\":\""
            << to_string(group.request.descriptor.id)
            << "\",\"layout\":\"" << grouped_layout_name(group.layout)
            << "\",\"source_bytes\":" << report.source.payload_size
            << ",\"frames\":" << report.states.size()
            << ",\"provenance\":" << report.provenance.size()
            << ",\"secondary_sources\":" << report.secondary_sources.size()
            << ",\"secondary_source_bytes\":" << secondary_source_bytes
            << ",\"state_exact\":"
            << (report.state_exact() ? "true" : "false");
  if (report.profile_error) {
    std::cout << ",\"profile_error\":\""
              << error_code_name(report.profile_error->code)
              << "\",\"profile_message\":\""
              << ::codec::detail::json_escape(report.profile_error->message)
              << "\"";
  }
  std::cout << "}\n";
}

}  // namespace detail

inline int grouped_video_ingest_command(
    std::span<const std::string_view> arguments) {
  if (arguments.size() < 3U || arguments[0] != "--archive" ||
      arguments[1].empty() || arguments[2] != "--video") {
    std::cerr << "codec: grouped video ingest requires --archive FILE followed by one or more --video groups\n";
    return 2;
  }

  const std::filesystem::path destination{std::string{arguments[1]}};
  auto destination_valid = detail::require_new_archive_path(destination);
  if (!destination_valid) {
    if (destination_valid.error().code == ErrorCode::invalid_argument) {
      std::cerr << "codec: " << destination_valid.error().message << '\n';
      return 2;
    }
    return detail::print_grouped_error(destination_valid.error());
  }

  auto groups = detail::parse_grouped_video_ingests(arguments.subspan(2),
                                                     destination);
  if (!groups) {
    std::cerr << "codec: " << groups.error().message << '\n';
    return 2;
  }

  if (!profiles::video::ffmpeg_video_ingest_available()) {
    return detail::print_grouped_error(
        Error{ErrorCode::model_incompatible,
              "FFmpeg video ingest backend is unavailable", false});
  }

  detail::TemporaryVideoArchives temporary;
  temporary.paths.reserve(groups->size());
  std::vector<std::optional<profiles::video::FfmpegVideoIngestReport>> reports(
      groups->size());
  std::vector<std::optional<Error>> errors(groups->size());

  for (std::size_t index = 0; index < groups->size(); ++index) {
    auto temporary_path = destination;
    temporary_path += ".video-" + std::to_string(index) + ".tmp";
    auto temporary_valid = detail::require_new_archive_path(temporary_path);
    if (!temporary_valid) {
      return detail::print_grouped_error(temporary_valid.error());
    }
    temporary.paths.push_back(temporary_path);
    (*groups)[index].request.archive_path = temporary_path;
  }

  std::vector<std::jthread> workers;
  workers.reserve(groups->size());
  for (std::size_t index = 0; index < groups->size(); ++index) {
    workers.emplace_back([&, index] {
      auto report =
          profiles::video::ingest_video_ffmpeg((*groups)[index].request);
      if (!report) {
        errors[index] = report.error();
        return;
      }
      reports[index] = std::move(*report);
    });
  }
  for (auto& worker : workers) worker.join();

  for (const auto& error : errors) {
    if (error) return detail::print_grouped_error(*error);
  }

  auto merged = detail::merge_group_archives(destination, temporary.paths);
  if (!merged) {
    std::error_code ignored;
    std::filesystem::remove(destination, ignored);
    return detail::print_grouped_error(merged.error());
  }

  bool profile_error = false;
  for (std::size_t index = 0; index < reports.size(); ++index) {
    if (!reports[index]) {
      return detail::print_grouped_error(
          Error{ErrorCode::internal,
                "grouped video worker produced no report or error", false});
    }
    detail::print_group_report(*reports[index], (*groups)[index], destination);
    profile_error = profile_error || reports[index]->profile_error.has_value();
  }
  return profile_error ? 1 : 0;
}

}  // namespace codec::cli
