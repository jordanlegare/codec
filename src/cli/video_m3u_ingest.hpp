#pragma once

#include "video_multi_ingest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace codec::cli {
namespace detail {

constexpr std::size_t maximum_m3u_entries = 256U;
constexpr std::size_t maximum_m3u_line_bytes = 64U * 1024U;
constexpr std::size_t maximum_m3u_file_bytes = 4U * 1024U * 1024U;

inline std::string_view trim_m3u(std::string_view text) noexcept {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1U);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1U);
  }
  return text;
}

inline bool m3u_has_uri_scheme(std::string_view text) noexcept {
  const auto colon = text.find(':');
  if (colon == std::string_view::npos || colon == 0U) return false;
  if (colon == 1U && text.size() > 2U &&
      ((text[2] == '/') || (text[2] == '\\'))) {
    return false;
  }
  if (std::isalpha(static_cast<unsigned char>(text.front())) == 0) return false;
  for (std::size_t index = 1U; index < colon; ++index) {
    const auto ch = static_cast<unsigned char>(text[index]);
    if (std::isalnum(ch) == 0 && ch != '+' && ch != '-' && ch != '.') {
      return false;
    }
  }
  return true;
}

inline Result<std::optional<std::int64_t>> parse_m3u_duration_ns(
    std::string_view field) {
  field = trim_m3u(field);
  const auto metadata = field.find_first_of(" \t");
  const auto token = trim_m3u(field.substr(0U, metadata));
  if (token.empty()) {
    return fail<std::optional<std::int64_t>>(
        ErrorCode::invalid_argument, "M3U EXTINF duration is empty");
  }

  std::size_t cursor = 0U;
  bool negative = false;
  if (token[cursor] == '+' || token[cursor] == '-') {
    negative = token[cursor] == '-';
    ++cursor;
  }

  std::uint64_t seconds = 0U;
  bool saw_digit = false;
  while (cursor < token.size() && token[cursor] >= '0' &&
         token[cursor] <= '9') {
    saw_digit = true;
    const auto digit = static_cast<std::uint64_t>(token[cursor] - '0');
    if (seconds > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      return fail<std::optional<std::int64_t>>(
          ErrorCode::invalid_argument, "M3U EXTINF duration is too large");
    }
    seconds = seconds * 10U + digit;
    ++cursor;
  }

  std::uint64_t fraction_ns = 0U;
  std::uint64_t scale = 100000000U;
  if (cursor < token.size() && token[cursor] == '.') {
    ++cursor;
    while (cursor < token.size() && token[cursor] >= '0' &&
           token[cursor] <= '9') {
      saw_digit = true;
      if (scale != 0U) {
        fraction_ns += static_cast<std::uint64_t>(token[cursor] - '0') * scale;
        scale /= 10U;
      }
      ++cursor;
    }
  }
  if (!saw_digit || cursor != token.size()) {
    return fail<std::optional<std::int64_t>>(
        ErrorCode::invalid_argument, "M3U EXTINF duration is invalid");
  }
  if (negative) return std::optional<std::int64_t>{};
  if (seconds > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max()) /
                    1000000000ULL) {
    return fail<std::optional<std::int64_t>>(
        ErrorCode::invalid_argument, "M3U EXTINF duration is too large");
  }
  const auto second_ns = seconds * 1000000000ULL;
  const auto maximum =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (second_ns > maximum - fraction_ns) {
    return fail<std::optional<std::int64_t>>(
        ErrorCode::invalid_argument, "M3U EXTINF duration is too large");
  }
  const auto duration = second_ns + fraction_ns;
  if (duration == 0U) return std::optional<std::int64_t>{};
  return std::optional<std::int64_t>{static_cast<std::int64_t>(duration)};
}

inline std::string m3u_fallback_label(std::string_view source,
                                      std::size_t index) {
  auto candidate = source;
  const auto suffix = candidate.find_first_of("?#");
  if (suffix != std::string_view::npos) candidate = candidate.substr(0U, suffix);
  const auto slash = candidate.find_last_of("/\\");
  if (slash != std::string_view::npos) candidate.remove_prefix(slash + 1U);

  std::string label;
  label.reserve(std::min<std::size_t>(candidate.size(), 96U));
  for (const unsigned char ch : candidate) {
    if (label.size() >= 96U) break;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
      label.push_back(static_cast<char>(ch));
    } else {
      label.push_back('_');
    }
  }
  while (!label.empty() && label.front() == '.') label.erase(label.begin());
  if (label.empty() || label == "." || label == "..") {
    return "video-" + std::to_string(index + 1U);
  }
  return label;
}

struct M3uCommandOptions {
  std::int64_t start_ns{0};
  std::optional<std::int64_t> fallback_end_ns;
  std::vector<std::pair<std::string, std::string>> common_video_options;
};

inline Result<M3uCommandOptions> parse_m3u_command_options(
    std::span<const std::string_view> arguments) {
  M3uCommandOptions parsed;
  std::set<std::string_view> seen;
  constexpr std::array<std::string_view, 8U> common_options{
      "--layout",
      "--maximum-source-bytes",
      "--maximum-decoded-bytes",
      "--maximum-decoded-audio-bytes",
      "--maximum-frames",
      "--maximum-hls-resources",
      "--maximum-hls-resource-bytes",
      "--maximum-hls-total-bytes",
  };

  for (std::size_t index = 0U; index < arguments.size();) {
    const auto name = arguments[index];
    if (name.empty() || name.rfind("--", 0U) != 0U ||
        index + 1U >= arguments.size() ||
        arguments[index + 1U].rfind("--", 0U) == 0U) {
      return fail<M3uCommandOptions>(
          ErrorCode::invalid_argument,
          "M3U video ingest options require NAME VALUE pairs");
    }
    if (!seen.insert(name).second) {
      return fail<M3uCommandOptions>(ErrorCode::invalid_argument,
                                     "duplicate M3U video ingest option: " +
                                         std::string{name});
    }
    const auto value = arguments[index + 1U];
    if (name == "--start-ns") {
      if (!parse_grouped_decimal(value, parsed.start_ns)) {
        return fail<M3uCommandOptions>(
            ErrorCode::invalid_argument,
            "M3U video ingest --start-ns must be an integer");
      }
    } else if (name == "--end-ns") {
      std::int64_t end_ns = 0;
      if (!parse_grouped_decimal(value, end_ns)) {
        return fail<M3uCommandOptions>(
            ErrorCode::invalid_argument,
            "M3U video ingest --end-ns must be an integer");
      }
      parsed.fallback_end_ns = end_ns;
    } else if (std::find(common_options.begin(), common_options.end(), name) !=
               common_options.end()) {
      parsed.common_video_options.emplace_back(std::string{name},
                                               std::string{value});
    } else {
      return fail<M3uCommandOptions>(ErrorCode::invalid_argument,
                                     "unknown M3U video ingest option: " +
                                         std::string{name});
    }
    index += 2U;
  }

  if (parsed.fallback_end_ns && *parsed.fallback_end_ns <= parsed.start_ns) {
    return fail<M3uCommandOptions>(
        ErrorCode::invalid_argument,
        "M3U video ingest requires --end-ns greater than --start-ns");
  }
  return parsed;
}

struct M3uEntry {
  std::string source;
  std::string label;
  std::optional<std::int64_t> duration_ns;
};

inline Result<std::vector<M3uEntry>> read_m3u_entries(
    const std::filesystem::path& playlist_path) {
  std::ifstream input(playlist_path, std::ios::binary);
  if (!input) {
    return fail<std::vector<M3uEntry>>(
        ErrorCode::archive_io,
        "cannot open M3U playlist: " + playlist_path.string());
  }

  std::vector<M3uEntry> entries;
  std::set<std::string> normalized_sources;
  std::optional<std::string> pending_title;
  std::optional<std::int64_t> pending_duration;
  bool pending_extinf = false;
  bool first_line = true;
  std::size_t total_bytes = 0U;
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() > maximum_m3u_line_bytes ||
        total_bytes > maximum_m3u_file_bytes -
                          std::min(maximum_m3u_file_bytes, line.size())) {
      return fail<std::vector<M3uEntry>>(
          ErrorCode::resource_exhausted,
          "M3U playlist exceeds configured text limits");
    }
    total_bytes += line.size();
    if (total_bytes < maximum_m3u_file_bytes) ++total_bytes;

    if (first_line) {
      first_line = false;
      if (line.size() >= 3U &&
          static_cast<unsigned char>(line[0]) == 0xefU &&
          static_cast<unsigned char>(line[1]) == 0xbbU &&
          static_cast<unsigned char>(line[2]) == 0xbfU) {
        line.erase(0U, 3U);
      }
    }

    const auto text = trim_m3u(line);
    if (text.empty()) continue;
    if (text.rfind("#EXTINF:", 0U) == 0U) {
      if (pending_extinf) {
        return fail<std::vector<M3uEntry>>(
            ErrorCode::invalid_argument,
            "M3U playlist contains consecutive EXTINF entries without media");
      }
      const auto body = text.substr(8U);
      const auto comma = body.find(',');
      if (comma == std::string_view::npos) {
        return fail<std::vector<M3uEntry>>(
            ErrorCode::invalid_argument,
            "M3U EXTINF entry requires a comma before its title");
      }
      auto duration = parse_m3u_duration_ns(body.substr(0U, comma));
      if (!duration) return duration.error();
      pending_duration = *duration;
      const auto title = trim_m3u(body.substr(comma + 1U));
      pending_title = title.empty() ? std::optional<std::string>{}
                                    : std::optional<std::string>{std::string{title}};
      pending_extinf = true;
      continue;
    }
    if (text.front() == '#') continue;

    if (entries.size() >= maximum_m3u_entries) {
      return fail<std::vector<M3uEntry>>(
          ErrorCode::resource_exhausted,
          "M3U playlist exceeds the 256-entry ingest limit");
    }

    const std::string original_source{text};
    std::string source = original_source;
    if (!m3u_has_uri_scheme(text)) {
      std::filesystem::path source_path{original_source};
      if (!source_path.is_absolute()) {
        source_path = playlist_path.parent_path() / source_path;
      }
      source = source_path.lexically_normal().string();
    }
    if (!normalized_sources.insert(source).second) {
      return fail<std::vector<M3uEntry>>(
          ErrorCode::invalid_argument,
          "M3U playlist contains a duplicate media source: " + source);
    }

    entries.push_back(M3uEntry{
        .source = std::move(source),
        .label = pending_title.value_or(
            m3u_fallback_label(original_source, entries.size())),
        .duration_ns = pending_extinf ? pending_duration : std::nullopt,
    });
    pending_title.reset();
    pending_duration.reset();
    pending_extinf = false;
  }
  if (input.bad()) {
    return fail<std::vector<M3uEntry>>(
        ErrorCode::archive_io,
        "cannot read M3U playlist: " + playlist_path.string());
  }
  if (pending_extinf) {
    return fail<std::vector<M3uEntry>>(
        ErrorCode::invalid_argument,
        "M3U playlist ends with EXTINF metadata without a media entry");
  }
  if (entries.empty()) {
    return fail<std::vector<M3uEntry>>(
        ErrorCode::invalid_argument,
        "M3U playlist contains no media entries");
  }
  return entries;
}

inline Result<std::vector<std::string>> expand_m3u_ingest_arguments(
    std::span<const std::string_view> arguments) {
  if (arguments.size() < 4U || arguments[0] != "--archive" ||
      arguments[1].empty() || arguments[2] != "--m3u" ||
      arguments[3].empty()) {
    return fail<std::vector<std::string>>(
        ErrorCode::invalid_argument,
        "M3U video ingest requires --archive FILE --m3u PLAYLIST");
  }

  auto options = parse_m3u_command_options(arguments.subspan(4U));
  if (!options) return options.error();
  const std::filesystem::path playlist_path{std::string{arguments[3]}};
  auto entries = read_m3u_entries(playlist_path);
  if (!entries) return entries.error();

  std::vector<std::string> expanded;
  expanded.reserve(2U + entries->size() *
                            (9U + options->common_video_options.size() * 2U));
  expanded.emplace_back("--archive");
  expanded.emplace_back(arguments[1]);
  for (const auto& entry : *entries) {
    std::int64_t end_ns = 0;
    if (entry.duration_ns) {
      if (options->start_ns >
          std::numeric_limits<std::int64_t>::max() - *entry.duration_ns) {
        return fail<std::vector<std::string>>(
            ErrorCode::invalid_argument,
            "M3U entry duration overflows the requested start time");
      }
      end_ns = options->start_ns + *entry.duration_ns;
    } else if (options->fallback_end_ns) {
      end_ns = *options->fallback_end_ns;
    } else {
      return fail<std::vector<std::string>>(
          ErrorCode::invalid_argument,
          "M3U entries without a positive EXTINF duration require --end-ns");
    }

    expanded.emplace_back("--video");
    expanded.emplace_back("--source");
    expanded.push_back(entry.source);
    expanded.emplace_back("--label");
    expanded.push_back(entry.label);
    expanded.emplace_back("--start-ns");
    expanded.push_back(std::to_string(options->start_ns));
    expanded.emplace_back("--end-ns");
    expanded.push_back(std::to_string(end_ns));
    for (const auto& [name, value] : options->common_video_options) {
      expanded.push_back(name);
      expanded.push_back(value);
    }
  }
  return expanded;
}

}  // namespace detail

inline int m3u_video_ingest_command(
    std::span<const std::string_view> arguments) {
  auto expanded = detail::expand_m3u_ingest_arguments(arguments);
  if (!expanded) {
    if (expanded.error().code == ErrorCode::invalid_argument) {
      std::cerr << "codec: " << expanded.error().message << '\n';
      return 2;
    }
    return detail::print_grouped_error(expanded.error());
  }

  std::vector<std::string_view> views;
  views.reserve(expanded->size());
  for (const auto& argument : *expanded) views.push_back(argument);
  return grouped_video_ingest_command(views);
}

}  // namespace codec::cli
