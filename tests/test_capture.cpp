#include "test.hpp"

#include "../src/capture/capture.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

std::filesystem::path unique_fifo_path() {
  static std::atomic_uint64_t counter{0};
  return std::filesystem::temp_directory_path() /
         ("codec-e3-cancel-" + std::to_string(::getpid()) + "-" +
          std::to_string(counter.fetch_add(1)) + ".fifo");
}

}  // namespace

TEST(prepared_capture_cancellation_interrupts_idle_fifo) {
  using namespace std::chrono_literals;

  const auto fifo = unique_fifo_path();
  std::filesystem::remove(fifo);
  EXPECT_EQ(::mkfifo(fifo.c_str(), 0600), 0);

  std::atomic_bool close_writer{false};
  std::promise<void> writer_opened;
  auto writer = std::thread([&] {
    const auto fd = ::open(fifo.c_str(), O_WRONLY | O_CLOEXEC);
    writer_opened.set_value();
    if (fd < 0) return;
    while (!close_writer.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(10ms);
    }
    ::close(fd);
  });

  codec::detail::CaptureOptions options;
  options.chunk_bytes = 4096;
  options.maximum_bytes = 4096;
  auto prepared = codec::detail::PreparedCapture::prepare(fifo.string(), options);
  EXPECT_TRUE(prepared);
  writer_opened.get_future().wait();
  if (!prepared) {
    close_writer.store(true, std::memory_order_relaxed);
    writer.join();
    std::filesystem::remove(fifo);
    return;
  }

  std::atomic_bool cancelled{false};
  auto running = std::async(std::launch::async, [&] {
    return prepared->run(
        [](std::span<const std::byte>) -> codec::Result<void> { return {}; },
        &cancelled);
  });

  std::this_thread::sleep_for(100ms);
  cancelled.store(true, std::memory_order_relaxed);
  const auto status = running.wait_for(750ms);
  EXPECT_EQ(status, std::future_status::ready);

  close_writer.store(true, std::memory_order_relaxed);
  writer.join();
  auto result = running.get();
  EXPECT_FALSE(result);
  if (!result) {
    EXPECT_EQ(result.error().code, codec::ErrorCode::cancelled);
  }

  std::filesystem::remove(fifo);
}
