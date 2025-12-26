//server/src/net/net.cpp
#include "net/net.h"
#include "protocol/protocol.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>

static constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024; // 4 MiB

static bool read_exact(int fd, char* buf, size_t n, std::string& err) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, buf + off, n - off);
    if (r == 0) { err = "peer_closed"; return false; }
    if (r < 0) {
      if (errno == EINTR) continue;
      err = std::strerror(errno);
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
      err = std::strerror(errno);
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
    if (r == 0) { err = "peer_closed"; return std::nullopt; }
    if (r < 0) {
      if (errno == EINTR) continue;
      err = std::strerror(errno);
      return std::nullopt;
    }
    s.push_back(c);
    if (c == '\n') break;
    if (s.size() > max_len) { err = "line_too_long"; return std::nullopt; }
  }
  return s;
}

static void handle_conn(int conn, ProtocolRouter* router) {
  std::string err;

  auto header_opt = read_line(conn, 64, err);
  if (!header_opt || header_opt->rfind("LEN ", 0) != 0) {
    std::string resp = Response::Err(400, "bad_len_header").Serialize();
    std::string h = "LEN " + std::to_string(resp.size()) + "\n";
    (void)write_all(conn, h.data(), h.size(), err);
    (void)write_all(conn, resp.data(), resp.size(), err);
    ::close(conn);
    return;
  }

  size_t n = 0;
  try { n = (size_t)std::stoul(header_opt->substr(4)); }
  catch (...) {
    std::string resp = Response::Err(400, "bad_len_value").Serialize();
    std::string h = "LEN " + std::to_string(resp.size()) + "\n";
    (void)write_all(conn, h.data(), h.size(), err);
    (void)write_all(conn, resp.data(), resp.size(), err);
    ::close(conn);
    return;
  }

  if (n > kMaxFrameBytes) {
    std::string resp = Response::Err(413, "payload_too_large").Serialize();
    std::string h = "LEN " + std::to_string(resp.size()) + "\n";
    (void)write_all(conn, h.data(), h.size(), err);
    (void)write_all(conn, resp.data(), resp.size(), err);
    ::close(conn);
    return;
  }

  std::string payload(n, '\0');
  if (!read_exact(conn, payload.data(), n, err)) {
    ::close(conn);
    return;
  }

  Response r = router->HandlePayload(payload);
  std::string out = r.Serialize();
  std::string out_h = "LEN " + std::to_string(out.size()) + "\n";
  (void)write_all(conn, out_h.data(), out_h.size(), err);
  (void)write_all(conn, out.data(), out.size(), err);
  ::close(conn);
}

TcpServer::TcpServer(std::string host, int port, ProtocolRouter* router)
  : host_(std::move(host)), port_(port), router_(router) {}

bool TcpServer::Start() {
  int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) { perror("socket"); return false; }

  int opt = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port_);
  addr.sin_addr.s_addr = inet_addr(host_.c_str());

  if (::bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    ::close(listen_fd);
    return false;
  }
  if (::listen(listen_fd, 64) < 0) {
    perror("listen");
    ::close(listen_fd);
    return false;
  }

  std::cout << "Server listening on " << host_ << ":" << port_ << "\n";

  while (true) {
    int conn = ::accept(listen_fd, nullptr, nullptr);
    if (conn < 0) { perror("accept"); continue; }
    std::thread(handle_conn, conn, router_).detach(); // one-thread-per-conn
  }
  ::close(listen_fd);
  return true;
}
