#include "test.hpp"

#include <codec/engine.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::filesystem::path live_path(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("codec-e3-" + std::to_string(::getpid()) + "-" +
          std::string{name});
}

bool write_all(int fd, std::string_view text) {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const auto count = ::write(fd, text.data() + offset, text.size() - offset);
    if (count < 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

}  // namespace

TEST(engine_live_recording_rejects_invalid_concurrency_bounds) {
  codec::EngineConfig config;
  config.maximum_concurrent_streams = 0;
  auto zero_streams = codec::Engine::create(config);
  EXPECT_FALSE(zero_streams);
  if (!zero_streams) {
    EXPECT_EQ(zero_streams.error().code, codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_concurrent_streams = 4097;
  auto too_many_streams = codec::Engine::create(config);
  EXPECT_FALSE(too_many_streams);
  if (!too_many_streams) {
    EXPECT_EQ(too_many_streams.error().code,
              codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_queued_chunks_per_stream = 0;
  auto zero_queue = codec::Engine::create(config);
  EXPECT_FALSE(zero_queue);
  if (!zero_queue) {
    EXPECT_EQ(zero_queue.error().code, codec::ErrorCode::invalid_argument);
  }

  config = {};
  config.maximum_queued_chunks_per_stream = 1025;
  auto too_large_queue = codec::Engine::create(config);
  EXPECT_FALSE(too_large_queue);
  if (!too_large_queue) {
    EXPECT_EQ(too_large_queue.error().code,
              codec::ErrorCode::invalid_argument);
  }
}

TEST(engine_live_recording_rejects_source_count_before_archive_creation) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto first_path = directory / "codec-e3-bound-first.bin";
  const auto second_path = directory / "codec-e3-bound-second.bin";
  const auto archive_path = directory / "codec-e3-bound.coda";
  {
    std::ofstream first(first_path, std::ios::binary | std::ios::trunc);
    first << "first";
    std::ofstream second(second_path, std::ios::binary | std::ios::trunc);
    second << "second";
  }
  std::filesystem::remove(archive_path);

  codec::EngineConfig config;
  config.maximum_concurrent_streams = 1;
  auto engine = codec::Engine::create(config);
  EXPECT_TRUE(engine);
  if (!engine) return;

  const std::vector<codec::FeedSpec> feeds{
      codec::FeedSpec{.uri = first_path.string(), .label = "first"},
      codec::FeedSpec{.uri = second_path.string(), .label = "second"},
  };
  auto result = engine->record(feeds, archive_path);
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::resource_exhausted);
  }
  EXPECT_FALSE(std::filesystem::exists(archive_path));

  std::filesystem::remove(first_path);
  std::filesystem::remove(second_path);
  std::filesystem::remove(archive_path);
}

TEST(engine_live_recording_commits_other_feed_while_first_remains_open) {
  using namespace std::chrono_literals;

  const auto first_fifo = live_path("first.fifo");
  const auto second_fifo = live_path("second.fifo");
  const auto archive_path = live_path("concurrent.coda");
  std::filesystem::remove(first_fifo);
  std::filesystem::remove(second_fifo);
  std::filesystem::remove(archive_path);
  EXPECT_EQ(::mkfifo(first_fifo.c_str(), 0600), 0);
  EXPECT_EQ(::mkfifo(second_fifo.c_str(), 0600), 0);

  std::atomic_bool release_first{false};
  std::atomic_bool producer_failed{false};
  std::thread first_producer([&] {
    const auto fd = ::open(first_fifo.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0 || !write_all(fd, "first-live-chunk")) {
      producer_failed.store(true, std::memory_order_relaxed);
      if (fd >= 0) ::close(fd);
      return;
    }
    while (!release_first.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(10ms);
    }
    ::close(fd);
  });
  std::thread second_producer([&] {
    const auto fd = ::open(second_fifo.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0 || !write_all(fd, "second-live-chunk")) {
      producer_failed.store(true, std::memory_order_relaxed);
      if (fd >= 0) ::close(fd);
      return;
    }
    ::close(fd);
  });

  codec::EngineConfig config;
  config.capture_chunk_bytes = 4096;
  config.maximum_queued_chunks_per_stream = 1;
  auto engine = codec::Engine::create(config);
  EXPECT_TRUE(engine);
  if (!engine) {
    release_first.store(true, std::memory_order_relaxed);
    first_producer.join();
    second_producer.join();
    return;
  }

  const auto second_stream =
      codec::derive_stream_id("second\n" + second_fifo.string());
  auto recording = std::async(std::launch::async, [&] {
    return engine->record(
        {codec::FeedSpec{.uri = first_fifo.string(), .label = "first"},
         codec::FeedSpec{.uri = second_fifo.string(), .label = "second"}},
        archive_path);
  });

  bool saw_second_before_first_closed = false;
  const auto deadline = std::chrono::steady_clock::now() + 1500ms;
  while (std::chrono::steady_clock::now() < deadline &&
         !saw_second_before_first_closed) {
    if (std::filesystem::exists(archive_path)) {
      auto archive = codec::CodaArchive::open(archive_path);
      if (archive) {
        auto records =
            archive->records(codec::ArchiveReadPolicy::verified_prefix);
        if (records) {
          saw_second_before_first_closed = std::any_of(
              records->begin(), records->end(), [&](const codec::RecordInfo& record) {
                return record.type == codec::RecordType::source_bytes &&
                       record.stream == second_stream;
              });
        }
      }
    }
    if (!saw_second_before_first_closed) std::this_thread::sleep_for(20ms);
  }
  EXPECT_TRUE(saw_second_before_first_closed);

  release_first.store(true, std::memory_order_relaxed);
  first_producer.join();
  second_producer.join();
  auto report = recording.get();
  EXPECT_TRUE(report);
  EXPECT_FALSE(producer_failed.load(std::memory_order_relaxed));
  if (report) {
    EXPECT_EQ(report->feeds_recorded, std::size_t{2});
  }

  auto archive = codec::CodaArchive::open(archive_path);
  EXPECT_TRUE(archive);
  if (archive) {
    auto records = archive->records();
    EXPECT_TRUE(records);
    if (records) {
      const auto first_source = std::find_if(
          records->begin(), records->end(), [](const codec::RecordInfo& record) {
            return record.type == codec::RecordType::source_bytes;
          });
      EXPECT_TRUE(first_source != records->end());
      if (first_source != records->end()) {
        const auto descriptors_before_source = std::count_if(
            records->begin(), first_source, [](const codec::RecordInfo& record) {
              return record.type == codec::RecordType::feed_descriptor;
            });
        EXPECT_EQ(descriptors_before_source, std::ptrdiff_t{2});
      }
    }
  }

  std::filesystem::remove(first_fifo);
  std::filesystem::remove(second_fifo);
  std::filesystem::remove(archive_path);
}
