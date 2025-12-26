// server/main.cpp
#include "auth/auth.h"
#include "services/paper.h"
#include "storage/admin_fs.h"
#include "protocol/protocol.h"
#include "net/net.h"
#include "FS.h"

#include <iostream>
#include <filesystem>

int main() {
  // root dir for virtual filesystem
  std::string vfs_root = "/paper_system";
  std::string disk_image = "./paper_system.img";

  fs::FileSystem vfs;
  if (!std::filesystem::exists(disk_image)) {
    auto err = vfs.format(disk_image);
    if (err != fs::ErrorCode::OK) {
      std::cerr << "vfs format failed\n";
      return 1;
    }
  }

  fs::FSConfig cfg;
  cfg.cache_capacity = 64;
  cfg.enable_cache = true;
  cfg.write_through = false;
  if (vfs.mount(disk_image, cfg) != fs::ErrorCode::OK) {
    std::cerr << "vfs mount failed\n";
    return 1;
  }
  bool fresh_root = !vfs.exists(vfs_root);

  AuthManager auth(vfs_root, /*ttl*/3600, &vfs, vfs_root);
  if (!auth.Init(fresh_root)) {
    std::cerr << "auth init failed\n";
    return 1;
  }

  PaperService paper(vfs_root, &auth);
  if (!paper.Init(fresh_root)) {
    std::cerr << "paper init failed\n";
    return 1;
  }

  AdminFsService admin_fs(&auth);
  ProtocolRouter router(&auth, &paper, &admin_fs);

  TcpServer server("127.0.0.1", 9090, &router);
  if (!server.Start()) return 1;
  return 0;
}
