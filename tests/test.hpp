#pragma once

#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test {

using Function = void (*)();

struct Case {
  std::string name;
  Function function;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> value;
  return value;
}

struct Register {
  Register(std::string name, Function function) {
    cases().push_back({std::move(name), function});
  }
};

template <typename Actual, typename Expected>
void expect_equal(const Actual& actual, const Expected& expected,
                  const char* actual_text, const char* expected_text,
                  const char* file, int line) {
  if (!(actual == expected)) {
    std::ostringstream out;
    out << file << ':' << line << ": expected " << actual_text << " == "
        << expected_text;
    throw std::runtime_error(out.str());
  }
}

inline void expect_true(bool value, const char* text, const char* file, int line) {
  if (!value) {
    std::ostringstream out;
    out << file << ':' << line << ": expected true: " << text;
    throw std::runtime_error(out.str());
  }
}

}  // namespace test

#define TEST(name)                                                           \
  static void name();                                                        \
  static ::test::Register name##_registration{#name, &name};                 \
  static void name()

#define EXPECT_EQ(actual, expected)                                          \
  ::test::expect_equal((actual), (expected), #actual, #expected, __FILE__,   \
                       __LINE__)

#define EXPECT_TRUE(value)                                                   \
  ::test::expect_true(static_cast<bool>(value), #value, __FILE__, __LINE__)

#define EXPECT_FALSE(value) EXPECT_TRUE(!(value))

