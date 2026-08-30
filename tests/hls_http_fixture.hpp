#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

struct HlsHttpResponse {
  int status{200};
  std::string content_type{"application/octet-stream"};
  std::vector<std::byte> body;
  std::vector<std::vector<std::byte>> subsequent_bodies{};
};

class HlsHttpFixture {
 public:
  explicit HlsHttpFixture(std::map<std::string, HlsHttpResponse> responses)
      : responses_(std::move(responses)) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) throw std::runtime_error("cannot create HLS test socket");

    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(0);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address),
               sizeof(address)) != 0) {
      close_listener();
      throw std::runtime_error("cannot bind HLS test socket");
    }
    if (::listen(listener_, 8) != 0) {
      close_listener();
      throw std::runtime_error("cannot listen on HLS test socket");
    }

    socklen_t address_size = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &address_size) != 0) {
      close_listener();
      throw std::runtime_error("cannot inspect HLS test socket");
    }
    port_ = ntohs(address.sin_port);
    server_ = std::thread([this] { serve(); });
  }

  ~HlsHttpFixture() {
    stopping_.store(true);
    if (listener_ >= 0) {
      (void)::shutdown(listener_, SHUT_RDWR);
      close_listener();
    }
    if (server_.joinable()) server_.join();
  }

  HlsHttpFixture(const HlsHttpFixture&) = delete;
  HlsHttpFixture& operator=(const HlsHttpFixture&) = delete;

  std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
  }

  std::size_t requests(std::string_view path) const {
    std::scoped_lock lock(mutex_);
    const auto found = requests_.find(std::string{path});
    return found == requests_.end() ? 0U : found->second;
  }

 private:
  static bool send_all(int socket, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto sent = ::send(socket, bytes.data() + offset,
                               bytes.size() - offset, MSG_NOSIGNAL);
      if (sent <= 0) return false;
      offset += static_cast<std::size_t>(sent);
    }
    return true;
  }

  static bool send_all(int socket, const std::vector<std::byte>& bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto sent = ::send(
          socket, reinterpret_cast<const char*>(bytes.data() + offset),
          bytes.size() - offset, MSG_NOSIGNAL);
      if (sent <= 0) return false;
      offset += static_cast<std::size_t>(sent);
    }
    return true;
  }

  void close_listener() noexcept {
    if (listener_ >= 0) {
      (void)::close(listener_);
      listener_ = -1;
    }
  }

  void serve() noexcept {
    while (!stopping_.load()) {
      sockaddr_in peer{};
      socklen_t peer_size = sizeof(peer);
      const int connection =
          ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peer_size);
      if (connection < 0) {
        if (stopping_.load()) return;
        if (errno == EINTR) continue;
        return;
      }
      serve_one(connection);
      (void)::close(connection);
    }
  }

  void serve_one(int connection) noexcept {
    std::string request;
    request.reserve(2048);
    char buffer[1024];
    while (request.find("\r\n\r\n") == std::string::npos &&
           request.size() < 16U * 1024U) {
      const auto received = ::recv(connection, buffer, sizeof(buffer), 0);
      if (received <= 0) return;
      request.append(buffer, static_cast<std::size_t>(received));
    }

    const auto line_end = request.find("\r\n");
    if (line_end == std::string::npos) return;
    const auto first_line = request.substr(0, line_end);
    constexpr std::string_view prefix = "GET ";
    const auto version = first_line.rfind(" HTTP/");
    if (!first_line.starts_with(prefix) || version == std::string::npos ||
        version <= prefix.size()) {
      send_response(connection, HlsHttpResponse{.status = 400, .content_type = "text/plain", .body = {}});
      return;
    }

    const auto target = first_line.substr(prefix.size(), version - prefix.size());
    HlsHttpResponse response{.status = 404, .content_type = "text/plain", .body = {}};
    {
      std::scoped_lock lock(mutex_);
      const auto request_index = requests_[target]++;
      auto found = responses_.find(target);
      if (found == responses_.end()) {
        const auto query = target.find('?');
        if (query != std::string::npos) {
          found = responses_.find(target.substr(0, query));
        }
      }
      if (found != responses_.end()) {
        response = found->second;
        if (request_index != 0U && !response.subsequent_bodies.empty()) {
          const auto body_index =
              std::min(request_index - 1U, response.subsequent_bodies.size() - 1U);
          response.body = response.subsequent_bodies[body_index];
        }
      }
    }
    send_response(connection, response);
  }

  static void send_response(int connection, const HlsHttpResponse& response) {
    const std::string reason = response.status == 200 ? "OK" :
                               response.status == 400 ? "Bad Request" :
                                                        "Not Found";
    const auto header =
        "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\n" +
        "Content-Type: " + response.content_type + "\r\n" +
        "Content-Length: " + std::to_string(response.body.size()) + "\r\n" +
        "Connection: close\r\n\r\n";
    if (!send_all(connection, header)) return;
    (void)send_all(connection, response.body);
  }

  std::map<std::string, HlsHttpResponse> responses_;
  mutable std::mutex mutex_;
  std::map<std::string, std::size_t> requests_;
  std::atomic<bool> stopping_{false};
  int listener_{-1};
  std::uint16_t port_{};
  std::thread server_;
};
