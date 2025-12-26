// client/include/session/SessionStore
#pragma once
#include <string>

class SessionStore {
public:
  bool IsLoggedIn() const;
  const std::string& Token() const;
  const std::string& Role() const;

  void Set(const std::string& token, const std::string& role);
  void Clear();

private:
  std::string token_;
  std::string role_;
};
