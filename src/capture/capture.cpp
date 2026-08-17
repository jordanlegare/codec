#include "capture.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace codec::detail {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

bool private_http_host(const std::string& uri) {
  const auto scheme = uri.find("://");
  if (scheme == std::string::npos) return false;
  auto host_start = scheme + 3;
  const auto authority_end = uri.find_first_of("/?#", host_start);
  auto authority = uri.substr(host_start, authority_end - host_start);
  const auto user_info = authority.rfind('@');
  if (user_info != std::string::npos) authority.erase(0, user_info + 1);
  std::string host;
  if (!authority.empty() && authority.front() == '[') {
    const auto close = authority.find(']');
    host = close == std::string::npos ? authority : authority.substr(0, close + 1);
  } else {
    host = authority.substr(0, authority.find(':'));
  }
  host = lower(host);
  if (host == "localhost" || host.ends_with(".localhost") ||
      host == "0.0.0.0" || host == "[::]" || host == "[::1]" ||
      starts_with(host, "127.") || starts_with(host, "10.") ||
      starts_with(host, "192.168.") || starts_with(host, "169.254.")) {
    return true;
  }
  if (starts_with(host, "172.")) {
    const auto second_end = host.find('.', 4);
    if (second_end != std::string::npos) {
      try {
        const auto second = std::stoi(host.substr(4, second_end - 4));
        if (second >= 16 && second <= 31) return true;
      } catch (...) {
        return true;
      }
    }
  }
  return false;
}

Result<CaptureReport> capture_descriptor(int descriptor,
                                         const CaptureOptions& options,
                                         const ByteSink& sink) {
  std::vector<std::byte> buffer(options.chunk_bytes);
  CaptureReport report;
  for (;;) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      return fail<CaptureReport>(
          ErrorCode::archive_io,
          "capture source read failed: " + std::string{std::strerror(errno)});
    }
    if (count == 0) break;
    const auto bytes = static_cast<std::uint64_t>(count);
    if (bytes > options.maximum_bytes - report.bytes) {
      return fail<CaptureReport>(ErrorCode::resource_exhausted,
                                 "capture exceeded the feed byte limit");
    }
    auto accepted = sink(std::span{buffer}.first(static_cast<std::size_t>(count)));
    if (!accepted) return accepted.error();
    report.bytes += bytes;
    report.chunks += 1;
  }
  return report;
}

struct CurlSink {
  const CaptureOptions* options{};
  const ByteSink* sink{};
  CaptureReport report;
  Error error;
  bool failed{false};
};

bool globally_routable_ipv4(const in_addr& address) {
  const auto value = ntohl(address.s_addr);
  const auto in_prefix = [value](std::uint32_t network,
                                 std::uint32_t mask) {
    return (value & mask) == network;
  };
  return !(in_prefix(0x00000000U, 0xff000000U) ||
           in_prefix(0x0a000000U, 0xff000000U) ||
           in_prefix(0x64400000U, 0xffc00000U) ||
           in_prefix(0x7f000000U, 0xff000000U) ||
           in_prefix(0xa9fe0000U, 0xffff0000U) ||
           in_prefix(0xac100000U, 0xfff00000U) ||
           in_prefix(0xc0000000U, 0xffffff00U) ||
           in_prefix(0xc0000200U, 0xffffff00U) ||
           in_prefix(0xc0a80000U, 0xffff0000U) ||
           in_prefix(0xc6120000U, 0xfffe0000U) ||
           in_prefix(0xc6336400U, 0xffffff00U) ||
           in_prefix(0xcb007100U, 0xffffff00U) ||
           in_prefix(0xe0000000U, 0xf0000000U) ||
           in_prefix(0xf0000000U, 0xf0000000U));
}

bool globally_routable(const curl_sockaddr& address) {
  if (address.family == AF_INET &&
      address.addrlen >= sizeof(sockaddr_in)) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address.addr);
    return globally_routable_ipv4(ipv4->sin_addr);
  }
  if (address.family == AF_INET6 &&
      address.addrlen >= sizeof(sockaddr_in6)) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address.addr);
    const auto& bytes = ipv6->sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&bytes)) {
      in_addr mapped{};
      std::memcpy(&mapped, &bytes.s6_addr[12], sizeof(mapped));
      return globally_routable_ipv4(mapped);
    }
    const bool global_unicast = (bytes.s6_addr[0] & 0xe0U) == 0x20U;
    const bool documentation =
        bytes.s6_addr[0] == 0x20U && bytes.s6_addr[1] == 0x01U &&
        bytes.s6_addr[2] == 0x0dU && bytes.s6_addr[3] == 0xb8U;
    return global_unicast && !documentation;
  }
  return false;
}

curl_socket_t curl_open_socket(void* user_data, curlsocktype purpose,
                               curl_sockaddr* address) {
  auto* context = static_cast<CurlSink*>(user_data);
  if (purpose != CURLSOCKTYPE_IPCXN || address == nullptr ||
      (context->options->deny_private_network &&
       !globally_routable(*address))) {
    context->error = {ErrorCode::unauthorized_source,
                      "resolved HTTP target is not globally routable", false};
    context->failed = true;
    return CURL_SOCKET_BAD;
  }
  return ::socket(address->family, address->socktype, address->protocol);
}

std::size_t curl_write(char* data, std::size_t size, std::size_t count,
                       void* user_data) {
  auto* context = static_cast<CurlSink*>(user_data);
  if (context->failed) return 0;
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    context->error = {ErrorCode::resource_exhausted,
                      "HTTP capture chunk size overflow", false};
    context->failed = true;
    return 0;
  }
  const auto total = size * count;
  if (total > context->options->maximum_bytes - context->report.bytes) {
    context->error = {ErrorCode::resource_exhausted,
                      "HTTP capture exceeded the feed byte limit", false};
    context->failed = true;
    return 0;
  }
  const auto bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(data), total};
  auto accepted = (*context->sink)(bytes);
  if (!accepted) {
    context->error = accepted.error();
    context->failed = true;
    return 0;
  }
  context->report.bytes += total;
  context->report.chunks += 1;
  return total;
}

void initialize_curl() {
  static std::once_flag flag;
  std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

Result<CaptureReport> capture_http(const std::string& uri,
                                   const CaptureOptions& options,
                                   const ByteSink& sink) {
  if (options.deny_private_network && private_http_host(uri)) {
    return fail<CaptureReport>(ErrorCode::unauthorized_source,
                               "private-network HTTP targets are denied");
  }
  initialize_curl();
  CURL* handle = curl_easy_init();
  if (handle == nullptr) {
    return fail<CaptureReport>(ErrorCode::network,
                               "cannot initialize HTTP capture", true);
  }
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  CurlSink context{&options, &sink, {}, {}, false};
  curl_easy_setopt(handle, CURLOPT_URL, uri.c_str());
  curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  // Redirects are deliberately refused in the MVP. Following them safely
  // requires applying authorization and DNS policy independently to every hop.
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(handle, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "CODEC/0.1");
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &curl_write);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, &curl_open_socket);
  curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, &context);
  if (options.deny_private_network) {
    // Environment proxy variables can otherwise route a denied target through
    // a different socket than the one inspected by curl_open_socket.
    curl_easy_setopt(handle, CURLOPT_PROXY, "");
    curl_easy_setopt(handle, CURLOPT_NOPROXY, "*");
  }
  const auto code = curl_easy_perform(handle);
  long response = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response);
  curl_easy_cleanup(handle);
  if (context.failed) return context.error;
  if (code != CURLE_OK) {
    const auto detail = error_buffer.front() == '\0'
                            ? std::string{curl_easy_strerror(code)}
                            : std::string{error_buffer.data()};
    return fail<CaptureReport>(ErrorCode::network,
                               "HTTP capture failed: " + detail, true);
  }
  if (response < 200 || response >= 300) {
    return fail<CaptureReport>(ErrorCode::protocol,
                               "HTTP response was not successful");
  }
  return context.report;
}

}  // namespace

Result<PreparedCapture> PreparedCapture::prepare(std::string uri,
                                                 CaptureOptions options) {
  if (uri.empty() || options.chunk_bytes == 0 || options.maximum_bytes == 0 ||
      options.chunk_bytes > static_cast<std::size_t>(SSIZE_MAX)) {
    return fail<PreparedCapture>(ErrorCode::invalid_argument,
                                 "invalid capture URI or limits");
  }
  if (starts_with(uri, "http://") || starts_with(uri, "https://")) {
    if (options.deny_private_network && private_http_host(uri)) {
      return fail<PreparedCapture>(ErrorCode::unauthorized_source,
                                   "private-network HTTP targets are denied");
    }
    return PreparedCapture{std::move(uri), options, -1, true};
  }
  int descriptor = -1;
  if (uri == "-") {
    descriptor = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
  } else {
    const auto path = starts_with(uri, "file://") ? uri.substr(7) : uri;
    if (path.find("://") != std::string::npos) {
      return fail<PreparedCapture>(ErrorCode::protocol,
                                   "unsupported feed URI scheme");
    }
    descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  }
  if (descriptor < 0) {
    return fail<PreparedCapture>(
        ErrorCode::archive_io,
        "cannot securely open capture source: " + uri + ": " +
            std::strerror(errno));
  }
  return PreparedCapture{std::move(uri), options, descriptor, false};
}

PreparedCapture::PreparedCapture(PreparedCapture&& other) noexcept
    : uri_(std::move(other.uri_)),
      options_(other.options_),
      descriptor_(std::exchange(other.descriptor_, -1)),
      http_(other.http_) {}

PreparedCapture& PreparedCapture::operator=(PreparedCapture&& other) noexcept {
  if (this == &other) return *this;
  if (descriptor_ >= 0) ::close(descriptor_);
  uri_ = std::move(other.uri_);
  options_ = other.options_;
  descriptor_ = std::exchange(other.descriptor_, -1);
  http_ = other.http_;
  return *this;
}

PreparedCapture::~PreparedCapture() {
  if (descriptor_ >= 0) ::close(descriptor_);
}

Result<CaptureReport> PreparedCapture::run(const ByteSink& sink) {
  if (!sink) {
    return fail<CaptureReport>(ErrorCode::invalid_argument,
                               "capture sink is required");
  }
  if (http_) return capture_http(uri_, options_, sink);
  if (descriptor_ < 0) {
    return fail<CaptureReport>(ErrorCode::internal,
                               "capture source is not prepared");
  }
  return capture_descriptor(descriptor_, options_, sink);
}

}  // namespace codec::detail
