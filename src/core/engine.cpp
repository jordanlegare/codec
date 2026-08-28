#include <codec/engine.hpp>

#include "../capture/capture.hpp"
#include "internal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
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

struct QueuedSource {
  std::deque<std::vector<std::byte>> chunks;
  bool finished{false};
};

struct ConcurrentCaptureState {
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<QueuedSource> sources;
  std::optional<Error> first_error;
  std::atomic_bool cancelled{false};
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

  // Descriptors establish every logical identity before any capture worker can
  // contribute source bytes. This keeps a growing archive self-describing.
  for (std::size_t index = 0; index < prepared.size(); ++index) {
    auto descriptor = append_descriptor(writer, index, now_ns());
    if (!descriptor) return descriptor.error();
  }

  ConcurrentCaptureState state;
  try {
    state.sources.resize(prepared.size());
  } catch (const std::bad_alloc&) {
    return fail<StreamRecordingReport>(
        ErrorCode::resource_exhausted,
        "cannot allocate concurrent capture queue state");
  }

  const auto set_worker_error = [&](std::size_t index,
                                    std::optional<Error> error) {
    {
      std::lock_guard lock(state.mutex);
      if (error && error->code != ErrorCode::cancelled &&
          !state.first_error) {
        state.first_error = std::move(*error);
        state.cancelled.store(true, std::memory_order_relaxed);
      }
      state.sources[index].finished = true;
    }
    state.changed.notify_all();
  };

  std::vector<std::thread> workers;
  try {
    workers.reserve(prepared.size());
    for (std::size_t index = 0; index < prepared.size(); ++index) {
      workers.emplace_back([&, index] {
        try {
          auto captured = prepared[index].capture.run(
              [&](std::span<const std::byte> bytes) -> Result<void> {
                std::vector<std::byte> owned(bytes.begin(), bytes.end());
                std::unique_lock lock(state.mutex);
                state.changed.wait(lock, [&] {
                  return state.cancelled.load(std::memory_order_relaxed) ||
                         state.sources[index].chunks.size() <
                             config.maximum_queued_chunks_per_stream;
                });
                if (state.cancelled.load(std::memory_order_relaxed)) {
                  return fail(ErrorCode::cancelled,
                              "concurrent capture cancelled");
                }
                state.sources[index].chunks.push_back(std::move(owned));
                lock.unlock();
                state.changed.notify_all();
                return {};
              },
              &state.cancelled);
          if (!captured) {
            set_worker_error(index, captured.error());
          } else {
            set_worker_error(index, std::nullopt);
          }
        } catch (const std::bad_alloc&) {
          set_worker_error(
              index,
              Error{ErrorCode::resource_exhausted,
                    "concurrent capture worker exhausted memory", false});
        } catch (...) {
          set_worker_error(
              index,
              Error{ErrorCode::internal,
                    "concurrent capture worker failed unexpectedly", false});
        }
      });
    }
  } catch (const std::bad_alloc&) {
    state.cancelled.store(true, std::memory_order_relaxed);
    state.changed.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
    return fail<StreamRecordingReport>(
        ErrorCode::resource_exhausted,
        "cannot allocate concurrent capture workers");
  } catch (...) {
    state.cancelled.store(true, std::memory_order_relaxed);
    state.changed.notify_all();
    for (auto& worker : workers) {
      if (worker.joinable()) worker.join();
    }
    return fail<StreamRecordingReport>(ErrorCode::internal,
                                       "cannot start capture workers");
  }

  std::optional<Error> writer_error;
  std::size_t next_source = 0;
  for (;;) {
    std::vector<std::byte> chunk;
    StreamId stream{};
    bool have_chunk = false;
    bool all_finished = false;

    {
      std::unique_lock lock(state.mutex);
      const auto any_chunk = [&] {
        return std::any_of(state.sources.begin(), state.sources.end(),
                           [](const QueuedSource& source) {
                             return !source.chunks.empty();
                           });
      };
      const auto every_worker_finished = [&] {
        return std::all_of(state.sources.begin(), state.sources.end(),
                           [](const QueuedSource& source) {
                             return source.finished;
                           });
      };
      state.changed.wait(lock, [&] {
        return any_chunk() || every_worker_finished();
      });

      all_finished = every_worker_finished();
      for (std::size_t offset = 0; offset < state.sources.size(); ++offset) {
        const auto index = (next_source + offset) % state.sources.size();
        if (state.sources[index].chunks.empty()) continue;
        chunk = std::move(state.sources[index].chunks.front());
        state.sources[index].chunks.pop_front();
        stream = prepared[index].stream;
        next_source = (index + 1) % state.sources.size();
        have_chunk = true;
        break;
      }
    }

    if (have_chunk) {
      state.changed.notify_all();
      const auto observed = now_ns();
      auto appended = writer.append(RecordType::source_bytes, stream, observed,
                                    observed, chunk);
      if (!appended) {
        writer_error = appended.error();
        state.cancelled.store(true, std::memory_order_relaxed);
        state.changed.notify_all();
        break;
      }
      report.source_bytes += chunk.size();
      report.source_records += 1;
      continue;
    }

    if (all_finished) break;
  }

  for (auto& worker : workers) {
    if (worker.joinable()) worker.join();
  }

  if (writer_error) return *writer_error;
  if (state.first_error) return *state.first_error;

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
      config.maximum_concurrent_streams == 0 ||
      config.maximum_concurrent_streams > 4096 ||
      config.maximum_queued_chunks_per_stream == 0 ||
      config.maximum_queued_chunks_per_stream > 1024) {
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
  if (streams.size() > config_.maximum_concurrent_streams) {
    return fail<StreamRecordingReport>(
        ErrorCode::resource_exhausted,
        "stream count exceeds the concurrent recording limit");
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
  if (feeds.size() > config_.maximum_concurrent_streams) {
    return fail<RecordingReport>(
        ErrorCode::resource_exhausted,
        "feed count exceeds the concurrent recording limit");
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
