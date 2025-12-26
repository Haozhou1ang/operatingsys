#include "storage/admin_fs.h"

#include "auth/auth.h"
#include "storage/storage.h"
#include "FS.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <shared_mutex>
#include <sstream>

using std::string;

static bool IsValidSnapshotName(const string& s) {
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

static void SetTokenError(TokenState st, int& err_code, string& err_msg) {
  err_code = (st == TokenState::EXPIRED ? 403 : (st == TokenState::NOT_FOUND ? 404 : 400));
  err_msg  = (st == TokenState::EXPIRED ? "token_expired" :
             (st == TokenState::NOT_FOUND ? "token_not_found" : "empty_token"));
}

static void SetFsError(fs::ErrorCode ec, int& err_code, string& err_msg) {
  switch (ec) {
    case fs::ErrorCode::E_NOT_FOUND:
      err_code = 404; err_msg = "not_found"; return;
    case fs::ErrorCode::E_ALREADY_EXISTS:
      err_code = 409; err_msg = "already_exists"; return;
    case fs::ErrorCode::E_NOT_DIR:
      err_code = 400; err_msg = "not_directory"; return;
    case fs::ErrorCode::E_IS_DIR:
      err_code = 400; err_msg = "is_directory"; return;
    case fs::ErrorCode::E_INVALID_PATH:
      err_code = 400; err_msg = "invalid_path"; return;
    case fs::ErrorCode::E_NAME_TOO_LONG:
      err_code = 400; err_msg = "name_too_long"; return;
    case fs::ErrorCode::E_NO_SPACE:
      err_code = 507; err_msg = "no_space"; return;
    case fs::ErrorCode::E_NO_INODE:
      err_code = 507; err_msg = "no_inode"; return;
    case fs::ErrorCode::E_FILE_TOO_LARGE:
      err_code = 413; err_msg = "file_too_large"; return;
    case fs::ErrorCode::E_PERMISSION:
      err_code = 403; err_msg = "permission_denied"; return;
    case fs::ErrorCode::E_SNAPSHOT_NOT_FOUND:
      err_code = 404; err_msg = "snapshot_not_found"; return;
    case fs::ErrorCode::E_SNAPSHOT_EXISTS:
      err_code = 409; err_msg = "snapshot_exists"; return;
    case fs::ErrorCode::E_MAX_SNAPSHOTS:
      err_code = 507; err_msg = "snapshot_limit"; return;
    default:
      err_code = 500; err_msg = "fs_error"; return;
  }
}

static bool CheckAdmin(AuthManager* auth, const string& token, SessionInfo& s,
                       int& err_code, string& err_msg) {
  if (!auth) { err_code = 500; err_msg = "auth_unavailable"; return false; }
  auto st = auth->CheckToken(token, &s);
  if (st != TokenState::OK) {
    SetTokenError(st, err_code, err_msg);
    return false;
  }
  if (s.role != "ADMIN") {
    err_code = 403; err_msg = "permission_denied";
    return false;
  }
  return true;
}

static string JoinPath(const string& root, const string& path) {
  if (path.empty()) return "";
  if (!path.empty() && path[0] == '/') return path;
  if (root.empty()) return path;
  if (root.back() == '/') return root + path;
  return root + "/" + path;
}

AdminFsService::AdminFsService(AuthManager* auth) : auth_(auth) {}

string AdminFsService::NormalizePath(const string& path) const {
  if (!auth_) return path;
  return JoinPath(auth_->GetVfsRoot(), path);
}

bool AdminFsService::Ls(const string& token, const string& path,
                        string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;
  if (path.empty()) { err_code = 400; err_msg = "empty_path"; return false; }

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }
  string p = NormalizePath(path);

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  if (!vfs->exists(p)) { err_code = 404; err_msg = "not_found"; return false; }
  if (!vfs->isDirectory(p)) { err_code = 400; err_msg = "not_directory"; return false; }

  auto r = vfs->readdir(p);
  if (!r.ok()) { SetFsError(r.error(), err_code, err_msg); return false; }

  auto entries = r.value();
  std::sort(entries.begin(), entries.end(),
            [](const fs::DirEntry& a, const fs::DirEntry& b) {
              return a.getName() < b.getName();
            });

  std::ostringstream oss;
  for (const auto& e : entries) {
    string name = e.getName();
    if (name.empty()) continue;
    char type = '?';
    if (e.file_type == fs::FileType::DIRECTORY) type = 'd';
    else if (e.file_type == fs::FileType::REGULAR) type = 'f';
    else if (e.file_type == fs::FileType::SYMLINK) type = 'l';
    oss << type << " " << name << "\n";
  }
  out_body = oss.str();
  return true;
}

bool AdminFsService::Read(const string& token, const string& path,
                          string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;
  if (path.empty()) { err_code = 400; err_msg = "empty_path"; return false; }

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }
  string p = NormalizePath(path);

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  if (!vfs->exists(p)) { err_code = 404; err_msg = "not_found"; return false; }
  if (!vfs->isFile(p)) { err_code = 400; err_msg = "not_file"; return false; }

  auto r = vfs->readFileAsString(p);
  if (!r.ok()) { SetFsError(r.error(), err_code, err_msg); return false; }
  out_body = r.value();
  return true;
}

bool AdminFsService::Write(const string& token, const string& path, const string& content,
                           int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;
  if (path.empty()) { err_code = 400; err_msg = "empty_path"; return false; }

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }
  string p = NormalizePath(path);

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  if (vfs->exists(p) && vfs->isDirectory(p)) {
    err_code = 400; err_msg = "is_directory"; return false;
  }
  if (!vfs->exists(p)) {
    auto err = vfs->create(p);
    if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  }
  auto err = vfs->truncate(p, 0);
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  auto w = vfs->writeFile(p, content, 0);
  if (!w.ok()) { SetFsError(w.error(), err_code, err_msg); return false; }
  err = vfs->sync();
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  return true;
}

bool AdminFsService::Mkdir(const string& token, const string& path,
                           int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;
  if (path.empty()) { err_code = 400; err_msg = "empty_path"; return false; }

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }
  string p = NormalizePath(path);

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  auto err = vfs->mkdir(p);
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  err = vfs->sync();
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  return true;
}

bool AdminFsService::BackupCreate(const string& token, const string& name,
                                  string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  string snap = name;
  if (snap.empty()) {
    snap = "snap_" + std::to_string(static_cast<long long>(std::time(nullptr)));
  }
  if (!IsValidSnapshotName(snap)) {
    err_code = 400; err_msg = "invalid_snapshot_name"; return false;
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  auto err = vfs->createSnapshot(snap);
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }

  out_body = "name=" + snap + "\n";
  return true;
}

bool AdminFsService::BackupList(const string& token,
                                string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  auto snaps = vfs->listSnapshots();
  std::ostringstream oss;
  for (const auto& it : snaps) {
    if (!it.valid) continue;
    oss << it.name << "\t" << it.create_time << "\t"
        << it.block_count << "\t" << (it.valid ? "1" : "0") << "\n";
  }
  out_body = oss.str();
  return true;
}

bool AdminFsService::BackupRestore(const string& token, const string& name,
                                   int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;
  if (name.empty()) { err_code = 400; err_msg = "empty_name"; return false; }

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  auto err = vfs->restoreSnapshot(name);
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  err = vfs->sync();
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  return true;
}

bool AdminFsService::SystemStatus(const string& token,
                                  string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  auto info = vfs->getInfo();
  const auto& cs = info.cache_stats;

  std::ostringstream oss;
  oss << "mounted=" << (info.mounted ? "1" : "0") << "\n";
  oss << "mount_path=" << info.mount_path << "\n";
  oss << "block_size=" << info.block_size << "\n";
  oss << "total_blocks=" << info.total_blocks << "\n";
  oss << "used_blocks=" << info.used_blocks << "\n";
  oss << "free_blocks=" << info.free_blocks << "\n";
  oss << "total_inodes=" << info.total_inodes << "\n";
  oss << "used_inodes=" << info.used_inodes << "\n";
  oss << "free_inodes=" << info.free_inodes << "\n";
  oss << "total_size=" << info.total_size << "\n";
  oss << "used_size=" << info.used_size << "\n";
  oss << "free_size=" << info.free_size << "\n";
  oss << "snapshot_count=" << info.snapshot_count << "\n";
  oss << "max_snapshots=" << info.max_snapshots << "\n";
  oss << "cache_hits=" << cs.hits << "\n";
  oss << "cache_misses=" << cs.misses << "\n";
  oss << "cache_evictions=" << cs.evictions << "\n";
  oss << "cache_capacity=" << cs.capacity << "\n";
  oss << "cache_size=" << cs.current_size << "\n";
  oss << "cache_hit_rate=" << std::fixed << std::setprecision(4) << cs.hit_rate << "\n";
  out_body = oss.str();
  return true;
}

bool AdminFsService::CacheStats(const string& token,
                                string& out_body, int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear(); out_body.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  auto cs = vfs->getCacheStats();
  std::ostringstream oss;
  oss << "hits=" << cs.hits << "\n";
  oss << "misses=" << cs.misses << "\n";
  oss << "evictions=" << cs.evictions << "\n";
  oss << "capacity=" << cs.capacity << "\n";
  oss << "current_size=" << cs.current_size << "\n";
  oss << "hit_rate=" << std::fixed << std::setprecision(4) << cs.hit_rate << "\n";
  out_body = oss.str();
  return true;
}

bool AdminFsService::CacheClear(const string& token,
                                int& err_code, string& err_msg) {
  err_code = 0; err_msg.clear();
  SessionInfo s;
  if (!CheckAdmin(auth_, token, s, err_code, err_msg)) return false;

  auto* vfs = auth_ ? auth_->GetVfs() : nullptr;
  if (!vfs) { err_code = 500; err_msg = "vfs_unavailable"; return false; }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  auto err = vfs->clearCache();
  if (err != fs::ErrorCode::OK) { SetFsError(err, err_code, err_msg); return false; }
  vfs->resetCacheStats();
  return true;
}
