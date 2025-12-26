// filesystem/include/Snapshot.h
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "FSTypes.h"
#include "Allocator.h"
#include "Directory.h"
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace fs {

class CachedDisk;

struct SnapshotInfo {
    std::string name;
    int64_t create_time;
    InodeId root_inode;
    uint32_t block_count;
    bool valid;
    
    SnapshotInfo() 
        : create_time(0), root_inode(INVALID_INODE), block_count(0), valid(false) {}
};

class SnapshotManager {
public:
    SnapshotManager(Allocator* alloc, Directory* dir, DiskImage* disk);
    SnapshotManager(Allocator* alloc, Directory* dir, CachedDisk* cached_disk);
    ~SnapshotManager();

    SnapshotManager(const SnapshotManager&) = delete;
    SnapshotManager& operator=(const SnapshotManager&) = delete;

    // 初始化
    ErrorCode load();
    ErrorCode sync();

    // 快照操作
    ErrorCode createSnapshot(const std::string& name);
    ErrorCode restoreSnapshot(const std::string& name);
    ErrorCode deleteSnapshot(const std::string& name);
    std::vector<SnapshotInfo> listSnapshots() const;
    Result<SnapshotInfo> getSnapshot(const std::string& name) const;
    bool snapshotExists(const std::string& name) const;
    uint32_t getSnapshotCount() const;
    uint32_t getMaxSnapshots() const { return MAX_SNAPSHOTS; }

    // COW
    bool needsCOW(BlockNo block_no) const;
    Result<BlockNo> performCOW(BlockNo block_no);
    ErrorCode cowWriteBlock(BlockNo block_no, const void* data, BlockNo& new_block_no);

    // 统计
    struct SnapshotStats {
        uint32_t total_snapshots;
        uint32_t cow_operations;
        uint32_t shared_blocks;
        uint64_t total_snapshot_size;
    };
    SnapshotStats getStats() const;
    void resetStats();

private:
    // 快照列表操作
    ErrorCode loadSnapshotList();
    ErrorCode saveSnapshotList();
    int32_t findSnapshotIndex(const std::string& name) const;
    ErrorCode allocSnapshotListBlock();

    // inode 树操作
    Result<InodeId> cloneInodeTree(InodeId src_inode);
    ErrorCode cloneDirectoryContents(const Inode& dir_inode, InodeId new_dir_id);

    Result<InodeId> cloneInodeTreeInternal(InodeId src_inode, InodeId parent_inode,
                                           std::unordered_map<InodeId, InodeId>& inode_map);
    ErrorCode freeSnapshotTree(InodeId inode_id, std::unordered_set<InodeId>& visited);
    Result<BlockNo> getFileBlock(const Inode& inode, uint32_t block_index) const;
    ErrorCode setFileBlock(Inode& inode, InodeId inode_id, uint32_t block_index, BlockNo block_no);
    ErrorCode initIndirectBlock(BlockNo block_no);
    ErrorCode incrementBlockRefs(const Inode& inode);
    ErrorCode decrementBlockRefs(const Inode& inode);

    // 块操作
    ErrorCode readBlockInternal(BlockNo block_no, void* buffer) const;
    ErrorCode writeBlockInternal(BlockNo block_no, const void* buffer);

    int64_t currentTime();

    // 成员变量
    Allocator* alloc_;
    Directory* dir_;
    DiskImage* disk_;
    CachedDisk* cached_disk_;
    bool use_cached_disk_;

    static constexpr uint32_t MAX_SNAPSHOTS = 15;
    std::vector<SnapshotInfo> snapshots_;
    BlockNo snapshot_list_block_;
    bool loaded_;
    bool dirty_;

    mutable SnapshotStats stats_;
    mutable std::mutex mutex_;
};

} // namespace fs

#endif // SNAPSHOT_H
