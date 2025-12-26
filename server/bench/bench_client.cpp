// server/bench/bench_client.cpp
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static bool read_exact(int fd, char* buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, buf + off, n - off);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(r);
  }
  return true;
}

static bool write_all(int fd, const char* buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = ::write(fd, buf + off, n - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

static bool read_line(int fd, std::string& out) {
  out.clear();
  char c = 0;
  while (true) {
    ssize_t r = ::read(fd, &c, 1);
    if (r == 0) return false;
    if (r < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    out.push_back(c);
    if (c == '\n') break;
    if (out.size() > 256) return false;
  }
  return true;
}

static bool send_payload(int fd, const std::string& payload, std::string& response) {
  std::string header = "LEN " + std::to_string(payload.size()) + "\n";
  if (!write_all(fd, header.data(), header.size())) return false;
  if (!write_all(fd, payload.data(), payload.size())) return false;

  std::string resp_header;
  if (!read_line(fd, resp_header)) return false;
  if (resp_header.rfind("LEN ", 0) != 0) return false;

  size_t n = 0;
  try {
    n = static_cast<size_t>(std::stoul(resp_header.substr(4)));
  } catch (...) {
    return false;
  }

  response.assign(n, '\0');
  return read_exact(fd, response.data(), n);
}

static int connect_to(const std::string& host, int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = inet_addr(host.c_str());
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

static bool do_login(const std::string& host, int port,
                     const std::string& user, const std::string& pass,
                     std::string& token_out) {
  static std::atomic<bool> logged{false};
  int fd = connect_to(host, port);
  if (fd < 0) return false;
  std::string payload = "LOGIN " + user + " " + pass + "\n";
  std::string resp;
  bool ok = send_payload(fd, payload, resp);
  ::close(fd);
  if (!ok) {
    if (!logged.exchange(true)) {
      std::cerr << "login_failed: no_response\n";
    }
    return false;
  }
  if (resp.rfind("OK ", 0) != 0) {
    if (!logged.exchange(true)) {
      std::cerr << "login_failed: " << resp << "\n";
    }
    return false;
  }
  // OK <role> <token>
  std::istringstream iss(resp.substr(3));
  std::string role;
  if (!(iss >> role >> token_out)) return false;
  return true;
}

static std::string render_template(const std::string& tpl, const std::string& token) {
  std::string out = tpl;
  const std::string needle = "{token}";
  size_t pos = 0;
  while ((pos = out.find(needle, pos)) != std::string::npos) {
    out.replace(pos, needle.size(), token);
    pos += token.size();
  }
  return out;
}

int main(int argc, char** argv) {
  if (argc < 6) {
    std::cerr << "usage: bench_client <host> <port> <threads> <req_per_thread> <cmd_tpl> [user pass]\n";
    std::cerr << "example: bench_client 127.0.0.1 9090 4 200 \"QUEUE {token}\" editor 123\n";
    return 2;
  }

  std::string host = argv[1];
  int port = std::stoi(argv[2]);
  int threads = std::stoi(argv[3]);
  int reqs = std::stoi(argv[4]);
  std::string cmd_tpl = argv[5];
  std::string user = (argc >= 8) ? argv[6] : "";
  std::string pass = (argc >= 8) ? argv[7] : "";

  std::atomic<int> ok_count{0};
  std::atomic<int> err_count{0};
  std::atomic<int> conn_fail{0};
  std::atomic<int> login_fail{0};
  std::atomic<int> send_fail{0};
  std::atomic<int> resp_fail{0};

  auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(threads));
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t]() {
      std::string token;
      if (!user.empty()) {
        if (!do_login(host, port, user, pass, token)) {
          login_fail.fetch_add(1);
          err_count.fetch_add(reqs);
          return;
        }
      }

      for (int i = 0; i < reqs; ++i) {
        int fd = connect_to(host, port);
        if (fd < 0) {
          conn_fail.fetch_add(1);
          err_count.fetch_add(1);
          continue;
        }
        std::string payload = render_template(cmd_tpl, token) + "\n";
        std::string resp;
        bool ok = send_payload(fd, payload, resp);
        ::close(fd);
        if (!ok) {
          send_fail.fetch_add(1);
          err_count.fetch_add(1);
          continue;
        }
        if (resp.rfind("OK", 0) != 0) {
          resp_fail.fetch_add(1);
          err_count.fetch_add(1);
        } else {
          ok_count.fetch_add(1);
        }
      }
    });
  }

  for (auto& th : workers) th.join();
  auto end = std::chrono::steady_clock::now();

  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  int total = threads * reqs;
  double secs = ms / 1000.0;
  double rps = secs > 0 ? total / secs : 0.0;

  std::cout << "total=" << total
            << " ok=" << ok_count.load()
            << " err=" << err_count.load()
            << " conn_fail=" << conn_fail.load()
            << " login_fail=" << login_fail.load()
            << " send_fail=" << send_fail.load()
            << " resp_fail=" << resp_fail.load()
            << " time_ms=" << ms
            << " rps=" << rps
            << "\n";
  return 0;
}
