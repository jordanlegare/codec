#include <codec/engine.hpp>

#include "../capture/capture.hpp"
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>
#include <string_view>
#include <vector>

namespace codec {
namespace {

bool valid_label(std::string_view label) {
  if (label.empty() || label.size() > 128) return false;
  return std::all_of(label.begin(), label.end(), [](unsigned char ch) {
    return ch >= 0x21U && ch <= 0x7eU && ch != '=';
  });
}

std::string redacted_uri(std::string uri) {
  const auto scheme = uri.find("://");
  if (scheme != std::string::npos) {
    const auto authority_start = scheme + 3;
    const auto authority_end = uri.find_first_of("/?#", authority_start);
    const auto at = uri.find('@', authority_start);
    if (at != std::string::npos &&
        (authority_end == std::string::npos || at < authority_end)) {
      uri.erase(authority_start, at - authority_start + 1);
    }
    const auto query = uri.find_first_of("?#");
    if (query != std::string::npos) uri.erase(query);
  }
  return uri;
}

std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

Result<Engine> Engine::create(EngineConfig config) {
  if (config.capture_chunk_bytes < 4096 ||
      config.capture_chunk_bytes > 16U * 1024U * 1024U ||
      config.maximum_feed_bytes == 0 || config.maximum_redirects > 20) {
    return fail<Engine>(ErrorCode::invalid_argument,
                        "invalid engine capture limits");
  }
  return Engine{config};
}

Capabilities Engine::capabilities() noexcept { return {}; }

Result<RecordingReport> Engine::record(
    const std::vector<FeedSpec>& feeds,
    const std::filesystem::path& archive_path) const {
  if (feeds.empty()) {
    return fail<RecordingReport>(ErrorCode::invalid_argument,
                                 "at least one feed is required");
  }
  std::set<std::string> labels;
  for (const auto& feed : feeds) {
    if (!valid_label(feed.label) || feed.uri.empty() ||
        !feed.preserve_source || feed.maximum_bytes == 0) {
      return fail<RecordingReport>(
          ErrorCode::invalid_argument,
          "each feed requires a unique printable label, URI, and S0 preservation");
    }
    if (!labels.insert(feed.label).second) {
      return fail<RecordingReport>(ErrorCode::invalid_argument,
                                   "feed labels must be unique");
    }
  }
  std::vector<detail::PreparedCapture> prepared;
  prepared.reserve(feeds.size());
  for (const auto& spec : feeds) {
    detail::CaptureOptions options;
    options.chunk_bytes = config_.capture_chunk_bytes;
    options.maximum_bytes =
        std::min(config_.maximum_feed_bytes, spec.maximum_bytes);
    options.maximum_redirects = config_.maximum_redirects;
    options.deny_private_network = config_.deny_private_network;
    auto source = detail::PreparedCapture::prepare(spec.uri, options);
    if (!source) return source.error();
    prepared.push_back(std::move(*source));
  }
  auto writer_result = CodaWriter::create(archive_path);
  if (!writer_result) return writer_result.error();
  auto writer = std::move(*writer_result);
  RecordingReport report;
  report.archive = archive_path;
  for (std::size_t feed_index = 0; feed_index < feeds.size(); ++feed_index) {
    const auto& spec = feeds[feed_index];
    const auto stream = derive_stream_id(spec.label + "\n" + spec.uri);
    FeedInfo info{stream, spec.label, redacted_uri(spec.uri), true};
    const auto descriptor = detail::encode_feed_descriptor(info);
    if (descriptor.empty()) {
      return fail<RecordingReport>(ErrorCode::invalid_argument,
                                   "feed descriptor is too large");
    }
    const auto timestamp = now_ns();
    auto descriptor_record = writer.append(RecordType::feed_descriptor, stream,
                                           timestamp, timestamp, descriptor);
    if (!descriptor_record) return descriptor_record.error();
    auto captured = prepared[feed_index].run(
        [&](std::span<const std::byte> bytes) -> Result<void> {
          const auto observed = now_ns();
          auto appended = writer.append(RecordType::source_bytes, stream,
                                        observed, observed, bytes);
          if (!appended) return appended.error();
          report.source_bytes += bytes.size();
          report.source_records += 1;
          return {};
        });
    if (!captured) return captured.error();
    report.feeds_recorded += 1;
  }
  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  return report;
}

}  // namespace codec
