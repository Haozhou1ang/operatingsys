//server/include/services/paper.h
#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <optional>

#include "storage/storage.h"

class AuthManager;

enum class PaperStatus {
  SUBMITTED,
  UNDER_REVIEW,
  FINISH_REVIEW,
  ACCEPTED,
  REJECTED
};

enum class PaperPersistResult {
  OK = 0,
  WRITE_FAIL = 1,
  SYNC_FAIL = 2,
};

struct PaperMeta {
  std::string paper_id;
  std::string author;
  PaperStatus status = PaperStatus::SUBMITTED;
  std::vector<std::string> reviewers; // usernames
  std::string current_version;        // v1/v2/...
};

class PaperService {
public:
  PaperService(std::string root_dir, AuthManager* auth);

  bool Init(bool fresh_root = false); // create folders if needed, load meta.csv to memory

  // Part2 commands
  bool Upload(const std::string& token, const std::string& paper_id, const std::string& content,
              int& err_code, std::string& err_msg);

  bool Revise(const std::string& token, const std::string& paper_id, const std::string& content,
              int& err_code, std::string& err_msg);

  bool Status(const std::string& token, const std::string& paper_id,
              std::string& out_body,
              int& err_code, std::string& err_msg);

  bool ReviewsGet(const std::string& token, const std::string& paper_id,
                  std::string& out_body,
                  int& err_code, std::string& err_msg);

  bool Papers(const std::string& token,
              std::string& out_body,
              int& err_code, std::string& err_msg);

  bool Download(const std::string& token, const std::string& paper_id,
                std::string& out_body,
                int& err_code, std::string& err_msg);

  bool ReviewsGive(const std::string& token, const std::string& paper_id, const std::string& review_content,
                   int& err_code, std::string& err_msg);

  bool Tasks(const std::string& token,
             std::string& out_body,
             int& err_code, std::string& err_msg);

  bool Assign(const std::string& token, const std::string& paper_id, const std::string& reviewer_username,
              int& err_code, std::string& err_msg);

  bool Decide(const std::string& token, const std::string& paper_id, const std::string& decision, // ACCEPT/REJECT
              int& err_code, std::string& err_msg);

  bool Reviews(const std::string& token, const std::string& paper_id,
               std::string& out_body,
               int& err_code, std::string& err_msg);

  bool Queue(const std::string& token,
             std::string& out_body,
             int& err_code, std::string& err_msg);

  // For USER_DEL unfinished task check
  bool UserHasNoUnfinishedTasks(const std::string& username, const std::string& role, std::string& why);

private:
  std::string root_;
  std::string meta_path_;
  std::string papers_dir_;
  AuthManager* auth_;

  std::mutex mu_; // protects in-memory meta cache
  std::unordered_map<std::string, PaperMeta> meta_; // paper_id -> meta

  // helpers
  static std::string StatusToString(PaperStatus s);
  static bool StringToStatus(const std::string& s, PaperStatus& out);

  bool LoadMeta();
  PaperPersistResult SaveMeta();
  PaperPersistResult SaveMetaLocked();

  bool PaperExistsLocked(const std::string& paper_id);
  bool EnsurePaperDirsLocked(const std::string& paper_id);
  static int ParseVersionNum(const std::string& v); // v12 -> 12
  static std::string MakeVersion(int n);           // 12 -> v12

  std::string PaperRoot(const std::string& paper_id) const;
  std::string VersionsRoot(const std::string& paper_id) const;
  std::string VersionDir(const std::string& paper_id, const std::string& v) const;
  std::string PaperFile(const std::string& paper_id, const std::string& v) const;
  std::string ReviewsDir(const std::string& paper_id, const std::string& v) const;

  static std::vector<std::string> SplitSemi(const std::string& s);
  static std::string JoinSemi(const std::vector<std::string>& v);
  static bool Has(const std::vector<std::string>& v, const std::string& x);
  static void AddUnique(std::vector<std::string>& v, const std::string& x);

  bool AllReviewsDoneLocked(const PaperMeta& m);

  // Best-effort cleanup for avoiding orphan files when meta.csv persistence fails.
  // Caller holds `g_fs_mu` (unique) and `mu_`.
  void CleanupPaperVersionLocked(const std::string& paper_id, const std::string& version);
};
