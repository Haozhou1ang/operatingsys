// client/src/session/SessionStore
#include "session/SessionStore.h"

bool SessionStore::IsLoggedIn() const { return !token_.empty(); }
const std::string& SessionStore::Token() const { return token_; }
const std::string& SessionStore::Role() const { return role_; }
void SessionStore::Clear() { token_.clear(); role_.clear(); }
void SessionStore::Set(const std::string& token, const std::string& role) {
  token_ = token;
  role_ = role;
}
