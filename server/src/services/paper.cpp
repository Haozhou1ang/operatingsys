// server/src/services/paper.cpp
#include "services/paper.h"
#include "auth/auth.h"
#include "storage/storage.h"
#include "FS.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

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

static bool IsValidVersionName(const std::string& v) {
  // Only allow "v" + digits (e.g. v1, v12).
  if (v.size() < 2 || v[0] != 'v') return false;
  for (size_t i = 1; i < v.size(); ++i) {
    unsigned char c = (unsigned char)v[i];
    if (c < '0' || c > '9') return false;
  }
  return true;
}

std::shared_mutex g_fs_mu;

static string Trim(string s) {
  while (!s.empty() && (s.back()=='\r' || s.back()=='\n' || s.back()==' ' || s.back()=='\t')) s.pop_back();
  size_t i=0;
  while (i<s.size() && (s[i]==' '||s[i]=='\t')) i++;
  return s.substr(i);
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

static PaperPersistResult WriteTextFileLocked(const string& path, const string& content) {
  ofstream fout(path, ios::trunc);
  if (!fout.is_open()) return PaperPersistResult::WRITE_FAIL;
  fout << content;
  return fout.good() ? PaperPersistResult::OK : PaperPersistResult::WRITE_FAIL;
}

static bool ReadTextFileLocked(const string& path, string& out) {
  ifstream fin(path);
  if (!fin.is_open()) return false;
  ostringstream oss;
  oss << fin.rdbuf();
  out = oss.str();
  return true;
}

static bool RemoveLocalFileIfExistsLocked(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  ec.clear();
  return std::filesystem::remove(path, ec);
}

static bool RemoveLocalDirIfExistsLocked(const std::string& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;
  ec.clear();
  return std::filesystem::remove(path, ec); // only removes empty directories
}

static fs::FileSystem* GetVfs(AuthManager* auth) {
  return auth ? auth->GetVfs() : nullptr;
}

static PaperPersistResult VfsWriteTextFileLocked(fs::FileSystem* vfs, const string& path, const string& content) {
  if (!vfs) return PaperPersistResult::WRITE_FAIL;
  if (!vfs->exists(path)) {
    if (vfs->create(path) != fs::ErrorCode::OK) return PaperPersistResult::WRITE_FAIL;
  }
  if (vfs->truncate(path, 0) != fs::ErrorCode::OK) return PaperPersistResult::WRITE_FAIL;
  auto w = vfs->writeFile(path, content, 0);
  if (!w.ok()) return PaperPersistResult::WRITE_FAIL;
  return vfs->sync() == fs::ErrorCode::OK
           ? PaperPersistResult::OK
           : PaperPersistResult::SYNC_FAIL;
}

static bool VfsReadTextFileLocked(fs::FileSystem* vfs, const string& path, string& out) {
  if (!vfs) return false;
  auto r = vfs->readFileAsString(path);
  if (!r.ok()) return false;
  out = r.value();
  return true;
}

static bool VfsMkdirpLocked(fs::FileSystem* vfs, const string& path) {
  if (!vfs) return false;
  return vfs->mkdirp(path) == fs::ErrorCode::OK;
}

static PaperPersistResult VfsRemoveFileIfExistsLocked(fs::FileSystem* vfs, const string& path) {
  if (!vfs) return PaperPersistResult::WRITE_FAIL;
  if (!vfs->exists(path)) return PaperPersistResult::OK;
  if (vfs->unlink(path) != fs::ErrorCode::OK) return PaperPersistResult::WRITE_FAIL;
  return vfs->sync() == fs::ErrorCode::OK
           ? PaperPersistResult::OK
           : PaperPersistResult::SYNC_FAIL;
}

static PaperPersistResult RemoveVfsPathIfExistsLocked(fs::FileSystem* vfs, const std::string& path) {
  if (!vfs) return PaperPersistResult::WRITE_FAIL;
  if (!vfs->exists(path)) return PaperPersistResult::OK;
  auto err = vfs->remove(path);
  if (err != fs::ErrorCode::OK) return PaperPersistResult::WRITE_FAIL;
  return vfs->sync() == fs::ErrorCode::OK
           ? PaperPersistResult::OK
           : PaperPersistResult::SYNC_FAIL;
}

PaperService::PaperService(std::string root_dir, AuthManager* auth)
  : root_(std::move(root_dir)), auth_(auth) {
  meta_path_ = root_ + "/meta.csv";
  papers_dir_ = root_ + "/papers";
}

bool PaperService::Init(bool fresh_root) {
  // strict: all resources locked
  std::unique_lock<std::shared_mutex> fs(g_fs_mu);

  if (auto* vfs = GetVfs(auth_)) {
    if (fresh_root) {
      if (!VfsMkdirpLocked(vfs, papers_dir_)) return false;
      meta_.clear();
      if (SaveMeta() != PaperPersistResult::OK) return false;
      return true;
    }

    if (!vfs->exists(papers_dir_)) return false;
    if (!LoadMeta()) return false;
    return true;
  }

  if (fresh_root) {
    std::filesystem::create_directories(papers_dir_);
    meta_.clear();
    return SaveMeta() == PaperPersistResult::OK;
  }

  if (!std::filesystem::exists(papers_dir_)) return false;
  if (!LoadMeta()) return false;
  return true;
}

string PaperService::StatusToString(PaperStatus s) {
  switch (s) {
    case PaperStatus::SUBMITTED:     return "SUBMITTED";
    case PaperStatus::UNDER_REVIEW:  return "UNDER_REVIEW";
    case PaperStatus::FINISH_REVIEW: return "FINISH_REVIEW";
    case PaperStatus::ACCEPTED:      return "ACCEPTED";
    case PaperStatus::REJECTED:      return "REJECTED";
  }
  return "SUBMITTED";
}

bool PaperService::StringToStatus(const string& s, PaperStatus& out) {
  if (s=="SUBMITTED")     { out=PaperStatus::SUBMITTED; return true; }
  if (s=="UNDER_REVIEW")  { out=PaperStatus::UNDER_REVIEW; return true; }
  if (s=="FINISH_REVIEW") { out=PaperStatus::FINISH_REVIEW; return true; }
  if (s=="ACCEPTED")      { out=PaperStatus::ACCEPTED; return true; }
  if (s=="REJECTED")      { out=PaperStatus::REJECTED; return true; }
  return false;
}

// NOTE: callers should hold g_fs_mu if you want “all resources locked” strictly.
// We still lock mu_ to protect in-memory meta_.
bool PaperService::LoadMeta() {
  lock_guard<mutex> lk(mu_);
  meta_.clear();

  string content;
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsReadTextFileLocked(vfs, meta_path_, content)) return false;
  } else {
    ifstream fin(meta_path_);
    if (!fin.is_open()) return false;
    ostringstream oss;
    oss << fin.rdbuf();
    content = oss.str();
  }

  istringstream iss(content);
  string line;
  while (getline(iss, line)) {
    line = Trim(line);
    if (line.empty()) continue;

    auto cols = SplitCSVLine(line);
    if (cols.size() != 5) continue;

    PaperMeta m;
    m.paper_id = Trim(cols[0]);
    m.author   = Trim(cols[1]);
    PaperStatus st;
    if (!StringToStatus(Trim(cols[2]), st)) continue;
    m.status = st;
    m.reviewers = SplitSemi(Trim(cols[3]));
    m.current_version = Trim(cols[4]);

    // Harden against path traversal/invalid names from CSV.
    if (!IsValidSimpleName(m.paper_id)) continue;
    if (!IsValidSimpleName(m.author)) continue;
    if (!IsValidVersionName(m.current_version)) continue;
    bool reviewers_ok = true;
    for (const auto& rv : m.reviewers) {
      if (!IsValidSimpleName(rv)) { reviewers_ok = false; break; }
    }
    if (!reviewers_ok) continue;
    meta_[m.paper_id] = m;
  }
  return true;
}

PaperPersistResult PaperService::SaveMeta() {
  lock_guard<mutex> lk(mu_);
  return SaveMetaLocked();
}

PaperPersistResult PaperService::SaveMetaLocked() {
  ostringstream fout;

  for (auto& kv : meta_) {
    auto& m = kv.second;
    fout << m.paper_id << ","
         << m.author << ","
         << StatusToString(m.status) << ","
         << JoinSemi(m.reviewers) << ","
         << m.current_version << "\n";
  }
  if (auto* vfs = GetVfs(auth_)) {
    return VfsWriteTextFileLocked(vfs, meta_path_, fout.str());
  }
  return WriteTextFileLocked(meta_path_, fout.str());
}

vector<string> PaperService::SplitSemi(const string& s) {
  vector<string> out;
  string cur;
  for (char c : s) {
    if (c == ';') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  if (!cur.empty()) out.push_back(cur);

  for (auto& x : out) x = Trim(x);
  out.erase(remove_if(out.begin(), out.end(),
                      [](const string& x){ return x.empty(); }),
            out.end());
  return out;
}

string PaperService::JoinSemi(const vector<string>& v) {
  string out;
  for (size_t i=0;i<v.size();++i) {
    if (i) out.push_back(';');
    out += v[i];
  }
  return out;
}

bool PaperService::Has(const vector<string>& v, const string& x) {
  return find(v.begin(), v.end(), x) != v.end();
}

void PaperService::AddUnique(vector<string>& v, const string& x) {
  if (!Has(v, x)) v.push_back(x);
}

int PaperService::ParseVersionNum(const string& v) {
  if (v.size() < 2 || v[0] != 'v') return 0;
  try { return stoi(v.substr(1)); } catch (...) { return 0; }
}

string PaperService::MakeVersion(int n) {
  return "v" + to_string(n);
}

string PaperService::PaperRoot(const string& paper_id) const {
  return papers_dir_ + "/" + paper_id;
}
string PaperService::VersionsRoot(const string& paper_id) const {
  return PaperRoot(paper_id) + "/versions";
}
string PaperService::VersionDir(const string& paper_id, const string& v) const {
  return VersionsRoot(paper_id) + "/" + v;
}
string PaperService::PaperFile(const string& paper_id, const string& v) const {
  return VersionDir(paper_id, v) + "/" + v + ".txt";
}
string PaperService::ReviewsDir(const string& paper_id, const string& v) const {
  return VersionDir(paper_id, v) + "/reviews";
}

void PaperService::CleanupPaperVersionLocked(const std::string& paper_id, const std::string& version) {
  const std::string paper_file = PaperFile(paper_id, version);
  const std::string reviews_dir = ReviewsDir(paper_id, version);
  const std::string version_dir = VersionDir(paper_id, version);
  const std::string versions_root = VersionsRoot(paper_id);
  const std::string paper_root = PaperRoot(paper_id);

  if (auto* vfs = GetVfs(auth_)) {
    // Best-effort cleanup (ignore failures).
    (void)RemoveVfsPathIfExistsLocked(vfs, paper_file);
    (void)RemoveVfsPathIfExistsLocked(vfs, reviews_dir);
    (void)RemoveVfsPathIfExistsLocked(vfs, version_dir);
    (void)RemoveVfsPathIfExistsLocked(vfs, versions_root);
    (void)RemoveVfsPathIfExistsLocked(vfs, paper_root);
    return;
  }

  (void)RemoveLocalFileIfExistsLocked(paper_file);
  (void)RemoveLocalDirIfExistsLocked(reviews_dir);
  (void)RemoveLocalDirIfExistsLocked(version_dir);
  (void)RemoveLocalDirIfExistsLocked(versions_root);
  (void)RemoveLocalDirIfExistsLocked(paper_root);
}

bool PaperService::PaperExistsLocked(const string& paper_id) {
  lock_guard<mutex> lk(mu_);
  return meta_.find(paper_id) != meta_.end();
}

bool PaperService::EnsurePaperDirsLocked(const string& paper_id) {
  if (auto* vfs = GetVfs(auth_)) {
    return VfsMkdirpLocked(vfs, VersionsRoot(paper_id));
  }
  std::error_code ec;
  std::filesystem::create_directories(VersionsRoot(paper_id), ec);
  return !ec;
}

bool PaperService::AllReviewsDoneLocked(const PaperMeta& m) {
  // caller should hold g_fs_mu
  if (m.reviewers.empty()) return false;
  const string& v = m.current_version;
  string rdir = ReviewsDir(m.paper_id, v);

  for (const auto& rv : m.reviewers) {
    string f = rdir + "/" + rv + ".txt";
    if (auto* vfs = GetVfs(auth_)) {
      if (!vfs->exists(f)) return false;
    } else {
      if (!std::filesystem::exists(f)) return false;
    }
  }
  return true;
}

bool PaperService::Upload(const string& token, const string& paper_id, const string& content,
                          int& err_code, string& err_msg) {
  err_code=0; err_msg.clear();
  if (token.empty() || paper_id.empty() || content.empty()) {
    err_code=400; err_msg="empty_fields";
    return false;
  }

  // NEW: paper_id allow [A-Za-z0-9_-]
  if (!IsValidSimpleName(paper_id)) {
    err_code = 400;
    err_msg = "invalid_paper_id";
    return false;
  }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "AUTHOR") {
    err_code=403; err_msg="permission_denied";
    return false;
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  if (meta_.find(paper_id) != meta_.end()) {
    err_code=409; err_msg="paper_exists";
    return false;
  }

  if (!EnsurePaperDirsLocked(paper_id)) {
    err_code = 500;
    err_msg = "mkdir_failed";
    return false;
  }
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsMkdirpLocked(vfs, VersionDir(paper_id, "v1"))) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    if (!VfsMkdirpLocked(vfs, ReviewsDir(paper_id, "v1"))) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    auto pr = VfsWriteTextFileLocked(vfs, PaperFile(paper_id, "v1"), content);
    if (pr != PaperPersistResult::OK) {
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "write_failed");
      return false;
    }
  } else {
    std::error_code ec;
    std::filesystem::create_directories(VersionDir(paper_id, "v1"), ec);
    if (ec) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    ec.clear();
    std::filesystem::create_directories(ReviewsDir(paper_id, "v1"), ec);
    if (ec) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    if (WriteTextFileLocked(PaperFile(paper_id, "v1"), content) != PaperPersistResult::OK) {
      err_code = 500; err_msg = "write_failed";
      return false;
    }
  }

  PaperMeta m;
  m.paper_id = paper_id;
  m.author = s.username;
  m.status = PaperStatus::SUBMITTED;
  m.reviewers.clear();
  m.current_version = "v1";
  meta_[paper_id] = m;

  {
    auto pr = SaveMetaLocked();
    if (pr != PaperPersistResult::OK) {
      meta_.erase(paper_id); // rollback in-memory state
      CleanupPaperVersionLocked(paper_id, "v1"); // best-effort cleanup of orphan files
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
      return false;
    }
  }
  return true;
}


bool PaperService::Revise(const string& token, const string& paper_id, const string& content,
                          int& err_code, string& err_msg) {
  err_code=0; err_msg.clear();
  if (token.empty() || paper_id.empty() || content.empty()) {
    err_code=400; err_msg="empty_fields";
    return false;
  }
  if (!IsValidSimpleName(paper_id)) {
    err_code = 400;
    err_msg = "invalid_paper_id";
    return false;
  }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "AUTHOR") {
    err_code=403; err_msg="permission_denied";
    return false;
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  auto it = meta_.find(paper_id);
  if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
  if (it->second.author != s.username) { err_code=403; err_msg="not_your_paper"; return false; }

  // Spec: ACCEPTED forbids REVISE
  if (it->second.status == PaperStatus::ACCEPTED) {
    err_code=400; err_msg="paper_accepted_no_revise"; return false;
  }
  // Spec: UNDER_REVIEW or FINISH_REVIEW forbids REVISE
  if (it->second.status == PaperStatus::UNDER_REVIEW || it->second.status == PaperStatus::FINISH_REVIEW) {
    err_code=400; err_msg="paper_in_review_no_revise"; return false;
  }

  if (it->second.status == PaperStatus::SUBMITTED) {
    // overwrite current version
    const string v = it->second.current_version;
    if (auto* vfs = GetVfs(auth_)) {
      (void)VfsMkdirpLocked(vfs, VersionDir(paper_id, v));
      (void)VfsMkdirpLocked(vfs, ReviewsDir(paper_id, v));
      auto pr = VfsWriteTextFileLocked(vfs, PaperFile(paper_id, v), content);
      if (pr != PaperPersistResult::OK) {
        err_code = 500;
        err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "write_failed");
        return false;
      }
    } else {
      std::filesystem::create_directories(VersionDir(paper_id, v));
      std::filesystem::create_directories(ReviewsDir(paper_id, v));
      if (WriteTextFileLocked(PaperFile(paper_id, v), content) != PaperPersistResult::OK) {
        err_code=500; err_msg="write_failed"; return false;
      }
    }
    return true;
  }

  // REJECTED => new version vn+1, clear reviewers, status=SUBMITTED
  if (it->second.status != PaperStatus::REJECTED) {
    // defensive (should not happen)
    err_code=400; err_msg="invalid_state_for_revise";
    return false;
  }

  int cur = ParseVersionNum(it->second.current_version);
  string nv = MakeVersion(cur + 1);

  // Write new version first; only update meta_ after all I/O succeeds.
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsMkdirpLocked(vfs, VersionDir(paper_id, nv))) { err_code=500; err_msg="mkdir_failed"; return false; }
    if (!VfsMkdirpLocked(vfs, ReviewsDir(paper_id, nv))) { err_code=500; err_msg="mkdir_failed"; return false; }
    auto pr = VfsWriteTextFileLocked(vfs, PaperFile(paper_id, nv), content);
    if (pr != PaperPersistResult::OK) {
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "write_failed");
      return false;
    }
  } else {
    std::error_code ec;
    std::filesystem::create_directories(VersionDir(paper_id, nv), ec);
    if (ec) { err_code=500; err_msg="mkdir_failed"; return false; }
    ec.clear();
    std::filesystem::create_directories(ReviewsDir(paper_id, nv), ec);
    if (ec) { err_code=500; err_msg="mkdir_failed"; return false; }
    if (WriteTextFileLocked(PaperFile(paper_id, nv), content) != PaperPersistResult::OK) {
      err_code=500; err_msg="write_failed"; return false;
    }
  }

  const PaperMeta old_meta = it->second;
  it->second.current_version = nv;
  it->second.reviewers.clear();
  it->second.status = PaperStatus::SUBMITTED;

  // persist meta
  {
    auto pr = SaveMetaLocked();
    if (pr != PaperPersistResult::OK) {
      it->second = old_meta; // rollback in-memory state
      CleanupPaperVersionLocked(paper_id, nv); // avoid orphan version files
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
      return false;
    }
  }
  return true;
}

bool PaperService::Status(const string& token, const string& paper_id,
                          string& out_body, int& err_code, string& err_msg) {
  err_code=0; err_msg.clear(); out_body.clear();
  if (token.empty() || paper_id.empty()) { err_code=400; err_msg="empty_fields"; return false; }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  PaperMeta m;
  {
    lock_guard<mutex> lk(mu_);
    auto it = meta_.find(paper_id);
    if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
    m = it->second;
  }
  // Access control:
  // - AUTHOR: can only see own papers
  // - REVIEWER: can only see assigned papers
  // - EDITOR/ADMIN: can see all
  if (s.role == "AUTHOR") {
    if (m.author != s.username) { err_code = 403; err_msg = "permission_denied"; return false; }
  } else if (s.role == "REVIEWER") {
    if (!Has(m.reviewers, s.username)) { err_code = 403; err_msg = "permission_denied"; return false; }
  }

  ostringstream oss;
  oss << "paper_id=" << m.paper_id << "\n";
  oss << "author=" << m.author << "\n";
  oss << "status=" << StatusToString(m.status) << "\n";
  oss << "reviewers=" << JoinSemi(m.reviewers) << "\n";
  oss << "current_version=" << m.current_version << "\n";
  out_body = oss.str();
  return true;
}

bool PaperService::ReviewsGet(const string& token, const string& paper_id,
                             string& out_body, int& err_code, string& err_msg) {
  err_code=0; err_msg.clear(); out_body.clear();
  if (token.empty() || paper_id.empty()) { err_code=400; err_msg="empty_fields"; return false; }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "AUTHOR") { err_code=403; err_msg="permission_denied"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  PaperMeta m;
  {
    lock_guard<mutex> lk(mu_);
    auto it = meta_.find(paper_id);
    if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
    m = it->second;
  }
  if (m.author != s.username) { err_code=403; err_msg="not_your_paper"; return false; }

  string rdir = ReviewsDir(paper_id, m.current_version);
  ostringstream oss;
  if (auto* vfs = GetVfs(auth_)) {
    if (!vfs->exists(rdir)) { out_body=""; return true; }
    auto entries = vfs->readdir(rdir);
    if (!entries.ok()) { out_body=""; return true; }
    vector<string> names;
    for (const auto& e : entries.value()) {
      const string name = e.getName();
      if (name == "." || name == "..") continue;
      if (e.file_type != fs::FileType::REGULAR) continue;
      names.push_back(name);
    }
    sort(names.begin(), names.end());
    for (const auto& name : names) {
      const string path = rdir + "/" + name;
      auto r = vfs->readFileAsString(path);
      if (!r.ok()) continue;
      oss << "----- " << name << " -----\n";
      oss << r.value();
      if (!r.value().empty() && r.value().back()!='\n') oss << "\n";
    }
  } else {
    if (!std::filesystem::exists(rdir)) { out_body=""; return true; }
    std::vector<std::filesystem::path> files;
    for (auto& ent : std::filesystem::directory_iterator(rdir)) {
      if (!ent.is_regular_file()) continue;
      files.push_back(ent.path());
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) {
                return a.filename().string() < b.filename().string();
              });
    for (const auto& p : files) {
      string path = p.string();
      string name = p.filename().string();
      string content;
      if (ReadTextFileLocked(path, content)) {
        oss << "----- " << name << " -----\n";
        oss << content;
        if (!content.empty() && content.back()!='\n') oss << "\n";
      }
    }
  }
  out_body = oss.str();
  return true;
}


bool PaperService::Papers(const string& token,
                          string& out_body, int& err_code, string& err_msg) {
  err_code = 0;
  err_msg.clear();
  out_body.clear();

  if (token.empty()) {
    err_code = 400;
    err_msg = "empty_token";
    return false;
  }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st == TokenState::EXPIRED ? 403 :
               (st == TokenState::NOT_FOUND ? 404 : 400));
    err_msg  = (st == TokenState::EXPIRED ? "token_expired" :
               (st == TokenState::NOT_FOUND ? "token_not_found" : "empty_token"));
    return false;
  }

  if (s.role != "AUTHOR") {
    err_code = 403;
    err_msg = "permission_denied";
    return false;
  }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  std::ostringstream oss;
  for (const auto& kv : meta_) {
    const auto& m = kv.second;
    if (m.author != s.username) continue;

    oss << m.paper_id << " " << StatusToString(m.status) << "\n";
  }

  out_body = oss.str();
  return true;
}


bool PaperService::Download(const string& token, const string& paper_id,
                            string& out_body, int& err_code, string& err_msg) {
  err_code=0; err_msg.clear(); out_body.clear();
  if (token.empty() || paper_id.empty()) { err_code=400; err_msg="empty_fields"; return false; }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "REVIEWER") { err_code=403; err_msg="permission_denied"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  PaperMeta m;
  {
    lock_guard<mutex> lk(mu_);
    auto it = meta_.find(paper_id);
    if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
    m = it->second;
  }
  if (!Has(m.reviewers, s.username)) { err_code=403; err_msg="not_assigned"; return false; }

  string path = PaperFile(paper_id, m.current_version);
  string content;
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsReadTextFileLocked(vfs, path, content)) { err_code=500; err_msg="read_failed"; return false; }
  } else {
    if (!ReadTextFileLocked(path, content)) { err_code=500; err_msg="read_failed"; return false; }
  }
  out_body = content;
  return true;
}

bool PaperService::ReviewsGive(const string& token, const string& paper_id, const string& review_content,
                               int& err_code, string& err_msg) {
  err_code=0; err_msg.clear();
  if (token.empty() || paper_id.empty() || review_content.empty()) {
    err_code=400; err_msg="empty_fields"; return false;
  }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "REVIEWER") { err_code=403; err_msg="permission_denied"; return false; }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  auto it = meta_.find(paper_id);
  if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }

  // Spec: ACCEPTED forbids REVIEWS_GIVE
  if (it->second.status == PaperStatus::ACCEPTED) { err_code=400; err_msg="paper_accepted_no_review"; return false; }

  if (!Has(it->second.reviewers, s.username)) { err_code=403; err_msg="not_assigned"; return false; }
  if (it->second.status != PaperStatus::UNDER_REVIEW) { err_code=400; err_msg="paper_not_under_review"; return false; }

  const string v = it->second.current_version;
  const string rdir = ReviewsDir(paper_id, v);
  const string f = rdir + "/" + s.username + ".txt";
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsMkdirpLocked(vfs, rdir)) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    auto pr = VfsWriteTextFileLocked(vfs, f, review_content);
    if (pr != PaperPersistResult::OK) {
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "write_failed");
      return false;
    }
  } else {
    std::error_code ec;
    std::filesystem::create_directories(rdir, ec);
    if (ec) { err_code = 500; err_msg = "mkdir_failed"; return false; }
    if (WriteTextFileLocked(f, review_content) != PaperPersistResult::OK) {
      err_code=500; err_msg="write_failed"; return false;
    }
  }

  // If all done => FINISH_REVIEW
  if (AllReviewsDoneLocked(it->second)) {
    const PaperStatus old_status = it->second.status;
    it->second.status = PaperStatus::FINISH_REVIEW;

    {
      auto pr = SaveMetaLocked();
      if (pr != PaperPersistResult::OK) {
        it->second.status = old_status; // rollback in-memory state
        err_code = 500;
        err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
        return false;
      }
    }
  }
  return true;
}

bool PaperService::Tasks(const string& token,
                         string& out_body, int& err_code, string& err_msg) {
  err_code=0; err_msg.clear(); out_body.clear();
  if (token.empty()) { err_code=400; err_msg="empty_token"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "REVIEWER") { err_code=403; err_msg="permission_denied"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  ostringstream oss;
  for (auto& kv : meta_) {
    const auto& m = kv.second;
    if (!Has(m.reviewers, s.username)) continue;

    bool done = false;
    string f = ReviewsDir(m.paper_id, m.current_version) + "/" + s.username + ".txt";
    if (auto* vfs = GetVfs(auth_)) {
      done = vfs->exists(f);
    } else {
      done = std::filesystem::exists(f);
    }
    oss << m.paper_id << " " << (done ? "DONE" : "PENDING") << "\n";
  }
  out_body = oss.str();
  return true;
}

bool PaperService::Assign(const string& token, const string& paper_id, const string& reviewer_username,
                          int& err_code, string& err_msg) {
  err_code=0; err_msg.clear();
  if (token.empty() || paper_id.empty() || reviewer_username.empty()) {
    err_code=400; err_msg="empty_fields"; return false;
  }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }
  if (!IsValidSimpleName(reviewer_username)) { err_code=400; err_msg="invalid_reviewer"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "EDITOR") { err_code=403; err_msg="permission_denied"; return false; }

  // reviewer must exist and role=REVIEWER
  UserInfo u;
  if (!auth_->UserExists(reviewer_username, &u) || u.role != "REVIEWER") {
    err_code=404; err_msg="reviewer_not_found";
    return false;
  }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  auto it = meta_.find(paper_id);
  if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }

  // Spec: ACCEPTED forbids ASSIGN
  if (it->second.status == PaperStatus::ACCEPTED) { err_code=400; err_msg="paper_accepted_no_assign"; return false; }
  // (optional strictness) REJECTED should be revised first
  if (it->second.status == PaperStatus::REJECTED) { err_code=400; err_msg="paper_rejected_need_revise"; return false; }

  if (Has(it->second.reviewers, reviewer_username)) {
    const PaperMeta old_meta = it->second;
    const string rdir = ReviewsDir(paper_id, it->second.current_version);
    const string f = rdir + "/" + reviewer_username + ".txt";

    bool had_old_review = false;
    std::string old_review_content;
    if (auto* vfs = GetVfs(auth_)) {
      if (vfs->exists(f)) {
        auto r = vfs->readFileAsString(f);
        if (r.ok()) { had_old_review = true; old_review_content = r.value(); }
      }
    } else {
      if (std::filesystem::exists(f)) {
        std::string tmp;
        if (ReadTextFileLocked(f, tmp)) { had_old_review = true; old_review_content = std::move(tmp); }
      }
    }

    if (auto* vfs = GetVfs(auth_)) {
      if (!VfsMkdirpLocked(vfs, rdir)) { err_code = 500; err_msg = "mkdir_failed"; return false; }
      auto pr = VfsRemoveFileIfExistsLocked(vfs, f);
      if (pr != PaperPersistResult::OK) {
        err_code = 500;
        err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "review_delete_failed");
        return false;
      }
    } else {
      std::error_code ec;
      std::filesystem::create_directories(rdir, ec);
      if (ec) { err_code = 500; err_msg = "mkdir_failed"; return false; }
      ec.clear();
      (void)std::filesystem::remove(f, ec);
      if (ec) {
        err_code = 500;
        err_msg = "review_delete_failed";
        return false;
      }
    }

    // Re-assign existing reviewer: delete their previous review (if any) and move back to UNDER_REVIEW.
    // This lets EDITOR invalidate an unreasonable review and require re-submission.
    if (it->second.status == PaperStatus::SUBMITTED || it->second.status == PaperStatus::FINISH_REVIEW) {
      it->second.status = PaperStatus::UNDER_REVIEW;
    }

    {
      auto pr = SaveMetaLocked();
      if (pr != PaperPersistResult::OK) {
        it->second = old_meta; // rollback in-memory state
        if (had_old_review) {
          // best-effort restore old review file
          if (auto* vfs = GetVfs(auth_)) {
            (void)VfsMkdirpLocked(vfs, rdir);
            (void)VfsWriteTextFileLocked(vfs, f, old_review_content);
          } else {
            std::error_code ec;
            std::filesystem::create_directories(rdir, ec);
            if (!ec) (void)WriteTextFileLocked(f, old_review_content);
          }
        }
        err_code = 500;
        err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
        return false;
      }
    }
    return true;
  }

  // If SUBMITTED or FINISH_REVIEW => set UNDER_REVIEW
  const PaperMeta old_meta = it->second;
  if (it->second.status == PaperStatus::SUBMITTED || it->second.status == PaperStatus::FINISH_REVIEW) {
    it->second.status = PaperStatus::UNDER_REVIEW;
  }

  it->second.reviewers.push_back(reviewer_username);

  // ensure dir exists
  if (auto* vfs = GetVfs(auth_)) {
    if (!VfsMkdirpLocked(vfs, ReviewsDir(paper_id, it->second.current_version))) { err_code = 500; err_msg = "mkdir_failed"; return false; }
  } else {
    std::error_code ec;
    std::filesystem::create_directories(ReviewsDir(paper_id, it->second.current_version), ec);
    if (ec) { err_code = 500; err_msg = "mkdir_failed"; return false; }
  }

  // persist meta
  {
    auto pr = SaveMetaLocked();
    if (pr != PaperPersistResult::OK) {
      it->second = old_meta; // rollback in-memory state
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
      return false;
    }
  }
  return true;
}

bool PaperService::Decide(const string& token, const string& paper_id, const string& decision,
                          int& err_code, string& err_msg) {
  err_code=0; err_msg.clear();
  if (token.empty() || paper_id.empty() || decision.empty()) {
    err_code=400; err_msg="empty_fields"; return false;
  }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "EDITOR") { err_code=403; err_msg="permission_denied"; return false; }

  string d = decision;
  if (d != "ACCEPT" && d != "REJECT") { err_code=400; err_msg="invalid_decision"; return false; }

  std::unique_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  auto it = meta_.find(paper_id);
  if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
  if (it->second.status != PaperStatus::FINISH_REVIEW) { err_code=400; err_msg="paper_not_finish_review"; return false; }

  const PaperStatus old_status = it->second.status;
  it->second.status = (d=="ACCEPT" ? PaperStatus::ACCEPTED : PaperStatus::REJECTED);

  // persist meta
  {
    auto pr = SaveMetaLocked();
    if (pr != PaperPersistResult::OK) {
      it->second.status = old_status; // rollback in-memory state
      err_code = 500;
      err_msg = (pr == PaperPersistResult::SYNC_FAIL ? "sync_failed" : "meta_write_failed");
      return false;
    }
  }
  return true;
}

bool PaperService::Reviews(const string& token, const string& paper_id,
                           string& out_body, int& err_code, string& err_msg) {
  err_code=0; err_msg.clear(); out_body.clear();
  if (token.empty() || paper_id.empty()) { err_code=400; err_msg="empty_fields"; return false; }
  if (!IsValidSimpleName(paper_id)) { err_code=400; err_msg="invalid_paper_id"; return false; }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st==TokenState::EXPIRED?403:(st==TokenState::NOT_FOUND?404:400));
    err_msg  = (st==TokenState::EXPIRED?"token_expired":(st==TokenState::NOT_FOUND?"token_not_found":"empty_token"));
    return false;
  }
  if (s.role != "EDITOR") { err_code=403; err_msg="permission_denied"; return false; }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  PaperMeta m;
  {
    lock_guard<mutex> lk(mu_);
    auto it = meta_.find(paper_id);
    if (it == meta_.end()) { err_code=404; err_msg="paper_not_found"; return false; }
    m = it->second;
  }

  string rdir = ReviewsDir(paper_id, m.current_version);
  ostringstream oss;
  if (auto* vfs = GetVfs(auth_)) {
    if (!vfs->exists(rdir)) { out_body=""; return true; }
    auto entries = vfs->readdir(rdir);
    if (!entries.ok()) { out_body=""; return true; }
    vector<string> names;
    for (const auto& e : entries.value()) {
      const string name = e.getName();
      if (name == "." || name == "..") continue;
      if (e.file_type != fs::FileType::REGULAR) continue;
      names.push_back(name);
    }
    sort(names.begin(), names.end());
    for (const auto& name : names) {
      const string path = rdir + "/" + name;
      auto r = vfs->readFileAsString(path);
      if (!r.ok()) continue;
      oss << "----- " << name << " -----\n";
      oss << r.value();
      if (!r.value().empty() && r.value().back()!='\n') oss << "\n";
    }
  } else {
    if (!std::filesystem::exists(rdir)) { out_body=""; return true; }
    for (auto& ent : std::filesystem::directory_iterator(rdir)) {
      if (!ent.is_regular_file()) continue;
      string path = ent.path().string();
      string name = ent.path().filename().string();
      string content;
      if (ReadTextFileLocked(path, content)) {
        oss << "----- " << name << " -----\n";
        oss << content;
        if (!content.empty() && content.back()!='\n') oss << "\n";
      }
    }
  }
  out_body = oss.str();
  return true;
}

bool PaperService::Queue(const string& token,
                         string& out_body, int& err_code, string& err_msg) {
  err_code = 0;
  err_msg.clear();
  out_body.clear();

  if (token.empty()) {
    err_code = 400;
    err_msg = "empty_token";
    return false;
  }

  SessionInfo s;
  auto st = auth_->CheckToken(token, &s);
  if (st != TokenState::OK) {
    err_code = (st == TokenState::EXPIRED ? 403 :
               (st == TokenState::NOT_FOUND ? 404 : 400));
    err_msg  = (st == TokenState::EXPIRED ? "token_expired" :
               (st == TokenState::NOT_FOUND ? "token_not_found" : "empty_token"));
    return false;
  }

  if (s.role != "EDITOR") {
    err_code = 403;
    err_msg = "permission_denied";
    return false;
  }

  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  std::ostringstream oss;
  for (const auto& kv : meta_) {
    const auto& m = kv.second;
    oss << m.paper_id << " " << StatusToString(m.status) << "\n";
  }

  out_body = oss.str();
  return true;
}


bool PaperService::UserHasNoUnfinishedTasks(const string& username, const string& role, string& why) {
  why.clear();

  // strict: lock all resources
  std::shared_lock<std::shared_mutex> fs(g_fs_mu);
  lock_guard<mutex> lk(mu_);

  if (role == "AUTHOR") {
    for (auto& kv : meta_) {
      const auto& m = kv.second;
      if (m.author != username) continue;
      if (!(m.status == PaperStatus::ACCEPTED || m.status == PaperStatus::REJECTED)) {
        why = "author_has_unfinished_paper:" + m.paper_id;
        return false;
      }
    }
    return true;
  }

  if (role == "REVIEWER") {
    for (auto& kv : meta_) {
      const auto& m = kv.second;
      if (!Has(m.reviewers, username)) continue;

      // unfinished if UNDER_REVIEW and no review file
      if (m.status == PaperStatus::UNDER_REVIEW) {
        string f = ReviewsDir(m.paper_id, m.current_version) + "/" + username + ".txt";
        bool exists = false;
        if (auto* vfs = GetVfs(auth_)) {
          exists = vfs->exists(f);
        } else {
          exists = std::filesystem::exists(f);
        }
        if (!exists) {
          why = "reviewer_pending:" + m.paper_id;
          return false;
        }
      }
    }
    return true;
  }

  // EDITOR (and others): no extra restriction
  return true;
}