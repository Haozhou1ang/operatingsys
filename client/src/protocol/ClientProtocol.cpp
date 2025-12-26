#include "protocol/ClientProtocol.h"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>

static std::string trim_right_newlines(std::string s) {
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}

void ClientProtocol::SplitFirstLine(const std::string& s,
                                    std::string& first_line,
                                    std::string& rest) {
  size_t pos = s.find('\n');
  if (pos == std::string::npos) {
    first_line = trim_right_newlines(s);
    rest.clear();
    return;
  }
  first_line = s.substr(0, pos);
  rest = s.substr(pos + 1);
}

static std::vector<std::string> split_ws(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  std::string w;
  while (iss >> w) out.push_back(w);
  return out;
}

static std::string join_ws(const std::vector<std::string>& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out.push_back(' ');
    out += v[i];
  }
  return out;
}

static std::string upper(std::string s) {
  for (char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

std::string ClientProtocol::BuildPayload(const std::string& cmdline,
                                         const std::string& body,
                                         const std::string& token) {
  auto parts = split_ws(cmdline);
  if (parts.empty()) return "";

  std::string verb = parts[0];
  std::string cmd  = upper(verb);

  int argc = (int)parts.size() - 1;
  auto require_args = [&](int n) -> bool { return argc == n; };
  auto require_args_minmax = [&](int mn, int mx) -> bool { return argc >= mn && argc <= mx; };

  // ---- validate ----
  if (verb == "ping") {
    if (!require_args(0)) return "";
  } else if (verb == "login") {
    if (!require_args(2)) return "";
  } else if (verb == "logout") {
    if (!require_args(0)) return "";
  } else if (verb == "help" || verb == "exit") {
    return "";
  } else if (verb == "ls" || verb == "read" || verb == "mkdir") {
    if (!require_args(1)) return "";
  } else if (verb == "write") {
    if (!require_args(1)) return "";
    if (body.empty()) return "";
  } else if (verb == "upload" || verb == "revise" || verb == "reviews_give") {
    if (!require_args(1)) return "";
    if (body.empty()) return "";
  } else if (verb == "status" || verb == "download" || verb == "reviews_get" || verb == "reviews") {
    if (!require_args(1)) return "";
  } else if (verb == "papers" || verb == "tasks" || verb == "queue" ||
             verb == "user_list" || verb == "backup_list" ||
             verb == "system_status" || verb == "cache_stats" || verb == "cache_clear") {
    if (!require_args(0)) return "";
  } else if (verb == "assign") {
    if (!require_args(2)) return "";
  } else if (verb == "decide") {
    if (!require_args(2)) return "";
    std::string v = upper(parts[2]);
    if (v != "ACCEPT" && v != "REJECT") return "";
    parts[2] = v;
  } else if (verb == "user_add") {
    if (!require_args(3)) return "";
    std::string r = upper(parts[3]);
    static const std::unordered_set<std::string> kRoles = {"ADMIN","EDITOR","REVIEWER","AUTHOR"};
    if (kRoles.find(r) == kRoles.end()) return "";
    parts[3] = r;
  } else if (verb == "user_del") {
    if (!require_args(1)) return "";
  } else if (verb == "backup_create") {
    if (!require_args_minmax(0, 1)) return "";
  } else if (verb == "backup_restore") {
    if (!require_args(1)) return "";
  } else {
    return "";
  }

  std::vector<std::string> out;
  out.push_back(cmd);

  // token rule: only LOGIN and PING do not need token
  if (verb != "login" && verb != "ping") {
    if (token.empty()) return "";
    out.push_back(token);
  }

  for (size_t i = 1; i < parts.size(); ++i) out.push_back(parts[i]);

  std::string payload = join_ws(out);
  if (!body.empty()) {
    payload.push_back('\n');
    payload += body;
  }
  return payload;
}

ProtoResponse ClientProtocol::ParseResponse(const std::string& payload) {
  ProtoResponse r;

  std::string first, rest;
  SplitFirstLine(payload, first, rest);

  // OK [msg...]
  if (first.rfind("OK", 0) == 0) {
    r.ok = true;
    r.body = rest;
    if (first.size() > 2) {
      std::string msg = first.substr(2);
      if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
      r.ok_msg = msg;
    }
    return r;
  }

  // ERROR <code> <msg...>
  if (first.rfind("ERROR", 0) == 0) {
    r.ok = false;
    r.body = rest;

    std::istringstream iss(first);
    std::string word;
    iss >> word;  // ERROR
    if (!(iss >> r.err_code)) r.err_code = -1;
    std::getline(iss, r.err_msg);
    if (!r.err_msg.empty() && r.err_msg[0] == ' ') r.err_msg.erase(0, 1);
    if (r.err_msg.empty()) r.err_msg = "unknown_error";
    return r;
  }

  r.ok = false;
  r.err_code = -1;
  r.err_msg = "bad_response_format";
  r.body = payload;
  return r;
}
