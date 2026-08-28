#pragma once

#include <codec/result.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>

namespace codec::detail {

struct CaptureOptions {
  std::size_t chunk_bytes{};
  std::uint64_t maximum_bytes{};
  std::uint32_t maximum_redirects{};
  bool deny_private_network{};
};

struct CaptureReport {
  std::uint64_t bytes{};
  std::uint64_t chunks{};
};

using ByteSink =
    std::function<Result<void>(std::span<const std::byte> bytes)>;

class PreparedCapture {
 public:
  static Result<PreparedCapture> prepare(std::string uri,
                                         CaptureOptions options);

  PreparedCapture(const PreparedCapture&) = delete;
  PreparedCapture& operator=(const PreparedCapture&) = delete;
  PreparedCapture(PreparedCapture&& other) noexcept;
  PreparedCapture& operator=(PreparedCapture&& other) noexcept;
  ~PreparedCapture();

  Result<CaptureReport> run(const ByteSink& sink,
                            const std::atomic_bool* cancelled = nullptr);

 private:
  PreparedCapture(std::string uri, CaptureOptions options, int descriptor,
                  bool http) noexcept
      : uri_(std::move(uri)),
        options_(options),
        descriptor_(descriptor),
        http_(http) {}

  std::string uri_;
  CaptureOptions options_;
  int descriptor_{-1};
  bool http_{false};
};

}  // namespace codec::detail
