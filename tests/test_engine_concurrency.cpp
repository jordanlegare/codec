#include "test.hpp"

#include <codec/archive.hpp>
#include <codec/engine.hpp>
#include <codec/stream.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fcntl.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

bool wait_until(const auto& predicate,
                std::chrono::milliseconds timeout = 2000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return predicate();
}

void write_fifo_once(const std::filesystem::path& path, std::string payload,
                     std::atomic<bool>& wrote,
                     const std::atomic<bool>& release) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return;
  const char* cursor = payload.data();
  std::size_t remaining = payload.size();
  while (remaining != 0) {
    const auto count = ::write(fd, cursor, remaining);
    if (count <= 0) {
      ::close(fd);
      return;
    }
    cursor += count;
    remaining -= static_cast<std::size_t>(count);
  }
  wrote.store(true, std::memory_order_release);
  while (!release.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(5ms);
  }
  ::close(fd);
}

}  // namespace

TEST(engine_records_ready_feeds_concurrently_before_first_feed_eof) {
  const auto directory = std::filesystem::temp_directory_path();
  const auto fifo_a = directory / "codec-e3-feed-a.fifo";
  const auto fifo_b = directory / "codec-e3-feed-b.fifo";
  const auto archive_path = directory / "codec-e3-concurrent.coda";
  std::filesystem::remove(fifo_a);
  std::filesystem::remove(fifo_b);
  std::filesystem::remove(archive_path);
  EXPECT_EQ(::mkfifo(fifo_a.c_str(), 0600), 0);
  EXPECT_EQ(::mkfifo(fifo_b.c_str(), 0600), 0);

  std::atomic<bool> release{false};
  std::atomic<bool> wrote_a{false};
  std::atomic<bool> wrote_b{false};
  std::thread feeder_a(write_fifo_once, fifo_a, std::string{"A-first"},
                       std::ref(wrote_a), std::cref(release));
  std::thread feeder_b(write_fifo_once, fifo_b, std::string{"B-first"},
                       std::ref(wrote_b), std::cref(release));

  auto engine = codec::Engine::create({});
  EXPECT_TRUE(engine);
  std::optional<codec::Result<codec::RecordingReport>> recording;
  std::thread recorder([&] {
    recording.emplace(engine->record(
        {codec::FeedSpec{.uri = fifo_a.string(), .label = "A"},
         codec::FeedSpec{.uri = fifo_b.string(), .label = "B"}},
        archive_path));
  });

  EXPECT_TRUE(wait_until([&] {
    return wrote_a.load(std::memory_order_acquire) &&
           wrote_b.load(std::memory_order_acquire);
  }));

  const auto stream_b = codec::derive_stream_id("B\n" + fifo_b.string());
  const bool saw_b_before_a_eof = wait_until(
      [&] {
        auto archive = codec::CodaArchive::open(archive_path);
        if (!archive) return false;
        auto records = archive->records(codec::ArchiveReadPolicy::verified_prefix);
        if (!records) return false;
        for (const auto& record : *records) {
          if (record.type == codec::RecordType::source_bytes &&
              record.stream == stream_b) {
            return true;
          }
        }
        return false;
      },
      500ms);

  release.store(true, std::memory_order_release);
  feeder_a.join();
  feeder_b.join();
  recorder.join();

  EXPECT_TRUE(recording.has_value());
  if (recording) EXPECT_TRUE(*recording);
  EXPECT_TRUE(saw_b_before_a_eof);

  if (recording && *recording) {
    EXPECT_EQ((*recording)->feeds_recorded, std::size_t{2});
    auto archive = codec::CodaArchive::open(archive_path);
    EXPECT_TRUE(archive);
    if (archive) {
      auto records = archive->records();
      EXPECT_TRUE(records);
      if (records) {
        std::size_t first_source = records->size();
        std::size_t descriptor_count_before_source = 0;
        for (std::size_t index = 0; index < records->size(); ++index) {
          if ((*records)[index].type == codec::RecordType::source_bytes) {
            first_source = index;
            break;
          }
        }
        for (std::size_t index = 0; index < first_source; ++index) {
          if ((*records)[index].type == codec::RecordType::feed_descriptor) {
            ++descriptor_count_before_source;
          }
        }
        EXPECT_EQ(descriptor_count_before_source, std::size_t{2});
      }
    }
  }

  std::filesystem::remove(fifo_a);
  std::filesystem::remove(fifo_b);
  std::filesystem::remove(archive_path);
}
