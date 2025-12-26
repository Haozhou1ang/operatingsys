#pragma once
#include <string>
#include <vector>

class AuthManager;
class PaperService;
class AdminFsService;

struct Response {
  bool ok = false;
  int err_code = 0;
  std::string err_msg;
  std::string body;

  // NEW: OK extra fields in first line: "OK <ok_msg>"
  std::string ok_msg;

  static Response Ok(std::string body = "", std::string ok_msg = "");
  static Response Err(int code, std::string msg, std::string body = "");
  std::string Serialize() const;  // "OK [msg]\n<body>" or "ERROR code msg\n<body>"
};

class ProtocolRouter {
public:
  ProtocolRouter(AuthManager* auth, PaperService* paper, AdminFsService* admin_fs);
  Response HandlePayload(const std::string& payload);

private:
  AuthManager* auth_;
  PaperService* paper_;
  AdminFsService* admin_fs_;

  static void SplitFirstLine(const std::string& s, std::string& first, std::string& rest);
  static std::vector<std::string> SplitWS(const std::string& s);
};
