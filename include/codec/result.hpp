#pragma once

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace codec {

enum class ErrorCode {
  ok = 0,
  invalid_argument = 1,
  unauthorized_source = 2,
  network = 3,
  protocol = 4,
  decode = 5,
  archive_io = 6,
  archive_corrupt = 7,
  model_incompatible = 8,
  inference = 9,
  identity_not_enrolled = 15,
  identity_uncalibrated = 16,
  cancelled = 17,
  resource_exhausted = 18,
  internal = 19,
};

struct Error {
  ErrorCode code{ErrorCode::internal};
  std::string message;
  bool retryable{false};
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : value_(std::move(error)) {}

  explicit operator bool() const noexcept {
    return std::holds_alternative<T>(value_);
  }

  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  Error& error() & { return std::get<Error>(value_); }
  const Error& error() const& { return std::get<Error>(value_); }

  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T&& operator*() && { return std::move(*this).value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

 private:
  std::variant<T, Error> value_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}

  explicit operator bool() const noexcept { return !error_.has_value(); }
  Error& error() { return *error_; }
  const Error& error() const { return *error_; }

 private:
  std::optional<Error> error_;
};

template <typename T>
Result<T> fail(ErrorCode code, std::string message, bool retryable = false) {
  return Error{code, std::move(message), retryable};
}

inline Result<void> fail(ErrorCode code, std::string message,
                         bool retryable = false) {
  return Error{code, std::move(message), retryable};
}

const char* error_code_name(ErrorCode code) noexcept;

}  // namespace codec
