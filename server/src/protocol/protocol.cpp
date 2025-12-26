#include "protocol/protocol.h"
#include "auth/auth.h"
#include "services/paper.h"
#include "storage/admin_fs.h"

#include <sstream>

Response Response::Ok(std::string body, std::string ok_msg) {
  Response r;
  r.ok = true;
  r.body = std::move(body);
  r.ok_msg = std::move(ok_msg);
  return r;
}
Response Response::Err(int code, std::string msg, std::string body) {
  Response r;
  r.ok = false;
  r.err_code = code;
  r.err_msg = std::move(msg);
  r.body = std::move(body);
  return r;
}
std::string Response::Serialize() const {
  std::ostringstream oss;
  if (ok) {
    oss << "OK";
    if (!ok_msg.empty()) oss << " " << ok_msg;
    oss << "\n";
    if (!body.empty()) oss << body;
  } else {
    oss << "ERROR " << err_code << " " << (err_msg.empty() ? "unknown_error" : err_msg) << "\n";
    if (!body.empty()) oss << body;
  }
  return oss.str();
}

ProtocolRouter::ProtocolRouter(AuthManager* auth, PaperService* paper, AdminFsService* admin_fs)
  : auth_(auth), paper_(paper), admin_fs_(admin_fs) {}

void ProtocolRouter::SplitFirstLine(const std::string& s, std::string& first, std::string& rest) {
  auto pos = s.find('\n');
  if (pos == std::string::npos) { first = s; rest.clear(); return; }
  first = s.substr(0, pos);
  rest = s.substr(pos + 1);
}

std::vector<std::string> ProtocolRouter::SplitWS(const std::string& s) {
  std::istringstream iss(s);
  std::vector<std::string> out;
  std::string w;
  while (iss >> w) out.push_back(w);
  return out;
}

Response ProtocolRouter::HandlePayload(const std::string& payload) {
  std::string first, body;
  SplitFirstLine(payload, first, body);
  auto parts = SplitWS(first);
  if (parts.empty()) return Response::Err(400, "empty_command");

  std::string cmd = parts[0];
  std::vector<std::string> args;
  for (size_t i=1;i<parts.size();++i) args.push_back(parts[i]);

  // PING (no token)
  if (cmd == "PING") {
    if (!args.empty()) return Response::Err(400, "usage_PING");
    return Response::Ok();
  }

  // ---- Part1 ----
  if (cmd == "LOGIN") {
    if (args.size() != 2) return Response::Err(400, "usage_LOGIN_username_password");
    std::string role, token;
    int ec; std::string em;
    if (!auth_->Login(args[0], args[1], role, token, ec, em)) return Response::Err(ec, em);
    // required: OK <role> <token>
    return Response::Ok(/*body=*/"", /*ok_msg=*/role + " " + token);
  }

  if (cmd == "LOGOUT") {
    if (args.size() != 1) return Response::Err(400, "usage_LOGOUT_token");
    int ec; std::string em;
    if (!auth_->Logout(args[0], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "USER_ADD") {
    if (args.size() != 4) return Response::Err(400, "usage_USER_ADD_token_username_password_role");
    int ec; std::string em;
    if (!auth_->UserAdd(args[0], args[1], args[2], args[3], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "USER_DEL") {
    if (args.size() != 2) return Response::Err(400, "usage_USER_DEL_token_username");
    int ec; std::string em;
    auto cb = [&](const std::string& u, const std::string& r, std::string& why)->bool {
      return paper_->UserHasNoUnfinishedTasks(u, r, why);
    };
    if (!auth_->UserDel(args[0], args[1], ec, em, cb)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "USER_LIST") {
    if (args.size() != 1) return Response::Err(400, "usage_USER_LIST_token");
    int ec; std::string em; std::string out;
    if (!auth_->UserList(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  // ---- Admin FS ----
  if (cmd == "LS") {
    if (args.size() != 2) return Response::Err(400, "usage_LS_token_path");
    int ec; std::string em; std::string out;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->Ls(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "READ") {
    if (args.size() != 2) return Response::Err(400, "usage_READ_token_path");
    int ec; std::string em; std::string out;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->Read(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "WRITE") {
    if (args.size() != 2) return Response::Err(400, "usage_WRITE_token_path");
    if (body.empty()) return Response::Err(400, "empty_body");
    int ec; std::string em;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->Write(args[0], args[1], body, ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "MKDIR") {
    if (args.size() != 2) return Response::Err(400, "usage_MKDIR_token_path");
    int ec; std::string em;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->Mkdir(args[0], args[1], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "BACKUP_CREATE") {
    if (args.size() != 1 && args.size() != 2) return Response::Err(400, "usage_BACKUP_CREATE_token_name");
    int ec; std::string em; std::string out;
    std::string name = (args.size() == 2 ? args[1] : "");
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->BackupCreate(args[0], name, out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "BACKUP_LIST") {
    if (args.size() != 1) return Response::Err(400, "usage_BACKUP_LIST_token");
    int ec; std::string em; std::string out;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->BackupList(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "BACKUP_RESTORE") {
    if (args.size() != 2) return Response::Err(400, "usage_BACKUP_RESTORE_token_name");
    int ec; std::string em;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->BackupRestore(args[0], args[1], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "SYSTEM_STATUS") {
    if (args.size() != 1) return Response::Err(400, "usage_SYSTEM_STATUS_token");
    int ec; std::string em; std::string out;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->SystemStatus(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "CACHE_STATS") {
    if (args.size() != 1) return Response::Err(400, "usage_CACHE_STATS_token");
    int ec; std::string em; std::string out;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->CacheStats(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "CACHE_CLEAR") {
    if (args.size() != 1) return Response::Err(400, "usage_CACHE_CLEAR_token");
    int ec; std::string em;
    if (!admin_fs_) return Response::Err(500, "admin_fs_unavailable");
    if (!admin_fs_->CacheClear(args[0], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  // ---- Part2 ----
  if (cmd == "UPLOAD") {
    if (args.size() != 2) return Response::Err(400, "usage_UPLOAD_token_paperid");
    int ec; std::string em;
    if (!paper_->Upload(args[0], args[1], body, ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "REVISE") {
    if (args.size() != 2) return Response::Err(400, "usage_REVISE_token_paperid");
    int ec; std::string em;
    if (!paper_->Revise(args[0], args[1], body, ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "STATUS") {
    if (args.size() != 2) return Response::Err(400, "usage_STATUS_token_paperid");
    int ec; std::string em; std::string out;
    if (!paper_->Status(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "REVIEWS_GET") {
    if (args.size() != 2) return Response::Err(400, "usage_REVIEWS_GET_token_paperid");
    int ec; std::string em; std::string out;
    if (!paper_->ReviewsGet(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "PAPERS") {
    if (args.size() != 1) return Response::Err(400, "usage_PAPERS_token");
    int ec; std::string em; std::string out;
    if (!paper_->Papers(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "DOWNLOAD") {
    if (args.size() != 2) return Response::Err(400, "usage_DOWNLOAD_token_paperid");
    int ec; std::string em; std::string out;
    if (!paper_->Download(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "REVIEWS_GIVE") {
    if (args.size() != 2) return Response::Err(400, "usage_REVIEWS_GIVE_token_paperid");
    int ec; std::string em;
    if (!paper_->ReviewsGive(args[0], args[1], body, ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "TASKS") {
    if (args.size() != 1) return Response::Err(400, "usage_TASKS_token");
    int ec; std::string em; std::string out;
    if (!paper_->Tasks(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "ASSIGN") {
    if (args.size() != 3) return Response::Err(400, "usage_ASSIGN_token_paperid_reviewer");
    int ec; std::string em;
    if (!paper_->Assign(args[0], args[1], args[2], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "DECIDE") {
    if (args.size() != 3) return Response::Err(400, "usage_DECIDE_token_paperid_ACCEPT_or_REJECT");
    int ec; std::string em;
    if (!paper_->Decide(args[0], args[1], args[2], ec, em)) return Response::Err(ec, em);
    return Response::Ok();
  }

  if (cmd == "REVIEWS") {
    if (args.size() != 2) return Response::Err(400, "usage_REVIEWS_token_paperid");
    int ec; std::string em; std::string out;
    if (!paper_->Reviews(args[0], args[1], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  if (cmd == "QUEUE") {
    if (args.size() != 1) return Response::Err(400, "usage_QUEUE_token");
    int ec; std::string em; std::string out;
    if (!paper_->Queue(args[0], out, ec, em)) return Response::Err(ec, em);
    return Response::Ok(out);
  }

  return Response::Err(404, "unknown_command");
}
