#include <codec/engine.hpp>

#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: codec_capture_example INPUT ARCHIVE.coda\n";
    return 2;
  }
  auto engine = codec::Engine::create({});
  if (!engine) {
    std::cerr << engine.error().message << '\n';
    return 1;
  }
  auto report = engine->record(
      {codec::FeedSpec{.uri = argv[1], .label = "example-feed"}}, argv[2]);
  if (!report) {
    std::cerr << report.error().message << '\n';
    return 1;
  }
  std::cout << "preserved " << report->source_bytes << " source bytes in "
            << report->archive << '\n';
  return 0;
}
