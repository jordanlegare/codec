#include <codec/integrity.hpp>

#include "internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace codec {

Sha256 sha256(std::span<const std::byte> data) {
  constexpr std::array<std::uint32_t, 64> constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
  std::array<std::uint32_t, 8> state{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  const auto rotate_right = [](std::uint32_t value, unsigned shift) {
    return (value >> shift) | (value << (32U - shift));
  };
  const auto process_block = [&](const std::byte* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto offset = index * 4;
      words[index] =
          (static_cast<std::uint32_t>(block[offset]) << 24U) |
          (static_cast<std::uint32_t>(block[offset + 1]) << 16U) |
          (static_cast<std::uint32_t>(block[offset + 2]) << 8U) |
          static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = rotate_right(words[index - 15], 7) ^
                      rotate_right(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
      const auto s1 = rotate_right(words[index - 2], 17) ^
                      rotate_right(words[index - 2], 19) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }
    auto a = state[0];
    auto b = state[1];
    auto c = state[2];
    auto d = state[3];
    auto e = state[4];
    auto f = state[5];
    auto g = state[6];
    auto h = state[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto first = h + sum1 + choose + constants[index] + words[index];
      const auto sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto second = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + first;
      d = c;
      c = b;
      b = a;
      a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  };

  std::size_t offset = 0;
  while (data.size() - offset >= 64) {
    process_block(data.data() + offset);
    offset += 64;
  }
  std::array<std::byte, 128> tail{};
  const auto remaining = data.size() - offset;
  std::copy_n(data.data() + offset, remaining, tail.data());
  tail[remaining] = std::byte{0x80};
  const auto padded_bytes = remaining < 56 ? std::size_t{64}
                                            : std::size_t{128};
  const auto bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
  for (std::size_t index = 0; index < 8; ++index) {
    tail[padded_bytes - 1 - index] =
        static_cast<std::byte>(bit_length >> (index * 8U));
  }
  process_block(tail.data());
  if (padded_bytes == 128) process_block(tail.data() + 64);

  Sha256 output{};
  for (std::size_t index = 0; index < state.size(); ++index) {
    output[index * 4] = static_cast<std::uint8_t>(state[index] >> 24U);
    output[index * 4 + 1] =
        static_cast<std::uint8_t>(state[index] >> 16U);
    output[index * 4 + 2] =
        static_cast<std::uint8_t>(state[index] >> 8U);
    output[index * 4 + 3] = static_cast<std::uint8_t>(state[index]);
  }
  return output;
}

std::string sha256_hex(std::span<const std::byte> data) {
  const auto digest = sha256(data);
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (const auto value : digest) {
    text << std::setw(2) << static_cast<unsigned int>(value);
  }
  return text.str();
}

const char* error_code_name(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::ok: return "ok";
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::unauthorized_source: return "unauthorized_source";
    case ErrorCode::network: return "network";
    case ErrorCode::protocol: return "protocol";
    case ErrorCode::decode: return "decode";
    case ErrorCode::archive_io: return "archive_io";
    case ErrorCode::archive_corrupt: return "archive_corrupt";
    case ErrorCode::model_incompatible: return "model_incompatible";
    case ErrorCode::inference: return "inference";
    case ErrorCode::identity_not_enrolled: return "identity_not_enrolled";
    case ErrorCode::identity_uncalibrated: return "identity_uncalibrated";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::resource_exhausted: return "resource_exhausted";
    case ErrorCode::internal: return "internal";
  }
  return "internal";
}

}  // namespace codec

namespace codec::detail {

std::uint32_t crc32c(std::span<const std::byte> data) noexcept {
  std::uint32_t value = 0xffffffffU;
  for (const auto byte : data) {
    value ^= static_cast<std::uint8_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      const auto mask = static_cast<std::uint32_t>(
          -static_cast<std::int32_t>(value & 1U));
      value = (value >> 1U) ^ (0x82f63b78U & mask);
    }
  }
  return ~value;
}

Result<std::vector<std::byte>> read_file(const std::filesystem::path& path,
                                        std::uint64_t maximum_bytes) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_io,
                                       "cannot open file for reading: " +
                                           path.string());
  }
  const auto end = input.tellg();
  if (end < 0) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_io,
                                       "cannot determine file size");
  }
  const auto size = static_cast<std::uint64_t>(end);
  if (size > maximum_bytes) {
    return fail<std::vector<std::byte>>(ErrorCode::resource_exhausted,
                                       "file exceeds configured byte limit");
  }
  std::vector<std::byte> data(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!data.empty()) {
    input.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
  }
  if (!input && !data.empty()) {
    return fail<std::vector<std::byte>>(ErrorCode::archive_io,
                                       "short read from file");
  }
  return data;
}

namespace {

Result<void> write_all(int descriptor, std::span<const std::byte> data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto written = ::write(descriptor, data.data() + offset,
                                 data.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      return fail(ErrorCode::archive_io,
                  "secure output write failed: " +
                      std::string{std::strerror(errno)});
    }
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

Result<void> atomic_create(const std::filesystem::path& path,
                           std::span<const std::byte> data, mode_t mode) {
  if (path.empty() || path.filename().empty()) {
    return fail(ErrorCode::invalid_argument, "output path must name a file");
  }
  const auto parent = path.has_parent_path() ? path.parent_path()
                                              : std::filesystem::path{"."};
  static std::atomic<std::uint64_t> counter{};
  std::filesystem::path temporary;
  int descriptor = -1;
  for (int attempt = 0; attempt < 32 && descriptor < 0; ++attempt) {
    temporary = parent /
                ("." + path.filename().string() + ".tmp." +
                 std::to_string(static_cast<unsigned long long>(::getpid())) +
                 "." + std::to_string(counter.fetch_add(1)));
    descriptor = ::open(temporary.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                        mode);
    if (descriptor < 0 && errno != EEXIST) break;
  }
  if (descriptor < 0) {
    return fail(ErrorCode::archive_io,
                "cannot create secure temporary output: " +
                    std::string{std::strerror(errno)});
  }
  auto cleanup = [&] {
    if (descriptor >= 0) ::close(descriptor);
    ::unlink(temporary.c_str());
  };
  if (::fchmod(descriptor, mode) != 0) {
    const auto message = std::string{std::strerror(errno)};
    cleanup();
    return fail(ErrorCode::archive_io,
                "cannot set secure output permissions: " + message);
  }
  auto written = write_all(descriptor, data);
  if (!written) {
    cleanup();
    return written.error();
  }
  const auto sync_status = ::fsync(descriptor);
  const auto close_status = ::close(descriptor);
  if (sync_status != 0 || close_status != 0) {
    const auto message = std::string{std::strerror(errno)};
    descriptor = -1;
    ::unlink(temporary.c_str());
    return fail(ErrorCode::archive_io,
                "cannot commit secure output: " + message);
  }
  descriptor = -1;
  if (::link(temporary.c_str(), path.c_str()) != 0) {
    const auto message = errno == EEXIST
                             ? "output already exists; refusing to replace it"
                             : std::string{"cannot publish secure output: "} +
                                   std::strerror(errno);
    ::unlink(temporary.c_str());
    return fail(ErrorCode::archive_io, message);
  }
  ::unlink(temporary.c_str());
  const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory >= 0) {
    ::fsync(directory);
    ::close(directory);
  }
  return {};
}

}  // namespace

Result<void> write_file(const std::filesystem::path& path,
                        std::span<const std::byte> data) {
  return atomic_create(path, data, 0644);
}

Result<void> write_private_file(const std::filesystem::path& path,
                                std::span<const std::byte> data) {
  return atomic_create(path, data, 0600);
}

std::string json_escape(std::string_view input) {
  std::string output;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': output += "\\\\"; break;
      case '"': output += "\\\""; break;
      case '\n': output += "\\n"; break;
      case '\r': output += "\\r"; break;
      case '\t': output += "\\t"; break;
      default:
        if (ch < 0x20U) {
          const char hex[] = "0123456789abcdef";
          output += "\\u00";
          output += hex[ch >> 4U];
          output += hex[ch & 0x0fU];
        } else {
          output += static_cast<char>(ch);
        }
    }
  }
  return output;
}

}  // namespace codec::detail
