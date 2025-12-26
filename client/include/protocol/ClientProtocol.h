#pragma once
#include <string>

struct ProtoResponse {
  bool ok = false;

  // OK: first line extra part, e.g. "AUTHOR abcd..."
  std::string ok_msg;

  // For ERROR:
  int err_code = 0;
  std::string err_msg;

  // For OK/ERROR body:
  std::string body;
};

class ClientProtocol {
public:
  static std::string BuildPayload(const std::string& commandLine,
                                  const std::string& body,
                                  const std::string& token);

  static ProtoResponse ParseResponse(const std::string& payload);

  static void SplitFirstLine(const std::string& s,
                             std::string& first_line,
                             std::string& rest);
};
