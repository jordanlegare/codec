#include <codec/archive.hpp>
#include <codec/archive_follow.hpp>
#include <codec/audio.hpp>
#include <codec/engine.hpp>
#include <codec/statement.hpp>
#include <codec/watermark.hpp>

#include "../core/internal.hpp"

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
      << "  codec list streams ARCHIVE\n"
      << "  codec extract ARCHIVE --feed LABEL --fidelity source-exact [--follow] --output FILE\n"
      << "  codec extract ARCHIVE --stream STREAM_ID --fidelity source-exact [--follow] --output FILE\n"
      << "  codec repair ARCHIVE --output FILE\n"
      << "  codec watermark keygen --private KEY --public KEY\n"
      << "  codec watermark issue INPUT.wav --output OUTPUT.wav --statement FILE\n"
      << "       --private-key KEY --feed-uuid UUID --code UINT16 --issuer NAME\n"
      << "       --key-id ID --issued-at SEC --not-before SEC --expires-at SEC --w1|--w2\n"
      << "  codec watermark detect INPUT.wav [--statement FILE --public-key KEY]\n"
      << "       [--at SEC] [--format jsonl]\n";
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

std::int64_t now_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

template <typename Integer>
std::optional<Integer> integer(std::string_view text, int base = 10) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoll(std::string{text}, &consumed, base);
    if (consumed != text.size() ||
        value < static_cast<long long>(std::numeric_limits<Integer>::min()) ||
        value > static_cast<long long>(std::numeric_limits<Integer>::max())) {
      return std::nullopt;
    }
    return static_cast<Integer>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint16_t> code_value(std::string_view text) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoul(std::string{text}, &consumed, 0);
    if (consumed != text.size() || value > 0xffffU) return std::nullopt;
    return static_cast<std::uint16_t>(value);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint8_t> hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<std::uint8_t>(value - 'A' + 10);
  }
  return std::nullopt;
}

std::optional<codec::StreamId> stream_id(std::string_view text) {
  if (text.size() != 36) return std::nullopt;
  codec::StreamId output{};
  std::size_t byte_index = 0;
  for (std::size_t index = 0; index < text.size();) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (text[index] != '-') return std::nullopt;
      ++index;
      continue;
    }
    if (byte_index >= output.bytes.size() || index + 1 >= text.size()) {
      return std::nullopt;
    }
    const auto high = hex_nibble(text[index]);
    const auto low = hex_nibble(text[index + 1]);
    if (!high || !low) return std::nullopt;
    output.bytes[byte_index++] =
        static_cast<std::uint8_t>((*high << 4U) | *low);
    index += 2;
  }
  if (byte_index != output.bytes.size()) return std::nullopt;
  return output;
}

int print_error(const codec::Error& error) {
  std::cerr << "codec: " << codec::error_code_name(error.code) << ": "
            << error.message << '\n';
  return 1;
}

codec::Result<std::vector<std::byte>> read_bytes(
    const std::filesystem::path& path) {
  return codec::detail::read_file(path, 64ULL * 1024ULL * 1024ULL);
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
            << ",\"w0_ed25519\":" << (value.w0_ed25519 ? "true" : "false")
            << ",\"w1_reference\":" << (value.w1_reference ? "true" : "false")
            << ",\"w2_reference\":" << (value.w2_reference ? "true" : "false")
            << ",\"w2_policy\":\"qualified_paths_only\""
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
  if (arguments.size() < 2 ||
      (arguments.front() != "feeds" && arguments.front() != "streams")) {
    std::cerr << "codec: list supports: list feeds|streams ARCHIVE\n";
    return 2;
  }
  auto archive = codec::CodaArchive::open(std::string{arguments[1]});
  if (!archive) return print_error(archive.error());
  if (arguments.front() == "feeds") {
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
  auto streams = archive->streams();
  if (!streams) return print_error(streams.error());
  for (const auto& stream : *streams) {
    std::cout << "{\"stream_id\":\"" << codec::to_string(stream.id)
              << "\",\"type\":" << static_cast<std::uint16_t>(stream.type)
              << ",\"label\":\"" << codec::detail::json_escape(stream.label)
              << "\",\"source_id\":\""
              << codec::detail::json_escape(stream.source_id)
              << "\",\"payload_type\":\""
              << codec::detail::json_escape(stream.payload_type)
              << "\",\"fidelity\":\"S0\"}\n";
  }
  return 0;
}

int follow_extract(const codec::CodaArchive& archive,
                   const std::optional<std::string_view>& label,
                   std::optional<codec::StreamId> selected_stream,
                   const std::filesystem::path& output_path) {
  using namespace std::chrono_literals;

  // Legacy feed labels are descriptor metadata. E.3 record commits all feed
  // descriptors before producer start, but polling remains valid for archives
  // written by other compatible producers where the descriptor may appear later.
  if (label) {
    for (;;) {
      auto feeds = archive.feeds(codec::ArchiveReadPolicy::verified_prefix);
      if (!feeds) return print_error(feeds.error());
      std::optional<codec::StreamId> match;
      for (const auto& feed : *feeds) {
        if (feed.label != *label) continue;
        if (match) {
          std::cerr << "codec: archive_corrupt: duplicate feed label: "
                    << *label << '\n';
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
        std::cerr << "codec: feed label not found: " << *label << '\n';
        return 1;
      }
      std::this_thread::sleep_for(50ms);
    }
  }

  if (!selected_stream) {
    std::cerr << "codec: extract follow could not resolve a stream\n";
    return 1;
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
      if (label) {
        std::cout << "{\"feed\":\"" << codec::detail::json_escape(*label)
                  << "\",\"fidelity\":\"source_exact\",\"bytes\":"
                  << total_bytes << ",\"follow\":true}\n";
      } else {
        std::cout << "{\"stream_id\":\""
                  << codec::to_string(*selected_stream)
                  << "\",\"fidelity\":\"source_exact\",\"bytes\":"
                  << total_bytes << ",\"follow\":true}\n";
      }
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
  const auto stream_text = option(tail, "--stream");
  const auto output = option(tail, "--output");
  const auto fidelity = option(tail, "--fidelity");
  const bool follow = flag(tail, "--follow");
  if (label.has_value() == stream_text.has_value() || !output ||
      (fidelity && *fidelity != "source-exact")) {
    std::cerr << "codec: extract requires exactly one of --feed or --stream, "
                 "--output, and source-exact fidelity\n";
    return 2;
  }
  std::optional<codec::StreamId> selected_stream;
  if (stream_text) {
    selected_stream = stream_id(*stream_text);
    if (!selected_stream) {
      std::cerr << "codec: invalid stream ID: " << *stream_text << '\n';
      return 2;
    }
  }
  auto archive = codec::CodaArchive::open(std::string{arguments.front()});
  if (!archive) return print_error(archive.error());

  if (follow) {
    return follow_extract(*archive, label, selected_stream,
                          std::filesystem::path{std::string{*output}});
  }

  if (selected_stream) {
    const codec::RecordQuery query{
        .stream = *selected_stream,
        .type = codec::record_type_code(codec::RecordType::source_bytes),
        .sequence = std::nullopt,
        .time = std::nullopt,
    };
    auto source_records = archive->query_records(query);
    if (!source_records) return print_error(source_records.error());
    if (source_records->empty()) {
      std::cerr << "codec: stream ID not found: "
                << codec::to_string(*selected_stream) << '\n';
      return 1;
    }
  }
  auto extracted = selected_stream ? archive->extract_stream(*selected_stream)
                                   : archive->extract_feed(*label);
  if (!extracted) return print_error(extracted.error());
  auto written = write_bytes(std::string{*output}, *extracted);
  if (!written) return print_error(written.error());
  if (selected_stream) {
    std::cout << "{\"stream_id\":\"" << codec::to_string(*selected_stream)
              << "\",\"fidelity\":\"source_exact\",\"bytes\":"
              << extracted->size() << "}\n";
  } else {
    std::cout << "{\"feed\":\"" << codec::detail::json_escape(*label)
              << "\",\"fidelity\":\"source_exact\",\"bytes\":"
              << extracted->size() << "}\n";
  }
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

int watermark_keygen(const Strings& arguments) {
  const auto private_key = option(arguments, "--private");
  const auto public_key = option(arguments, "--public");
  if (!private_key || !public_key) {
    std::cerr << "codec: keygen requires --private and --public\n";
    return 2;
  }
  auto result = codec::generate_ed25519_keypair(std::string{*private_key},
                                                 std::string{*public_key});
  if (!result) return print_error(result.error());
  std::cout << "{\"algorithm\":\"Ed25519\",\"private_key_written\":true,"
               "\"public_key_written\":true}\n";
  return 0;
}

int watermark_issue(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: watermark issue requires an input WAV\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  const auto output = option(tail, "--output");
  const auto statement_path = option(tail, "--statement");
  const auto private_key = option(tail, "--private-key");
  const auto feed_uuid = option(tail, "--feed-uuid");
  const auto code_text = option(tail, "--code");
  const auto issuer = option(tail, "--issuer");
  const auto key_id = option(tail, "--key-id");
  const auto issued_text = option(tail, "--issued-at");
  const auto not_before_text = option(tail, "--not-before");
  const auto expires_text = option(tail, "--expires-at");
  const bool w2 = flag(tail, "--w2");
  if (!output || !statement_path || !private_key || !feed_uuid || !code_text ||
      !issuer || !key_id || !issued_text || !not_before_text || !expires_text ||
      (w2 && flag(tail, "--w1"))) {
    std::cerr << "codec: watermark issue is missing or has invalid arguments\n";
    return 2;
  }
  const auto code = code_value(*code_text);
  const auto issued = integer<std::int64_t>(*issued_text);
  const auto not_before = integer<std::int64_t>(*not_before_text);
  const auto expires = integer<std::int64_t>(*expires_text);
  if (!code || !issued || !not_before || !expires) {
    std::cerr << "codec: watermark code or statement time is invalid\n";
    return 2;
  }
  const auto acoustic_code = code.value();
  const auto issued_at = issued.value();
  const auto not_before_at = not_before.value();
  const auto expires_at = expires.value();
  auto audio = codec::WavPcm16::read(std::string{arguments.front()});
  if (!audio) return print_error(audio.error());
  const auto band = w2 ? codec::CarrierBand::w2 : codec::CarrierBand::w1;
  auto embedded = codec::embed_watermark(*audio, acoustic_code, band);
  if (!embedded) return print_error(embedded.error());
  codec::FeedStatement statement{.feed_uuid = std::string{*feed_uuid},
                                 .acoustic_code = acoustic_code,
                                 .issued_at_seconds = issued_at,
                                 .not_before_seconds = not_before_at,
                                 .expires_at_seconds = expires_at,
                                 .issuer = std::string{*issuer},
                                 .key_id = std::string{*key_id}};
  auto cose = codec::issue_statement(statement, std::string{*private_key});
  if (!cose) return print_error(cose.error());
  auto audio_written = audio->write(std::string{*output});
  if (!audio_written) return print_error(audio_written.error());
  auto statement_written = write_bytes(std::string{*statement_path}, *cose);
  if (!statement_written) {
    std::error_code ignored;
    std::filesystem::remove(std::string{*output}, ignored);
    return print_error(statement_written.error());
  }
  std::cout << "{\"band\":\"" << codec::carrier_band_name(band)
            << "\",\"code\":" << acoustic_code
            << ",\"frames_embedded\":" << embedded->frames_embedded
            << ",\"derived_audio\":true,\"original_modified\":false}\n";
  return 0;
}

int watermark_detect(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: watermark detect requires an input WAV\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  auto audio = codec::WavPcm16::read(std::string{arguments.front()});
  if (!audio) return print_error(audio.error());
  auto observations = codec::detect_watermarks(*audio);
  if (!observations) return print_error(observations.error());
  const auto statement_path = option(tail, "--statement");
  const auto public_key = option(tail, "--public-key");
  const auto at_text = option(tail, "--at");
  const auto at = at_text ? integer<std::int64_t>(*at_text)
                          : std::optional<std::int64_t>{now_seconds()};
  if ((statement_path.has_value() != public_key.has_value()) || !at) {
    std::cerr << "codec: --statement and --public-key must be supplied together\n";
    return 2;
  }
  std::optional<codec::StatementVerification> verification;
  std::string statement_error;
  if (statement_path) {
    auto cose = read_bytes(std::string{*statement_path});
    if (!cose) return print_error(cose.error());
    auto checked = codec::verify_statement(*cose, std::string{*public_key}, *at);
    if (checked) verification = std::move(*checked);
    else statement_error = checked.error().message;
  }
  for (const auto& observation : *observations) {
    const bool signature_bound =
        verification && verification->verified() &&
        verification->statement.acoustic_code == observation.code;
    const auto matching_hops = static_cast<std::size_t>(std::count_if(
        observations->begin(), observations->end(), [&](const auto& item) {
          return item.band == observation.band && item.code == observation.code;
        }));
    const auto state = signature_bound && matching_hops >= 3
                           ? "signature_bound_candidate"
                           : "candidate";
    std::cout << "{\"state\":\"" << state << "\",\"band\":\""
              << codec::carrier_band_name(observation.band)
              << "\",\"code\":" << observation.code
              << ",\"confidence\":" << observation.confidence
              << ",\"start_frame\":" << observation.start_frame
              << ",\"end_frame\":" << observation.end_frame
              << ",\"confirmation_hops\":" << matching_hops
              << ",\"detection_statistic\":\"goertzel_bin_dominance\""
              << ",\"waveform_spike\":false,\"authoritative\":false"
              << ",\"replay_check\":\"unavailable_in_stateless_reference_detector\"";
    if (verification) {
      std::cout << ",\"statement_state\":\""
                << codec::statement_state_name(verification->state) << "\""
                << ",\"claimed_feed_uuid\":\""
                << codec::detail::json_escape(
                       verification->statement.feed_uuid)
                << "\"";
      if (signature_bound) {
        std::cout << ",\"issuer\":\""
                  << codec::detail::json_escape(verification->statement.issuer)
                  << "\"";
      }
    } else if (!statement_error.empty()) {
      std::cout << ",\"statement_state\":\"malformed\",\"statement_error\":\""
                << codec::detail::json_escape(statement_error) << "\"";
    } else {
      std::cout << ",\"statement_state\":\"absent\"";
    }
    std::cout << "}\n";
  }
  return observations->empty() ? 4 : 0;
}

int watermark_command(const Strings& arguments) {
  if (arguments.empty()) {
    std::cerr << "codec: watermark subcommand required\n";
    return 2;
  }
  const Strings tail(arguments.begin() + 1, arguments.end());
  if (arguments.front() == "keygen") return watermark_keygen(tail);
  if (arguments.front() == "issue") return watermark_issue(tail);
  if (arguments.front() == "detect" || arguments.front() == "watch") {
    return watermark_detect(tail);
  }
  std::cerr << "codec: unknown watermark subcommand\n";
  return 2;
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
  if (command == "watermark") return watermark_command(arguments);
  std::cerr << "codec: unknown command: " << command << '\n';
  usage(std::cerr);
  return 2;
}