// client/src/cli/CliApp
#include "cli/CliApp.h"
#include "net/TcpTransport.h"

#include "protocol/ClientProtocol.h"
#include "session/SessionStore.h"

#include <iostream>
#include <sstream>
#include <string>
#include <optional>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <iomanip>

static const char* kColorOk = "\033[32m";
static const char* kColorError = "\033[31m";
static const char* kColorTitle = "\033[36m";
static const char* kColorPrompt = "\033[35m";
static const char* kColorLabel = "\033[33m";
static const char* kColorMuted = "\033[2m";
static const char* kStyleBold = "\033[1m";
static const char* kStyleUnderline = "\033[4m";
static const char* kColorReset = "\033[0m";

// 统一执行一次请求：组包由外面做，网络在这里做，响应解析在这里做
static std::optional<ProtoResponse> DoRequest(const std::string& host,
                                              int port,
                                              const std::string& payload,
                                              std::string& io_error) {
  io_error.clear();

  TcpTransport t;
  if (!t.Connect(host, port, 2000)) {
    io_error = "connect failed: " + t.LastError();
    return std::nullopt;
  }
  if (!t.SendFrame(payload)) {
    io_error = "send failed: " + t.LastError();
    return std::nullopt;
  }
  auto resp = t.RecvFrame();
  if (!resp) {
    io_error = "recv failed: " + t.LastError();
    return std::nullopt;
  }
  return ClientProtocol::ParseResponse(*resp);
}

static void PrintParsedResponse(const ProtoResponse& parsed) {
  if (parsed.ok) {
    std::cout << kStyleBold << kColorOk << "✔ [OK]" << kColorReset << "\n";
    if (!parsed.body.empty()) std::cout << parsed.body;
    if (!parsed.body.empty() && parsed.body.back() != '\n') std::cout << "\n";
  } else {
    std::cout << kStyleBold << kColorError << "✖ [ERROR]" << kColorReset << " " << parsed.err_code
              << " " << parsed.err_msg << "\n";
    if (!parsed.body.empty()) std::cout << parsed.body;
    if (!parsed.body.empty() && parsed.body.back() != '\n') std::cout << "\n";
  }
}



static std::string ltrim_one_space(std::string s) {
  if (!s.empty() && s[0] == ' ' ) s.erase(0, 1);
  return s;
}

static std::string trim_ws(std::string s) {
  // trim right
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
    s.pop_back();
  // trim left
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  return s.substr(i);
}

static std::string read_body_multiline() {
  std::cout << kColorMuted << "Paste body below. End input with a single line: "
            << kStyleBold << "END" << kColorReset << kColorMuted << "." << kColorReset << "\n";
  std::string body, line;
  while (true) {
    std::getline(std::cin, line);
    if (!std::cin) break;

    std::string key = trim_ws(line);
    if (key == "END") break;

    body += line;
    body.push_back('\n');
  }
  return body;
}


static bool ParseLoginBody(const std::string& body,
                           std::string& out_token,
                           std::string& out_role) {
  out_token.clear();
  out_role.clear();

  // body 形如：
  // token=abcd
  // role=AUTHOR
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (line.rfind("token=", 0) == 0) out_token = line.substr(6);
    else if (line.rfind("role=", 0) == 0) out_role = line.substr(5);
  }
  return !out_token.empty() && !out_role.empty();
}

struct CommandSpec {
  bool needs_body;                 // 是否需要多行 body
  bool requires_auth;              // 是否必须已登录
  std::vector<std::string> roles;  // 允许的角色，空 = 所有已登录用户
  std::string usage;               // 用法
  std::string desc;                // 说明
};


static const std::unordered_map<std::string, CommandSpec>& Registry() {
  static const std::unordered_map<std::string, CommandSpec> kReg = {
      // （可选）联调命令：未登录可用
      {"ping", {false, false, {}, "ping", "ping server (no body)"}},

      // ===== ADMIN: FS =====
      {"ls",    {false, true, {"ADMIN"}, "ls <path>", "list directory (ADMIN)"}},
      {"read",  {false, true, {"ADMIN"}, "read <path>", "read file (ADMIN)"}},
      {"write", {true,  true, {"ADMIN"}, "write <path>", "write file (ADMIN, multi-line body)"}},
      {"mkdir", {false, true, {"ADMIN"}, "mkdir <path>", "create directory (ADMIN)"}},

      // ===== AUTHOR =====
      {"upload",      {true,  true, {"AUTHOR"}, "upload <paper_id>", "upload new paper (AUTHOR, multi-line body)"}},
      {"revise",      {true,  true, {"AUTHOR"}, "revise <paper_id>", "revise paper (AUTHOR, multi-line body)"}},
      {"status",      {false, true, {"AUTHOR","REVIEWER","EDITOR","ADMIN"}, "status <paper_id>", "query paper status"}},
      {"reviews_get", {false, true, {"AUTHOR"}, "reviews_get <paper_id>", "get reviews for my paper (AUTHOR)"}},
      {"papers",      {false, true, {"AUTHOR"}, "papers", "list my papers (AUTHOR)"}},

      // ===== REVIEWER =====
      {"download",     {false, true, {"REVIEWER"}, "download <paper_id>", "download assigned paper (REVIEWER)"}},
      {"reviews_give", {true,  true, {"REVIEWER"}, "reviews_give <paper_id>", "submit review (REVIEWER, multi-line body)"}},
      {"tasks",        {false, true, {"REVIEWER"}, "tasks", "list my tasks (REVIEWER)"}},

      // ===== EDITOR =====
      {"assign", {false, true, {"EDITOR"}, "assign <paper_id> <reviewer_username>", "assign reviewer (EDITOR)"}},
      {"decide", {false, true, {"EDITOR"}, "decide <paper_id> <ACCEPT|REJECT>", "final decision (EDITOR)"}},
      {"reviews",{false, true, {"EDITOR"}, "reviews <paper_id>", "view reviews for paper (EDITOR)"}},
      {"queue",  {false, true, {"EDITOR"}, "queue", "list queue (EDITOR)"}},

      // ===== ADMIN: users =====
      {"user_add",  {false, true, {"ADMIN"}, "user_add <username> <password> <role>", "create user (ADMIN)"}},
      {"user_del",  {false, true, {"ADMIN"}, "user_del <username>", "delete user (ADMIN)"}},
      {"user_list", {false, true, {"ADMIN"}, "user_list", "list users (ADMIN)"}},

      // ===== ADMIN: backup/system/cache =====
      {"backup_create",  {false, true, {"ADMIN"}, "backup_create [name]", "create backup snapshot (ADMIN)"}},
      {"backup_list",    {false, true, {"ADMIN"}, "backup_list", "list backups (ADMIN)"}},
      {"backup_restore", {false, true, {"ADMIN"}, "backup_restore <name>", "restore backup (ADMIN)"}},

      {"system_status", {false, true, {"ADMIN"}, "system_status", "show system status (ADMIN)"}},
      {"cache_stats",   {false, true, {"ADMIN"}, "cache_stats", "show cache stats (ADMIN)"}},
      {"cache_clear",   {false, true, {"ADMIN"}, "cache_clear", "clear cache (ADMIN)"}},
  };
  return kReg;
}




static void PrintBusinessHelp(const SessionStore& session) {
  std::vector<std::pair<std::string, CommandSpec>> items;
  items.reserve(Registry().size());
  for (const auto& kv : Registry()) items.push_back(kv);

  std::sort(items.begin(), items.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  const int kUsageWidth = 30;
  std::cout << "\n" << kColorTitle << kStyleBold << "Business commands"
            << kColorReset << " " << kColorMuted << "(role-aware)" << kColorReset << "\n";
  std::cout << kColorMuted << "────────────────────────────────────────" << kColorReset << "\n";

  auto group_label = [](const CommandSpec& spec) -> std::string {
    if (!spec.requires_auth) return "PUBLIC";
    if (spec.roles.empty()) return "ALL AUTHENTICATED";
    std::string label;
    for (size_t i = 0; i < spec.roles.size(); ++i) {
      if (i > 0) label += "/";
      label += spec.roles[i];
    }
    return label;
  };

  std::string current_group;
  for (const auto& it : items) {
    const auto& spec = it.second;

    bool show = true;
    if (spec.requires_auth && !session.IsLoggedIn()) show = false;

    if (show && session.IsLoggedIn() && !spec.roles.empty()) {
      if (std::find(spec.roles.begin(), spec.roles.end(), session.Role()) == spec.roles.end()) {
        show = false;
      }
    }

    if (!show) continue;

    std::string next_group = group_label(spec);
    if (next_group != current_group) {
      current_group = next_group;
      std::cout << "\n" << kColorLabel << kStyleBold << current_group << kColorReset
                << " " << kColorMuted << "┆" << kColorReset << "\n";
    }

    std::string desc = spec.desc;
    if (spec.needs_body) {
      if (!desc.empty()) desc += " ";
      desc += kColorMuted;
      desc += "[body]";
      desc += kColorReset;
    }

    std::cout << "  " << kColorPrompt << "›" << kColorReset << " "
              << std::left << std::setw(kUsageWidth) << spec.usage;
    if (!desc.empty()) std::cout << desc;
    std::cout << "\n";
  }
  std::cout << "\n";
}


static void PrintHelp(const SessionStore& session) {
  const int kUsageWidth = 26;
  std::cout << kStyleBold << kColorTitle << "Paper System CLI" << kColorReset
            << " " << kColorMuted << "— interactive terminal" << kColorReset << "\n";
  std::cout << kColorMuted << "────────────────────────────────────────" << kColorReset << "\n";
  std::cout << kColorLabel << kStyleUnderline << "Commands" << kColorReset << "\n";
  auto print_row = [kUsageWidth](const std::string& usage, const std::string& desc) {
    std::cout << "  " << kColorPrompt << "›" << kColorReset << " "
              << std::left << std::setw(kUsageWidth) << usage << desc << "\n";
  };
  print_row("help", "show help");
  print_row("connect <ip> <port>", "set server address");
  print_row("login <user> <pass>", "login");
  print_row("whoami", "show current session");
  print_row("logout", "logout");
  print_row("exit", "exit client");
  print_row("send <commandLine>", "debug: send raw command");
  print_row("sendb <commandLine>", "debug: send with multi-line body");
  PrintBusinessHelp(session);
}





void CliApp::Run() {
  std::string host = "127.0.0.1";
  int port = 9090;

  std::string line;
  SessionStore session;
  PrintHelp(session);

  while (true) {
    std::cout << kColorPrompt << kStyleBold << "paper" << kColorReset
              << kColorMuted << "▸ " << kColorReset << std::flush;
    if (!std::getline(std::cin, line)) break;

    std::istringstream iss(line);
    std::string op;
    iss >> op;
    if (op.empty()) continue;

    if (op == "exit" || op == "quit") break;

    if (op == "help") {
      PrintHelp(session);
      continue;
    }



    if (op == "connect") {
      iss >> host >> port;
      if (host.empty() || port <= 0) {
        std::cout << "Usage: connect <ip> <port>\n";
        continue;
      }
      std::cout << "Server set to " << host << ":" << port << "\n";
      continue;
    }

    if (op == "whoami"){
        if (!session.IsLoggedIn()){
            std::cout << "Not logged in.\n";
        }else{
            const std::string& tok = session.Token();
            std::string tok8 = tok.substr(0, std::min<size_t>(8, tok.size()));
            std::cout << "Logged in. role=" << session.Role()
                      << " token=" << tok8;
            if (tok.size() > tok8.size()) std::cout << "...";
            std::cout << "\n";  
        }
        continue;
    }

    if (op == "logout") {
        if (!session.IsLoggedIn()) {
            std::cout << "Not logged in.\n";
            continue;
        }

        // 组装 logout 命令（走 BuildPayload，会注入 token 到命令行里）
        std::string cmdline = "logout";
        std::string payload = ClientProtocol::BuildPayload(cmdline, /*body=*/"", session.Token());
        if (payload.empty()) {
            std::cout << "Usage: logout\n";
            continue;
        }

        std::string io_error;
        auto parsed_opt = DoRequest(host, port, payload, io_error);
        if (!parsed_opt) {
            std::cout << io_error << "\n";
            std::cout << "Logout failed; local session kept.\n";
            continue;
        }

        const auto& parsed = *parsed_opt;
        PrintParsedResponse(parsed);

        if (parsed.ok) {
            session.Clear();
            std::cout << "Logged out (local session cleared).\n";
        } else {
            std::cout << "Logout failed; local session kept.\n";
        }
        continue;
    }


    if (op == "login") {
        if (session.IsLoggedIn()) {
            std::cout << "Already logged in as role=" << session.Role()
                      << ". Please logout first.\n";
            continue;
        }

        std::string user, pass;
        iss >> user >> pass;
        if (user.empty() || pass.empty()) {
            std::cout << "Usage: login <user> <pass>\n";
            continue;
        }

        std::string cmdline = "login " + user + " " + pass;
        std::string payload =
            ClientProtocol::BuildPayload(cmdline, /*body=*/"", /*token=*/"");

        std::string io_error;
        auto parsed_opt = DoRequest(host, port, payload, io_error);
        if (!parsed_opt) {
            std::cout << io_error << "\n";
            continue;
        }

        const auto& parsed = *parsed_opt;
        if (!parsed.ok) {
            PrintParsedResponse(parsed);
            continue;
        }

        // OK <role> <token>
        std::istringstream iss2(parsed.ok_msg);
        std::string role, token;
        iss2 >> role >> token;
        if (role.empty() || token.empty()) {
            std::cout << "ERROR -1 bad_login_response_format\n";
            continue;
        }

        session.Set(token, role);
        std::cout << "OK\nLogged in. role=" << role << "\n";
        continue;
    }




    if (op == "send" || op == "sendb") {
      std::string cmdline;
      std::getline(iss, cmdline);
      cmdline = ltrim_one_space(cmdline);
      if (cmdline.empty()) {
        std::cout << "Usage: " << op << " <commandLine>\n";
        continue;
      }

      std::string body;
      if (op == "sendb") body = read_body_multiline();

      
      std::string payload = ClientProtocol::BuildPayload(cmdline, body, session.Token());



      std::string io_error;
      auto parsed_opt = DoRequest(host, port, payload, io_error);
      if (!parsed_opt){
        std::cout << io_error << "\n";
        continue;
      }
      PrintParsedResponse(*parsed_opt);
      continue;
    }


    
// ---- Fallback: business command ----
    std::string cmdline = line;
    std::string verb = op;

    auto it = Registry().find(verb);
    if (it == Registry().end()) {
      std::cout << "Unknown command: " << verb << " (type 'help')\n";
      continue;
    }

    const auto& spec = it->second;

    // auth check
    if (spec.requires_auth && !session.IsLoggedIn()) {
      std::cout << "Not logged in.\n";
      continue;
    }

    // role check
    if (!spec.roles.empty()) {
      if (!session.IsLoggedIn()) {
        std::cout << "Not logged in.\n";
        continue;
      }
      if (std::find(spec.roles.begin(), spec.roles.end(), session.Role()) == spec.roles.end()) {
        std::cout << "Permission denied for role " << session.Role() << "\n";
        continue;
      }
    }

    // read body if needed
    std::string body;
    if (spec.needs_body) {
      body = read_body_multiline();
    }

    // build payload (protocol mapping + token insertion happens inside BuildPayload)
    std::string payload = ClientProtocol::BuildPayload(cmdline, body, session.Token());
    if (payload.empty()) {
      // BuildPayload returns empty when usage/args invalid
      std::cout << "Usage: " << spec.usage << "\n";
      continue;
    }

    std::string io_error;
    auto parsed_opt = DoRequest(host, port, payload, io_error);
    if (!parsed_opt) {
      std::cout << io_error << "\n";
      continue;
    }
    PrintParsedResponse(*parsed_opt);
    continue;




    std::cout << "Unknown command. Type 'help'.\n";
  }
}
