// client/src/net/TcpTransport
#include "net/TcpTransport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>

static constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024; // 4 MiB

static bool read_exact(int fd, char* buf, size_t n, std::string& err) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, buf + off, n - off);
    if (r == 0) { err = "read_exact: peer closed"; return false; }
    if (r < 0) {
      if (errno == EINTR) continue;
      err = std::string("read_exact: ") + std::strerror(errno);
      return false;
    }
    off += (size_t)r;
  }
  return true;
}

static bool write_all(int fd, const char* buf, size_t n, std::string& err) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      err = std::string("write_all: ") + std::strerror(errno);
      return false;
    }
    off += (size_t)w;
  }
  return true;
}

static std::optional<std::string> read_line(int fd, size_t max_len, std::string& err) {
  std::string s;
  s.reserve(64);
  char c = 0;
  while (true) {
    ssize_t r = ::read(fd, &c, 1);
    if (r == 0) { err = "read_line: peer closed"; return std::nullopt; }
    if (r < 0) {
      if (errno == EINTR) continue;
      err = std::string("read_line: ") + std::strerror(errno);
      return std::nullopt;
    }
    s.push_back(c);
    if (c == '\n') break;
    if (s.size() > max_len) { err = "read_line: too long"; return std::nullopt; }
  }
  return s;
}

bool TcpTransport::Connect(const std::string& host, int port, int /*timeout_ms*/) {
  Close();
  last_error_.clear();

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo* res = nullptr;
  std::string port_s = std::to_string(port);
  int rc = ::getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res);
  if (rc != 0) {
    last_error_ = std::string("getaddrinfo: ") + gai_strerror(rc);
    return false;
  }

  int fd = -1;
  for (addrinfo* p = res; p; p = p->ai_next) {
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    ::close(fd);
    fd = -1;
  }
  ::freeaddrinfo(res);

  if (fd < 0) {
    last_error_ = std::string("connect failed: ") + std::strerror(errno);
    return false;
  }

  sockfd_ = fd;
  return true;
}

bool TcpTransport::SendFrame(const std::string& payload) {
  last_error_.clear();
  if (sockfd_ < 0) { last_error_ = "SendFrame: not connected"; return false; }

  if (payload.size() > kMaxFrameBytes) {
    last_error_ = "SendFrame: payload too large";
    return false;
  }

  std::string header = "LEN " + std::to_string(payload.size()) + "\n";
  if (!write_all(sockfd_, header.data(), header.size(), last_error_)) return false;
  if (!write_all(sockfd_, payload.data(), payload.size(), last_error_)) return false;
  return true;
}

std::optional<std::string> TcpTransport::RecvFrame() {
  last_error_.clear();
  if (sockfd_ < 0) { last_error_ = "RecvFrame: not connected"; return std::nullopt; }

  auto header_opt = read_line(sockfd_, 64, last_error_);
  if (!header_opt) return std::nullopt;
  const std::string& header = *header_opt;

  if (header.rfind("LEN ", 0) != 0) {
    last_error_ = "RecvFrame: bad header (missing LEN)";
    return std::nullopt;
  }

  size_t n = 0;
  try {
    n = (size_t)std::stoul(header.substr(4));
  } catch (...) {
    last_error_ = "RecvFrame: bad length";
    return std::nullopt;
  }

  if (n > kMaxFrameBytes) {
    last_error_ = "RecvFrame: payload too large";
    return std::nullopt;
  }

  std::string payload(n, '\0');
  if (!read_exact(sockfd_, payload.data(), n, last_error_)) return std::nullopt;
  return payload;
}

void TcpTransport::Close() {
  if (sockfd_ >= 0) {
    ::close(sockfd_);
    sockfd_ = -1;
  }
}

std::string TcpTransport::LastError() const { return last_error_; }
