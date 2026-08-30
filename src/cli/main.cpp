#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>
#include <codec/engine.hpp>

#include "../core/internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
  if (command == "verify") return verify_command(arguments, false);
  if (command == "inspect") return verify_command(arguments, true);
  if (command == "list") return list_command(arguments);
  if (command == "extract") return extract_command(arguments);
  if (command == "repair") return repair_command(arguments);
  std::cerr << "codec: unknown command: " << command << '\n';
  usage(std::cerr);
  return 2;
}
