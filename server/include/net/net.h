//server/include/net/net.h
#pragma once
#include <string>
#include <functional>

class ProtocolRouter;

class TcpServer {
public:
  TcpServer(std::string host, int port, ProtocolRouter* router);
  bool Start(); // blocking accept loop

private:
  std::string host_;
  int port_;
  ProtocolRouter* router_;
};
