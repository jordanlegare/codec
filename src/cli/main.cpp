#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>
#include <codec/engine.hpp>
#include <codec/profiles/video.hpp>
#include <codec/profiles/video_export.hpp>

#include "../core/internal.hpp"
#include "video_multi_ingest.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using Strings = std::vector<std::string_view>;

void usage(std::ostream& output) {
  output
      << "CODEC " << CODEC_VERSION_STRING
      << " - preservation-first audio feed archive and identity\n\n"
      << "Usage:\n"
      << "  codec capabilities\n"
      << "  codec record --archive FILE --feed LABEL=URI [--feed ...]\n"
      << "  codec video ingest --source URI --archive FILE --label LABEL --start-ns NS --end-ns NS [--layout gray8|rgb24|rgba32|yuv420p8] [--maximum-source-bytes N] [--maximum-decoded-bytes N] [--maximum-frames N] [--maximum-hls-resources N] [--maximum-hls-resource-bytes N] [--maximum-hls-total-bytes N]\n"
      << "  codec video ingest --archive FILE --video --source URI --label LABEL --start-ns NS --end-ns NS [video options] [--video ...]\n"
      << "  codec video export ARCHIVE --stream UUID --output FILE [--maximum-frames N] [--maximum-input-bytes N] [--maximum-output-bytes N]\n"
      << "  codec inspect ARCHIVE\n"
      << "  codec verify ARCHIVE [--level full]\n"
      << "  codec list feeds ARCHIVE\n"
      << "  codec extract ARCHIVE --feed LABEL [--fidelity source-exact] [--follow] --output FILE\n"
      << "  codec repair ARCHIVE --output FILE\n";
}

std::optional<std::string_view> option(const Strings& arguments,
                                       std::string_view name) {
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] == name) return arguments[index + 1];
  }
  return std::nullopt;
}

bool flag(const Strings& arguments, std::string_view name) {
  return std::find(arguments.begin(), arguments.end(), name) !=
         arguments.end();
}

template <typename Integer>
bool parse_decimal(std::string_view text, Integer& value) {
  if (text.empty()) return false;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto [next, error] = std::from_chars(begin, end, value, 10);
  return error == std::errc{} && next == end;
}

int print_error(const codec::Error& error) {
  std::cerr << "codec: " << codec::error_code_name(error.code) << ": "
            << error.message << '\n';
  return 1;
}

codec::Result<void> write_bytes(const std::filesystem::path& path,
                                std::span<const std::byte> bytes) {
  return codec::detail::write_file(path, bytes);
}

int capabilities_command() {
  const auto value = codec::Engine::capabilities();
  std::cout << "{\"version\":\"" << CODEC_VERSION_STRING << "\","
            << "\"coda_archive\":" << (value.coda_archive ? "true" : "false")
            << ",\"file_capture\":" << (value.file_capture ? "true" : "false")
            << ",\"http_capture\":" << (value.http_capture ? "true" : "false")
            << ",\"pcm16_wav\":" << (value.pcm16_wav ? "true" : "false")
            << ",\"neural_separation\":"
            << (value.neural_separation ? "true" : "false")
            << ",\"gpu_inference\":"
            << (value.gpu_inference ? "true" : "false") << "}\n";
  return 0;
}

int record_command(const Strings& arguments) {
  const auto archive = option(arguments, "--archive");
  if (!archive) {
    std::cerr << "codec: record requires --archive\n";
    return 2;
  }
  std::vector<codec::FeedSpec> feeds;
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] != "--feed") continue;
    const auto specification = arguments[index + 1];
    const auto separator = specification.find('=');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 == specification.size()) {
      std::cerr << "codec: --feed must be LABEL=URI\n";
      return 2;
    }
    feeds.push_back({.uri = std::string{specification.substr(separator + 1)},
                     .label = std::string{specification.substr(0, separator)}});
  }
  auto engine = codec::Engine::create({});
  if (!engine) return print_error(engine.error());
  auto report = engine->record(feeds, std::string{*archive});
  if (!report) return print_error(report.error());
  std::cout << "{\"archive\":\""
            << codec::detail::json_escape(report->archive.string())
            << "\",\"feeds_recorded\":" << report->feeds_recorded
            << ",\"source_bytes\":" << report->source_bytes
            << ",\"source_records\":" << report->source_records << "}\n";
  return 0;
}

std::optional<codec::profiles::video::PixelLayout> parse_video_layout(
    std::string_view text) {
  using codec::profiles::video::PixelLayout;
  if (text == "gray8") return PixelLayout::gray8;
  if (text == "rgb24") return PixelLayout::rgb24;
  if (text == "rgba32") return PixelLayout::rgba32;
  if (text == "yuv420p8") return PixelLayout::yuv420p8;
  return std::nullopt;
}

std::string_view video_layout_name(codec::profiles::video::PixelLayout layout) {
  using codec::profiles::video::PixelLayout;
  switch (layout) {
    case PixelLayout::gray8: return "gray8";
    case PixelLayout::rgb24: return "rgb24";
    case PixelLayout::rgba32: return "rgba32";
    case PixelLayout::yuv420p8: return "yuv420p8";
  }
  return "unknown";
}

int video_export_command(const Strings& arguments) {
  namespace video = codec::profiles::video;

  if (arguments.empty()) {
    std::cerr << "codec: video export requires an archive\n";
    return 2;
  }
  const auto archive_path = arguments.front();
  const Strings tail(arguments.begin() + 1, arguments.end());
  const auto stream_text = option(tail, "--stream");
  const auto output_text = option(tail, "--output");
  if (!stream_text || !output_text || stream_text->empty() ||
      output_text->empty()) {
    std::cerr << "codec: video export requires ARCHIVE, --stream, and --output\n";
    return 2;
  }

  std::size_t maximum_frames = 1024U;
  std::uint64_t maximum_input_bytes = 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t maximum_output_bytes = 1024ULL * 1024ULL * 1024ULL;
  if (const auto value = option(tail, "--maximum-frames")) {
    std::uint64_t parsed = 0U;
    if (!parse_decimal(*value, parsed) || parsed == 0U ||
        parsed >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      std::cerr << "codec: video export --maximum-frames must be a positive process-sized integer\n";
      return 2;
    }
    maximum_frames = static_cast<std::size_t>(parsed);
  }
  if (const auto value = option(tail, "--maximum-input-bytes")) {
    if (!parse_decimal(*value, maximum_input_bytes) ||
        maximum_input_bytes == 0U) {
      std::cerr << "codec: video export --maximum-input-bytes must be a positive integer\n";
      return 2;
    }
  }
  if (const auto value = option(tail, "--maximum-output-bytes")) {
    if (!parse_decimal(*value, maximum_output_bytes) ||
        maximum_output_bytes == 0U) {
      std::cerr << "codec: video export --maximum-output-bytes must be a positive integer\n";
      return 2;
    }
  }

  auto archive = codec::CodaArchive::open(std::string{archive_path});
  if (!archive) return print_error(archive.error());
  auto descriptors = archive->streams();
  if (!descriptors) return print_error(descriptors.error());

  std::optional<codec::StreamId> selected_stream;
  for (const auto& descriptor : *descriptors) {
    if (descriptor.type != codec::StreamType::video ||
        codec::to_string(descriptor.id) != *stream_text) {
      continue;
    }
    if (selected_stream) {
      return print_error(codec::Error{
          codec::ErrorCode::archive_corrupt,
          "duplicate video stream descriptor: " + std::string{*stream_text},
          false});
    }
    selected_stream = descriptor.id;
  }
  if (!selected_stream) {
    return print_error(codec::Error{
        codec::ErrorCode::invalid_argument,
        "video stream ID not found: " + std::string{*stream_text}, false});
  }

  auto exported = video::export_verified_video_mp4(
      *archive,
      video::VideoFrameQuery{
          .stream = *selected_stream,
          .time = std::nullopt,
          .maximum_results = maximum_frames,
          .maximum_encoded_bytes = maximum_input_bytes,
          .decode_limits = {},
      },
      video::VideoMp4ExportLimits{
          .maximum_output_bytes = maximum_output_bytes,
      });
  if (!exported) return print_error(exported.error());

  const std::filesystem::path output_path{std::string{*output_text}};
  auto written = write_bytes(output_path, exported->output.payload);
  if (!written) return print_error(written.error());

  std::cout << "{\"archive\":\""
            << codec::detail::json_escape(std::string{archive_path})
            << "\",\"stream_id\":\"" << codec::to_string(*selected_stream)
            << "\",\"output\":\""
            << codec::detail::json_escape(output_path.string())
            << "\",\"payload_type\":\""
            << codec::detail::json_escape(exported->output.payload_type)
            << "\",\"frames\":" << exported->state_records.size()
            << ",\"bytes\":" << exported->output.payload.size() << "}\n";
  return 0;
}

int video_command(const Strings& arguments) {
  if (!arguments.empty() && arguments.front() == "export") {
    return video_export_command(
        Strings(arguments.begin() + 1, arguments.end()));
  }
  if (arguments.empty() || arguments.front() != "ingest") {
    std::cerr << "codec: video supports: video ingest, video export\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  if (tail.size() >= 3U && tail[0] == "--archive" && tail[2] == "--video") {
    return codec::cli::grouped_video_ingest_command(tail);
  }
  const auto source = option(tail, "--source");
  const auto archive = option(tail, "--archive");
  const auto label = option(tail, "--label");
  const auto start_text = option(tail, "--start-ns");
  const auto end_text = option(tail, "--end-ns");
  const auto layout_text = option(tail, "--layout");
  if (!source || !archive || !label || !start_text || !end_text ||
      source->empty() || archive->empty() || label->empty()) {
    std::cerr << "codec: video ingest requires --source, --archive, --label, --start-ns, and --end-ns\n";
    return 2;
  }

  std::int64_t start_ns = 0;
  std::int64_t end_ns = 0;
  if (!parse_decimal(*start_text, start_ns) ||
      !parse_decimal(*end_text, end_ns) || end_ns <= start_ns) {
    std::cerr << "codec: video ingest requires a valid positive --start-ns/--end-ns interval\n";
    return 2;
  }

  const auto layout =
      parse_video_layout(layout_text.value_or(std::string_view{"yuv420p8"}));
  if (!layout) {
    std::cerr << "codec: video ingest --layout must be gray8, rgb24, rgba32, or yuv420p8\n";
    return 2;
  }

  std::uint64_t maximum_source_bytes = 1024ULL * 1024ULL * 1024ULL;
  std::uint64_t maximum_decoded_bytes = 1024ULL * 1024ULL * 1024ULL;
  std::size_t maximum_frames = 4096U;
  std::size_t maximum_hls_resources = 256U;
  std::uint64_t maximum_hls_resource_bytes = 64ULL * 1024ULL * 1024ULL;
  std::uint64_t maximum_hls_total_bytes = 1024ULL * 1024ULL * 1024ULL;
  if (const auto value = option(tail, "--maximum-source-bytes")) {
    if (!parse_decimal(*value, maximum_source_bytes) || maximum_source_bytes == 0U) {
      std::cerr << "codec: video ingest --maximum-source-bytes must be a positive integer\n";
      return 2;
    }
  }
  if (const auto value = option(tail, "--maximum-decoded-bytes")) {
    if (!parse_decimal(*value, maximum_decoded_bytes) || maximum_decoded_bytes == 0U) {
      std::cerr << "codec: video ingest --maximum-decoded-bytes must be a positive integer\n";
      return 2;
    }
  }
  if (const auto value = option(tail, "--maximum-frames")) {
    std::uint64_t parsed = 0U;
    if (!parse_decimal(*value, parsed) || parsed == 0U ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      std::cerr << "codec: video ingest --maximum-frames must be a positive process-sized integer\n";
      return 2;
    }
    maximum_frames = static_cast<std::size_t>(parsed);
  }
  if (const auto value = option(tail, "--maximum-hls-resources")) {
    std::uint64_t parsed = 0U;
    if (!parse_decimal(*value, parsed) || parsed == 0U ||
        parsed >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      std::cerr << "codec: video ingest --maximum-hls-resources must be a positive process-sized integer\n";
      return 2;
    }
    maximum_hls_resources = static_cast<std::size_t>(parsed);
  }
  if (const auto value = option(tail, "--maximum-hls-resource-bytes")) {
    if (!parse_decimal(*value, maximum_hls_resource_bytes) ||
        maximum_hls_resource_bytes == 0U) {
      std::cerr << "codec: video ingest --maximum-hls-resource-bytes must be a positive integer\n";
      return 2;
    }
  }
  if (const auto value = option(tail, "--maximum-hls-total-bytes")) {
    if (!parse_decimal(*value, maximum_hls_total_bytes) ||
        maximum_hls_total_bytes == 0U) {
      std::cerr << "codec: video ingest --maximum-hls-total-bytes must be a positive integer\n";
      return 2;
    }
  }

  std::string identity = "codec.video.cli\n";
  identity += *label;
  identity += '\n';
  identity += *source;
  const auto stream = codec::derive_stream_id(identity);
  codec::profiles::video::FfmpegVideoIngestRequest request{
      .source_uri = std::string{*source},
      .archive_path = std::filesystem::path{std::string{*archive}},
      .descriptor = codec::StreamDescriptor{
          .id = stream,
          .type = codec::StreamType::video,
          .label = std::string{*label},
          .source_id = "codec.video.cli",
          .payload_type = "application/octet-stream",
      },
      .start_ns = start_ns,
      .end_ns = end_ns,
      .output_layout = *layout,
      .maximum_source_bytes = maximum_source_bytes,
      .maximum_decoded_bytes = maximum_decoded_bytes,
      .maximum_frames = maximum_frames,
      .maximum_hls_resources = maximum_hls_resources,
      .maximum_hls_resource_bytes = maximum_hls_resource_bytes,
      .maximum_hls_total_bytes = maximum_hls_total_bytes,
  };
  auto report = codec::profiles::video::ingest_video_ffmpeg(request);
  if (!report) return print_error(report.error());

  std::uint64_t secondary_source_bytes = 0U;
  for (const auto& secondary : report->secondary_sources) {
    if (secondary.payload_size >
        std::numeric_limits<std::uint64_t>::max() - secondary_source_bytes) {
      return print_error(codec::Error{
          codec::ErrorCode::internal,
          "video ingest secondary source byte count overflowed", false});
    }
    secondary_source_bytes += secondary.payload_size;
  }

  std::cout << "{\"archive\":\""
            << codec::detail::json_escape(report->archive_path.string())
            << "\",\"stream_id\":\"" << codec::to_string(stream)
            << "\",\"layout\":\"" << video_layout_name(*layout)
            << "\",\"source_bytes\":" << report->source.payload_size
            << ",\"frames\":" << report->states.size()
            << ",\"provenance\":" << report->provenance.size()
            << ",\"secondary_sources\":"
            << report->secondary_sources.size()
            << ",\"secondary_source_bytes\":" << secondary_source_bytes
            << ",\"state_exact\":"
            << (report->state_exact() ? "true" : "false");
  if (report->profile_error) {
    std::cout << ",\"profile_error\":\""
              << codec::error_code_name(report->profile_error->code)
              << "\",\"profile_message\":\""
              << codec::detail::json_escape(report->profile_error->message)
              << "\"";
  }
  std::cout << "}\n";
  return report->profile_error ? 1 : 0;
}

int verify_command(const Strings& arguments, bool inspect) {
  if (arguments.empty()) {
    std::cerr << "codec: archive path required\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments.front()});
  if (!archive) return print_error(archive.error());
  const auto report = archive->verify();
  std::cout << "{\"ok\":" << (report.ok ? "true" : "false")
            << ",\"finalized\":" << (report.finalized ? "true" : "false")
            << ",\"committed_records\":" << report.committed_records
            << ",\"verified_payload_bytes\":"
            << report.verified_payload_bytes
            << ",\"valid_prefix_bytes\":" << report.valid_prefix_bytes
            << ",\"file_bytes\":" << report.file_bytes
            << ",\"message\":\""
            << codec::detail::json_escape(report.message) << "\"";
  if (inspect && report.ok) {
    auto feeds = archive->feeds();
    std::cout << ",\"feeds\":" << (feeds ? feeds->size() : 0);
  }
  std::cout << "}\n";
  return report.ok ? 0 : 3;
}

int list_command(const Strings& arguments) {
  if (arguments.size() < 2 || arguments.front() != "feeds") {
    std::cerr << "codec: list supports: list feeds ARCHIVE\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments[1]});
  if (!archive) return print_error(archive.error());
  auto feeds = archive->feeds();
  if (!feeds) return print_error(feeds.error());
  for (const auto& feed : *feeds) {
    std::cout << "{\"label\":\"" << codec::detail::json_escape(feed.label)
              << "\",\"stream_id\":\"" << codec::to_string(feed.stream)
              << "\",\"uri\":\"" << codec::detail::json_escape(feed.uri)
              << "\",\"fidelity\":\"S0\"}\n";
  }
  return 0;
}

int follow_extract(const codec::CodaArchive& archive, std::string_view label,
                   const std::filesystem::path& output_path) {
  using namespace std::chrono_literals;

  std::optional<codec::StreamId> selected_stream;
  for (;;) {
    auto feeds = archive.feeds(codec::ArchiveReadPolicy::verified_prefix);
    if (!feeds) return print_error(feeds.error());
    std::optional<codec::StreamId> match;
    for (const auto& feed : *feeds) {
      if (feed.label != label) continue;
      if (match) {
        std::cerr << "codec: archive_corrupt: duplicate feed label: "
                  << label << '\n';
        return 1;
      }
      match = feed.stream;
    }
    if (match) {
      selected_stream = *match;
      break;
    }
    auto records = archive.records(codec::ArchiveReadPolicy::verified_prefix);
    if (!records) return print_error(records.error());
    const bool finalized = std::any_of(
        records->begin(), records->end(), [](const codec::RecordInfo& record) {
          return record.type == codec::RecordType::final_index;
        });
    if (finalized) {
      std::cerr << "codec: feed label not found: " << label << '\n';
      return 1;
    }
    std::this_thread::sleep_for(50ms);
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "codec: archive_io: cannot open output: "
              << output_path.string() << '\n';
    return 1;
  }

  codec::SourceExactCursor cursor{};
  std::uint64_t total_bytes = 0;
  bool observed_stream = false;
  for (;;) {
    auto batch = codec::extract_stream_source_exact_prefix(
        archive, *selected_stream, cursor);
    if (!batch) return print_error(batch.error());
    cursor = batch->cursor;

    for (const auto& extracted : batch->records) {
      observed_stream = true;
      if (!extracted.payload.empty()) {
        output.write(reinterpret_cast<const char*>(extracted.payload.data()),
                     static_cast<std::streamsize>(extracted.payload.size()));
        if (!output) {
          std::cerr << "codec: archive_io: cannot write follow output\n";
          return 1;
        }
      }
      total_bytes += extracted.payload.size();
    }
    if (!batch->records.empty()) {
      output.flush();
      if (!output) {
        std::cerr << "codec: archive_io: cannot flush follow output\n";
        return 1;
      }
    }

    if (batch->finalized) {
      if (!observed_stream) {
        auto streams = archive.streams(codec::ArchiveReadPolicy::verified_prefix);
        if (!streams) return print_error(streams.error());
        const bool declared = std::any_of(
            streams->begin(), streams->end(), [&](const auto& descriptor) {
              return descriptor.id == *selected_stream;
            });
        if (!declared) {
          std::cerr << "codec: stream ID not found: "
                    << codec::to_string(*selected_stream) << '\n';
          return 1;
        }
      }
      std::cout << "{\"feed\":\"" << codec::detail::json_escape(label)
                << "\",\"fidelity\":\"source_exact\",\"bytes\":"
                << total_bytes << ",\"follow\":true}\n";
      return 0;
    }

    if (batch->records.empty()) std::this_thread::sleep_for(50ms);
  }
}

int extract_command(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: extract requires an archive\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  const auto label = option(tail, "--feed");
  const auto output = option(tail, "--output");
  const auto fidelity = option(tail, "--fidelity");
  const bool follow = flag(tail, "--follow");
  if (!label || !output || flag(tail, "--stream") ||
      (fidelity && *fidelity != "source-exact")) {
    std::cerr << "codec: extract requires --feed, --output, and source-exact fidelity\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments.front()});
  if (!archive) return print_error(archive.error());

  if (follow) {
    return follow_extract(*archive, *label,
                          std::filesystem::path{std::string{*output}});
  }

  auto extracted = archive->extract_feed(*label);
  if (!extracted) return print_error(extracted.error());
  auto written = write_bytes(std::string{*output}, *extracted);
  if (!written) return print_error(written.error());
  std::cout << "{\"feed\":\"" << codec::detail::json_escape(*label)
            << "\",\"fidelity\":\"source_exact\",\"bytes\":"
            << extracted->size() << "}\n";
  return 0;
}

int repair_command(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: repair requires an archive\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  const auto output = option(tail, "--output");
  if (!output) {
    std::cerr << "codec: repair requires --output\n";
    return 2;
  }
  auto report = codec::CodaArchive::repair(std::string{arguments.front()},
                                            std::string{*output});
  if (!report) return print_error(report.error());
  std::cout << "{\"recovered_records\":" << report->recovered_records
            << ",\"recovered_payload_bytes\":"
            << report->recovered_payload_bytes
            << ",\"discarded_tail_bytes\":"
            << report->discarded_tail_bytes << "}\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(std::cerr);
    return 2;
  }
  Strings arguments;
  for (int index = 2; index < argc; ++index) arguments.push_back(argv[index]);
  const std::string_view command = argv[1];
  if (command == "--help" || command == "help") {
    usage(std::cout);
    return 0;
  }
  if (command == "--version") {
    std::cout << "codec " << CODEC_VERSION_STRING << '\n';
    return 0;
  }
  if (command == "capabilities") return capabilities_command();
  if (command == "record") return record_command(arguments);
  if (command == "video") return video_command(arguments);
  if (command == "verify") return verify_command(arguments, false);
  if (command == "inspect") return verify_command(arguments, true);
  if (command == "list") return list_command(arguments);
  if (command == "extract") return extract_command(arguments);
  if (command == "repair") return repair_command(arguments);
  std::cerr << "codec: unknown command: " << command << '\n';
  usage(std::cerr);
  return 2;
}
