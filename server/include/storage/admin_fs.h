#pragma once

#include <string>

class AuthManager;

class AdminFsService {
public:
  explicit AdminFsService(AuthManager* auth);

  bool Ls(const std::string& token, const std::string& path,
          std::string& out_body, int& err_code, std::string& err_msg);

  bool Read(const std::string& token, const std::string& path,
            std::string& out_body, int& err_code, std::string& err_msg);

  bool Write(const std::string& token, const std::string& path, const std::string& content,
             int& err_code, std::string& err_msg);

  bool Mkdir(const std::string& token, const std::string& path,
             int& err_code, std::string& err_msg);

  bool BackupCreate(const std::string& token, const std::string& name,
                    std::string& out_body, int& err_code, std::string& err_msg);

  bool BackupList(const std::string& token,
                  std::string& out_body, int& err_code, std::string& err_msg);

  bool BackupRestore(const std::string& token, const std::string& name,
                     int& err_code, std::string& err_msg);

  bool SystemStatus(const std::string& token,
                    std::string& out_body, int& err_code, std::string& err_msg);

  bool CacheStats(const std::string& token,
                  std::string& out_body, int& err_code, std::string& err_msg);

  bool CacheClear(const std::string& token,
                  int& err_code, std::string& err_msg);

private:
  AuthManager* auth_;

  std::string NormalizePath(const std::string& path) const;
};
