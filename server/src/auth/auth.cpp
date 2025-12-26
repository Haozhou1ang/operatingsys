//server/src/auth/auth.cpp
#include "auth/auth.h"
#include "storage/storage.h"
#include "FS.h"
#include <fstream>
#include <sstream>
#include <random>
#include <filesystem>

using namespace std;

static bool IsValidSimpleName(const std::string& s) {
  if (s.empty()) return false;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '_' || c == '-') {
      continue;
    }
    return false;
  }
  return true;
}

static bool IsSafeCsvField(const std::string& s) {
  // Prevent CSV injection/field pollution: forbid delimiter/newlines.
  // (Also avoids path traversal if these fields are ever used in paths.)
  for (unsigned char c : s) {
    // NOTE: current CSV parser is minimal; forbid quotes to avoid parse ambiguity.
    if (c == ',' || c == '\n' || c == '\r' || c == '"') return false;
  }
  return true;
}

static vector<string> SplitCSVLine(const string& line) {
  vector<string> out;
  string cur;
  bool inq = false;
  for (char c : line) {
    if (c == '"') { inq = !inq; continue; }
    if (!inq && c == ',') { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

static string Trim(string s) {
  while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ' || s.back()=='\t')) s.pop_back();
  size_t i=0;
  while (i<s.size() && (s[i]==' '||s[i]=='\t')) i++;
  return s.substr(i);
}

AuthManager::AuthManager(std::string root_dir, int token_ttl_sec,
                         fs::FileSystem* vfs, std::string vfs_root)
  : root_(std::move(root_dir))
  , ttl_sec_(token_ttl_sec)
  , vfs_(vfs)
  , vfs_root_(std::move(vfs_root)) {
  users_path_ = root_ + "/users.csv";
}

bool AuthManager::IsValidRole(const string& r) {
  return r=="ADMIN" || r=="EDITOR" || r=="REVIEWER" || r=="AUTHOR";
}

string AuthManager::GenToken() {
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<unsigned long long> dis;

  unsigned long long a = dis(gen);
  unsigned long long b = dis(gen);
  std::ostringstream oss;
  oss << std::hex << a << b;
  return oss.str();
}

bool AuthManager::Init(bool fresh_root) {
  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  std::lock_guard<std::mutex> lk(mu_);

  if (fresh_root) {
    if (vfs_) {
      if (vfs_->mkdirp(root_) != fs::ErrorCode::OK) return false;
    } else {
      std::filesystem::create_directories(root_);
    }
    users_.clear();
    users_["admin"]   = {"admin","123","ADMIN"};
    users_["alice"]   = {"alice","123","AUTHOR"};
    users_["reviewer"]= {"reviewer","123","REVIEWER"};
    users_["editor"]  = {"editor","123","EDITOR"};
    users_["reviewer1"]  = {"reviewer1","123","REVIEWER"};
    users_["bob"]   = {"bob","123","AUTHOR"};
    if (SaveUsersLocked() != PersistResult::OK) return false;
  } else {
    if (!LoadUsersLocked() || users_.empty()) return false;
  }

  sessions_.clear();
  user2token_.clear();
  return true;
}

static bool ReadTextFileLocked(fs::FileSystem* vfs, const string& path, string& out) {
  if (!vfs) {
    ifstream fin(path);
    if (!fin.is_open()) return false;
    ostringstream oss;
    oss << fin.rdbuf();
    out = oss.str();
    return true;
  }
  auto r = vfs->readFileAsString(path);
  if (!r.ok()) return false;
  out = r.value();
  return true;
}

static PersistResult WriteTextFileLocked(fs::FileSystem* vfs, const string& path, const string& content) {
  if (!vfs) {
    ofstream fout(path, ios::trunc);
    if (!fout.is_open()) return PersistResult::WRITE_FAIL;
    fout << content;
    return fout.good() ? PersistResult::OK : PersistResult::WRITE_FAIL;
  }
  if (!vfs->exists(path)) {
    if (vfs->create(path) != fs::ErrorCode::OK) return PersistResult::WRITE_FAIL;
  }
  if (vfs->truncate(path, 0) != fs::ErrorCode::OK) return PersistResult::WRITE_FAIL;
  auto w = vfs->writeFile(path, content, 0);
  if (!w.ok()) return PersistResult::WRITE_FAIL;
  return vfs->sync() == fs::ErrorCode::OK
           ? PersistResult::OK
           : PersistResult::SYNC_FAIL;
}

bool AuthManager::LoadUsersLocked() {
  users_.clear();
  string content;
  if (!ReadTextFileLocked(vfs_, users_path_, content)) return false;

  istringstream iss(content);
  string line;
  while (std::getline(iss, line)) {
    line = Trim(line);
    if (line.empty()) continue;
    auto cols = SplitCSVLine(line);
    if (cols.size() != 3) continue;
    UserInfo u{Trim(cols[0]), Trim(cols[1]), Trim(cols[2])};
    if (!IsValidSimpleName(u.username)) continue;
    if (u.password.empty() || !IsSafeCsvField(u.password)) continue;
    if (!IsValidRole(u.role)) continue;
    users_[u.username] = u;
  }
  return true;
}

PersistResult AuthManager::SaveUsersLocked() {
  ostringstream fout;
  for (auto& kv : users_) {
    auto& u = kv.second;
    fout << u.username << "," << u.password << "," << u.role << "\n";
  }
  return WriteTextFileLocked(vfs_, users_path_, fout.str());
}

TokenState AuthManager::CheckToken(const string& token, SessionInfo* out_session) {
  if (token.empty()) return TokenState::EMPTY;

  std::lock_guard<std::mutex> lk(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) return TokenState::NOT_FOUND;

  std::time_t now = std::time(nullptr);
  if (!(ttl_sec_ > 0 && (now - it->second.create_time) > ttl_sec_)) {
    if (out_session) *out_session = it->second;
    return TokenState::OK;
  }

  // expired => delete session + user2token mapping
  const std::string username = it->second.username;

  sessions_.erase(it);
  auto it2 = user2token_.find(username);
  if (it2 != user2token_.end() && it2->second == token) {
    user2token_.erase(it2);
  }

  return TokenState::EXPIRED;
}


bool AuthManager::GetSessionByToken(const string& token, SessionInfo& out) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) return false;
  out = it->second;
  return true;
}

bool AuthManager::UserExists(const string& username, UserInfo* out) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = users_.find(username);
  if (it == users_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool AuthManager::Login(const string& username, const string& password,
                        string& out_role, string& out_token,
                        int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  if (username.empty() || password.empty()) {
    err_code = 400; err_msg = "empty_username_or_password";
    return false;
  }

  std::lock_guard<std::mutex> lk(mu_);
  auto it = users_.find(username);
  if (it == users_.end() || it->second.password != password) {
    err_code = 401; err_msg = "invalid_credentials";
    return false;
  }

  // if already has token => delete old session row
  auto it2 = user2token_.find(username);
  if (it2 != user2token_.end()) {
    sessions_.erase(it2->second);
    user2token_.erase(it2);
  }

  SessionInfo s;
  s.username = username;
  s.role = it->second.role;
  s.token = GenToken();
  s.create_time = std::time(nullptr);

  sessions_[s.token] = s;
  user2token_[username] = s.token;

  out_role = s.role;
  out_token = s.token;
  return true;
}

bool AuthManager::Logout(const string& token, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  if (token.empty()) {
    err_code = 400; err_msg = "empty_token";
    return false;
  }

  std::lock_guard<std::mutex> lk(mu_);
  auto it = sessions_.find(token);
  if (it == sessions_.end()) {
    err_code = 404; err_msg = "token_not_found";
    return false;
  }
  std::time_t now = std::time(nullptr);
  if (ttl_sec_ > 0 && (now - it->second.create_time) > ttl_sec_) {
    // expired: still can delete? spec says error and delete on success path
    err_code = 403; err_msg = "token_expired";
    return false;
  }

  user2token_.erase(it->second.username);
  sessions_.erase(it);
  return true;
}

bool AuthManager::UserAdd(const string& token, const string& username,
                          const string& password, const string& role,
                          int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  if (token.empty() || username.empty() || password.empty() || role.empty()) {
    err_code = 400; err_msg = "empty_fields";
    return false;
  }

  // NEW: username allow [A-Za-z0-9_-]
  if (!IsValidSimpleName(username)) {
    err_code = 400;
    err_msg = "invalid_username";
    return false;
  }
  // NEW: forbid CSV delimiter/newlines in password to avoid CSV injection/pollution.
  if (!IsSafeCsvField(password)) {
    err_code = 400;
    err_msg = "invalid_password";
    return false;
  }

  SessionInfo s;
  auto st = CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "ADMIN") {
    err_code = 403; err_msg = "permission_denied";
    return false;
  }
  if (!IsValidRole(role)) {
    err_code = 400; err_msg = "invalid_role";
    return false;
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  std::lock_guard<std::mutex> lk(mu_);
  if (users_.find(username) != users_.end()) {
    err_code = 409; err_msg = "user_exists";
    return false;
  }
  users_[username] = {username, password, role};
  PersistResult pr = SaveUsersLocked();
  if (pr != PersistResult::OK) {
    err_code = 500;
    err_msg = (pr == PersistResult::SYNC_FAIL ? "sync_failed" : "persist_failed");
    return false;
  }
  return true;
}


bool AuthManager::UserDel(const string& token, const string& username,
                          int& err_code, string& err_msg,
                          const function<bool(const string&, const string&, string& why)>& unfinished_check) {
  err_code = 0; err_msg.clear();
  if (token.empty() || username.empty()) {
    err_code = 400; err_msg = "empty_fields";
    return false;
  }

  SessionInfo s;
  auto st = CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "ADMIN") {
    err_code = 403; err_msg = "permission_denied";
    return false;
  }

  string target_role;
  {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = users_.find(username);
    if (it == users_.end()) {
      err_code = 404; err_msg = "user_not_found";
      return false;
    }
    if (it->second.role == "ADMIN") {
      err_code = 400; err_msg = "cannot_delete_admin";
      return false;
    }
    // must be offline
    if (user2token_.find(username) != user2token_.end()) {
      err_code = 409; err_msg = "user_online";
      return false;
    }
    target_role = it->second.role;
  }

  // unfinished tasks check via callback (paper service)
  if (unfinished_check) {
    string why;
    if (!unfinished_check(username, target_role, why)) {
      err_code = 409;
      err_msg = "user_has_unfinished_tasks";
      if (!why.empty()) err_msg += (":" + why);
      return false;
    }
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  std::lock_guard<std::mutex> lk(mu_);
  auto it = users_.find(username);
  if (it == users_.end()) {
    err_code = 404; err_msg = "user_not_found";
    return false;
  }
  if (it->second.role == "ADMIN") {
    err_code = 400; err_msg = "cannot_delete_admin";
    return false;
  }
  if (user2token_.find(username) != user2token_.end()) {
    err_code = 409; err_msg = "user_online";
    return false;
  }

  // remove user and any stale session rows (defensive)
  users_.erase(it);

  for (auto sit = sessions_.begin(); sit != sessions_.end(); ) {
    if (sit->second.username == username) sit = sessions_.erase(sit);
    else ++sit;
  }
  user2token_.erase(username);

  PersistResult pr_users = SaveUsersLocked();
  if (pr_users != PersistResult::OK) {
    err_code = 500;
    err_msg = (pr_users == PersistResult::SYNC_FAIL ? "sync_failed" : "persist_failed");
    return false;
  }
  return true;
}

bool AuthManager::UserList(const string& token,
                           string& out_body,
                           int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  out_body.clear();

  if (token.empty()) {
    err_code = 400; err_msg = "empty_token";
    return false;
  }

  SessionInfo s;
  auto st = CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "ADMIN") {
    err_code = 403; err_msg = "permission_denied";
    return false;
  }

  std::lock_guard<std::mutex> lk(mu_);
  std::ostringstream oss;
  for (auto& kv : users_) {
    oss << kv.second.username << " " << kv.second.role << "\n";
  }
  out_body = oss.str();
  return true;
}
