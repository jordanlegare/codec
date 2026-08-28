#include "test.hpp"

#include <string>
#include <string_view>

namespace {

struct Filters {
  std::string include_prefix;
  std::string exclude_prefix;
};

bool parse_filters(int argc, char** argv, Filters* filters) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    if ((argument == "--include-prefix" || argument == "--exclude-prefix") &&
        index + 1 < argc) {
      auto& destination = argument == "--include-prefix"
                              ? filters->include_prefix
                              : filters->exclude_prefix;
      if (!destination.empty()) return false;
      destination = argv[++index];
      if (destination.empty()) return false;
      continue;
    }
    return false;
  }
  return true;
}

bool selected(const test::Case& item, const Filters& filters) {
  if (!filters.include_prefix.empty() &&
      !item.name.starts_with(filters.include_prefix)) {
    return false;
  }
  if (!filters.exclude_prefix.empty() &&
      item.name.starts_with(filters.exclude_prefix)) {
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Filters filters;
  if (!parse_filters(argc, argv, &filters)) {
    std::cerr << "usage: codec_tests [--include-prefix PREFIX] "
                 "[--exclude-prefix PREFIX]\n";
    return 2;
  }

  std::size_t failures = 0;
  std::size_t selected_count = 0;
  for (const auto& item : test::cases()) {
    if (!selected(item, filters)) continue;
    ++selected_count;
    try {
      item.function();
      std::cout << "PASS " << item.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << item.name << ": " << error.what() << '\n';
    }
  }

  if (selected_count == 0U) {
    std::cerr << "no tests selected\n";
    return 2;
  }

  std::cout << selected_count << " tests, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
