#include <codec/engine.hpp>

#include "../capture/capture.hpp"
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <thread>
#include <utility>
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

struct PreparedSource {
  StreamId stream{};
  detail::PreparedCapture capture;
};

struct PendingChunk {
  std::vector<std::byte> bytes;
  std::int64_t observed_ns{};
};

struct ProducerState {
  std::deque<PendingChunk> chunks;
  bool done{false};
};

struct SchedulerState {
  explicit SchedulerState(std::size_t count) : producers(count) {}

  std::mutex mutex;
  std::condition_variable changed;
  std::vector<ProducerState> producers;
  std::uint64_t pending_bytes{};
  bool cancelled{false};
  std::optional<Error> first_error;
};

template <typename Spec, typename StreamForSpec>
Result<std::vector<PreparedSource>> prepare_sources(
    const std::vector<Spec>& specs, const EngineConfig& config,
    StreamForSpec&& stream_for_spec) {
  std::vector<PreparedSource> prepared;
  prepared.reserve(specs.size());
  for (const auto& spec : specs) {
    detail::CaptureOptions options;
    options.chunk_bytes = config.capture_chunk_bytes;
    options.maximum_bytes =
        std::min(config.maximum_feed_bytes, spec.maximum_bytes);
    options.maximum_redirects = config.maximum_redirects;
    options.deny_private_network = config.deny_private_network;
    auto source = detail::PreparedCapture::prepare(spec.uri, options);
    if (!source) return source.error();
    prepared.push_back(
        PreparedSource{stream_for_spec(spec), std::move(*source)});
  }
  return prepared;
}

bool all_producers_done(const SchedulerState& scheduler) {
  return std::all_of(scheduler.producers.begin(), scheduler.producers.end(),
                     [](const ProducerState& producer) {
                       return producer.done && producer.chunks.empty();
                     });
}

void cancel_scheduler(SchedulerState& scheduler, Error error) {
  {
    std::lock_guard lock(scheduler.mutex);
    if (!scheduler.first_error) scheduler.first_error = std::move(error);
    scheduler.cancelled = true;
  }
  scheduler.changed.notify_all();
}

template <typename AppendDescriptor>
Result<StreamRecordingReport> record_prepared_sources(
    std::vector<PreparedSource> prepared,
    const std::filesystem::path& archive_path, const EngineConfig& config,
    AppendDescriptor&& append_descriptor) {
  auto writer_result = CodaWriter::create(archive_path);
  if (!writer_result) return writer_result.error();
  auto writer = std::move(*writer_result);

  StreamRecordingReport report;
  report.archive = archive_path;

  for (std::size_t index = 0; index < prepared.size(); ++index) {
    auto descriptor = append_descriptor(writer, index, now_ns());
    if (!descriptor) return descriptor.error();
  }

  SchedulerState scheduler{prepared.size()};
  std::vector<std::thread> producers;
  producers.reserve(prepared.size());

  for (std::size_t index = 0; index < prepared.size(); ++index) {
    producers.emplace_back([&, index] {
      auto captured = prepared[index].capture.run(
          [&](std::span<const std::byte> bytes) -> Result<void> {
            PendingChunk chunk;
            try {
              chunk.bytes.assign(bytes.begin(), bytes.end());
            } catch (const std::bad_alloc&) {
              return fail(ErrorCode::resource_exhausted,
                          "capture queue allocation failed");
            }
            chunk.observed_ns = now_ns();
            const auto chunk_bytes =
                static_cast<std::uint64_t>(chunk.bytes.size());
            if (chunk_bytes > config.maximum_pending_bytes) {
              return fail(ErrorCode::resource_exhausted,
                          "capture chunk exceeds pending-byte bound");
            }

            std::unique_lock lock(scheduler.mutex);
            scheduler.changed.wait(lock, [&] {
              return scheduler.cancelled ||
                     (scheduler.producers[index].chunks.size() <
                          config.maximum_pending_chunks_per_stream &&
                      scheduler.pending_bytes <=
                          config.maximum_pending_bytes - chunk_bytes);
            });
            if (scheduler.cancelled) {
              return fail(ErrorCode::internal, "capture recording cancelled");
            }
            scheduler.pending_bytes += chunk_bytes;
            scheduler.producers[index].chunks.push_back(std::move(chunk));
            lock.unlock();
            scheduler.changed.notify_all();
            return {};
          });

      {
        std::lock_guard lock(scheduler.mutex);
        if (!captured && !scheduler.first_error && !scheduler.cancelled) {
          scheduler.first_error = captured.error();
          scheduler.cancelled = true;
        }
        scheduler.producers[index].done = true;
      }
      scheduler.changed.notify_all();
    });
  }

  auto join_producers = [&] {
    for (auto& producer : producers) {
      if (producer.joinable()) producer.join();
    }
  };

  std::size_t next_index = 0;
  for (;;) {
    PendingChunk chunk;
    std::size_t source_index = 0;
    bool have_chunk = false;
    std::optional<Error> producer_error;

    {
      std::unique_lock lock(scheduler.mutex);
      scheduler.changed.wait(lock, [&] {
        if (scheduler.first_error) return true;
        if (all_producers_done(scheduler)) return true;
        return std::any_of(scheduler.producers.begin(),
                           scheduler.producers.end(),
                           [](const ProducerState& producer) {
                             return !producer.chunks.empty();
                           });
      });

      if (scheduler.first_error) {
        producer_error = scheduler.first_error;
      } else {
        for (std::size_t offset = 0; offset < scheduler.producers.size();
             ++offset) {
          const auto candidate =
              (next_index + offset) % scheduler.producers.size();
          auto& state = scheduler.producers[candidate];
          if (state.chunks.empty()) continue;
          source_index = candidate;
          chunk = std::move(state.chunks.front());
          state.chunks.pop_front();
          scheduler.pending_bytes -=
              static_cast<std::uint64_t>(chunk.bytes.size());
          next_index = (candidate + 1) % scheduler.producers.size();
          have_chunk = true;
          break;
        }
        if (!have_chunk && all_producers_done(scheduler)) break;
      }
    }

    scheduler.changed.notify_all();

    if (producer_error) {
      join_producers();
      return *producer_error;
    }
    if (!have_chunk) continue;

    auto appended = writer.append(RecordType::source_bytes,
                                  prepared[source_index].stream,
                                  chunk.observed_ns, chunk.observed_ns,
                                  chunk.bytes);
    if (!appended) {
      cancel_scheduler(scheduler, appended.error());
      join_producers();
      return appended.error();
    }
    report.source_bytes += chunk.bytes.size();
    report.source_records += 1;
  }

  join_producers();
  report.streams_recorded = prepared.size();
  auto finalized = writer.finalize();
  if (!finalized) return finalized.error();
  return report;
}

}  // namespace

Result<Engine> Engine::create(EngineConfig config) {
  if (config.capture_chunk_bytes < 4096 ||
      config.capture_chunk_bytes > 16U * 1024U * 1024U ||
      config.maximum_feed_bytes == 0 || config.maximum_redirects > 20 ||
      config.maximum_pending_chunks_per_stream == 0 ||
      config.maximum_pending_bytes == 0) {
    return fail<Engine>(ErrorCode::invalid_argument,
                        "invalid engine capture limits");
  }
  return Engine{config};
}

Capabilities Engine::capabilities() noexcept { return {}; }

Result<StreamRecordingReport> Engine::record_streams(
    const std::vector<StreamSpec>& streams,
    const std::filesystem::path& archive_path) const {
  if (streams.empty()) {
    return fail<StreamRecordingReport>(ErrorCode::invalid_argument,
                                       "at least one stream is required");
  }
  std::set<StreamId> ids;
  for (const auto& stream : streams) {
    if (stream.uri.empty() || !stream.preserve_source ||
        stream.maximum_bytes == 0) {
      return fail<StreamRecordingReport>(
          ErrorCode::invalid_argument,
          "each stream requires a URI and S0 preservation");
    }
    if (!ids.insert(stream.descriptor.id).second) {
      return fail<StreamRecordingReport>(ErrorCode::invalid_argument,
                                         "stream IDs must be unique");
    }
    auto encoded = detail::encode_stream_descriptor(stream.descriptor);
    if (!encoded) return encoded.error();
  }
  auto prepared = prepare_sources(
      streams, config_,
      [](const StreamSpec& stream) { return stream.descriptor.id; });
  if (!prepared) return prepared.error();
  return record_prepared_sources(
      std::move(*prepared), archive_path, config_,
      [&streams](CodaWriter& writer, std::size_t index,
                 std::int64_t timestamp_ns) {
        return writer.append_stream_descriptor(streams[index].descriptor,
                                               timestamp_ns);
      });
}

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
  auto prepared = prepare_sources(
      feeds, config_, [](const FeedSpec& feed) {
        return derive_stream_id(feed.label + "\n" + feed.uri);
      });
  if (!prepared) return prepared.error();
  auto captured = record_prepared_sources(
      std::move(*prepared), archive_path, config_,
      [&feeds](CodaWriter& writer, std::size_t index,
               std::int64_t timestamp_ns) -> Result<RecordInfo> {
        const auto& spec = feeds[index];
        const auto stream = derive_stream_id(spec.label + "\n" + spec.uri);
        const FeedInfo info{stream, spec.label, redacted_uri(spec.uri), true};
        const auto descriptor = detail::encode_feed_descriptor(info);
        if (descriptor.empty()) {
          return fail<RecordInfo>(ErrorCode::invalid_argument,
                                  "feed descriptor is too large");
        }
        return writer.append(RecordType::feed_descriptor, stream, timestamp_ns,
                             timestamp_ns, descriptor);
      });
  if (!captured) return captured.error();
  return RecordingReport{
      .archive = std::move(captured->archive),
      .feeds_recorded = captured->streams_recorded,
      .source_bytes = captured->source_bytes,
      .source_records = captured->source_records,
  };
}

}  // namespace codec
