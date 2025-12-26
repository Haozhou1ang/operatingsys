//server/include/auth/auth.h
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>
#include <ctime>
#include <functional>  

namespace fs {
class FileSystem;
}
enum class TokenState {
  OK = 0,
  NOT_FOUND = 1,
  EXPIRED = 2,
  EMPTY = 3,
};

enum class PersistResult {
  OK = 0,
  WRITE_FAIL = 1,
  SYNC_FAIL = 2,
};

struct UserInfo {
  std::string username;
  std::string password;
  std::string role; // ADMIN/EDITOR/REVIEWER/AUTHOR
};

struct SessionInfo {
  std::string username;
  std::string token;
  std::time_t create_time = 0;
  std::string role;
};

class AuthManager {
public:
  explicit AuthManager(std::string root_dir, int token_ttl_sec = 3600,
                       fs::FileSystem* vfs = nullptr,
                       std::string vfs_root = "/paper_system");

  // Init/load from disk if exists, else create default users in memory and persist
  bool Init(bool fresh_root = false);

  // Token check required by spec
  TokenState CheckToken(const std::string& token, SessionInfo* out_session = nullptr);

  // Commands (Part1)
  // LOGIN <username> <password>
  // Return: OK body: token=...\nrole=...\n
  bool Login(const std::string& username, const std::string& password,
             std::string& out_role, std::string& out_token,
             int& err_code, std::string& err_msg);

  // LOGOUT <token>
  bool Logout(const std::string& token, int& err_code, std::string& err_msg);

  // USER_ADD <token> <username> <password> <role>
  bool UserAdd(const std::string& token, const std::string& username,
               const std::string& password, const std::string& role,
               int& err_code, std::string& err_msg);

  // USER_DEL <token> <username>
  // NOTE: unfinished-task checks are done in PaperService via callbacks.
  bool UserDel(
  const std::string& token,
  const std::string& username,
  int& err_code,
  std::string& err_msg,
  const std::function<bool(const std::string& username,
                           const std::string& role,
                           std::string& why)>& unfinished_check);

  // USER_LIST <token>
  bool UserList(const std::string& token,
                std::string& out_body,
                int& err_code, std::string& err_msg);

  // For PaperService role lookup / username by token
  bool GetSessionByToken(const std::string& token, SessionInfo& out);

  // For PaperService to verify user role existence
  bool UserExists(const std::string& username, UserInfo* out = nullptr);

  fs::FileSystem* GetVfs() const { return vfs_; }
  const std::string& GetVfsRoot() const { return vfs_root_; }

private:
  std::string root_;
  std::string users_path_;
  int ttl_sec_;
  fs::FileSystem* vfs_ = nullptr;
  std::string vfs_root_;

  std::mutex mu_;
  std::unordered_map<std::string, UserInfo> users_;      // username -> user
  std::unordered_map<std::string, SessionInfo> sessions_; // token -> session
  std::unordered_map<std::string, std::string> user2token_; // username -> token

  // disk
  bool LoadUsersLocked();
  PersistResult SaveUsersLocked();

  static bool IsValidRole(const std::string& r);
  static std::string GenToken();
};
