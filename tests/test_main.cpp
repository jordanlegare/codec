#include "test.hpp"

int main() {
  std::size_t failures = 0;
  for (const auto& item : test::cases()) {
    try {
      item.function();
      std::cout << "PASS " << item.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << item.name << ": " << error.what() << '\n';
    }
  }
  std::cout << test::cases().size() << " tests, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}

