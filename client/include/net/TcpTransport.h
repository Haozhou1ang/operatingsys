// client/include/net/TcpTransport
#pragma once
#include <string>
#include <optional>

class TcpTransport {
public:
  bool Connect(const std::string& host, int port, int timeout_ms);
  bool SendFrame(const std::string& payload);
  std::optional<std::string> RecvFrame();
  void Close();
  std::string LastError() const;

private:
  int sockfd_ = -1;
  std::string last_error_;
};
