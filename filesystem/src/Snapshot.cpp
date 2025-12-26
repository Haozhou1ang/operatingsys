// filesystem/src/Snapshot.cpp
#include "Snapshot.h"
#include "Cache.h"
#include <cstring>
#include <ctime>
#include <algorithm>

namespace fs {

//==============================================================================
// 构造与析构
//==============================================================================

SnapshotManager::SnapshotManager(Allocator* alloc, Directory* dir, DiskImage* disk)
    : alloc_(alloc)
    , dir_(dir)
    , disk_(disk)
    , cached_disk_(nullptr)
    , use_cached_disk_(false)
    , snapshot_list_block_(INVALID_BLOCK)
    , loaded_(false)
    , dirty_(false)
    , stats_{0, 0, 0, 0}
{
}

SnapshotManager::SnapshotManager(Allocator* alloc, Directory* dir, CachedDisk* cached_disk)
    : alloc_(alloc)
    , dir_(dir)
    , disk_(cached_disk ? cached_disk->getDisk() : nullptr)
    , cached_disk_(cached_disk)
    , use_cached_disk_(cached_disk != nullptr)
    , snapshot_list_block_(INVALID_BLOCK)
    , loaded_(false)
    , dirty_(false)
    , stats_{0, 0, 0, 0}
{
}

SnapshotManager::~SnapshotManager() {
    if (loaded_ && dirty_) {
        sync();
    }
}

//==============================================================================
// 块读写内部接口
//==============================================================================

ErrorCode SnapshotManager::readBlockInternal(BlockNo block_no, void* buffer) const {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->readBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->readBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

ErrorCode SnapshotManager::writeBlockInternal(BlockNo block_no, const void* buffer) {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->writeBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->writeBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

//==============================================================================
// 初始化接口
//==============================================================================

ErrorCode SnapshotManager::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!alloc_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    const Superblock& sb = alloc_->getSuperblock();
    snapshot_list_block_ = sb.snapshot_list_block;

    if (snapshot_list_block_ == 0 || snapshot_list_block_ == INVALID_BLOCK) {
        snapshots_.clear();
        loaded_ = true;
        dirty_ = false;
        return ErrorCode::OK;
    }

    ErrorCode err = loadSnapshotList();
    if (err != ErrorCode::OK) {
        return err;
    }

    loaded_ = true;
    dirty_ = false;
    stats_.total_snapshots = static_cast<uint32_t>(snapshots_.size());

    return ErrorCode::OK;
}

ErrorCode SnapshotManager::sync() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::OK;
    }
    
    if (!dirty_ && snapshots_.empty()) {
        return ErrorCode::OK;
    }

    ErrorCode err = saveSnapshotList();
    if (err != ErrorCode::OK) {
        return err;
    }

    dirty_ = false;
    return alloc_->sync();
}

//==============================================================================
// 快照列表磁盘操作
//==============================================================================

ErrorCode SnapshotManager::loadSnapshotList() {
    if (snapshot_list_block_ == 0 || snapshot_list_block_ == INVALID_BLOCK) {
        snapshots_.clear();
        return ErrorCode::OK;
    }

    uint8_t block_data[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(snapshot_list_block_, block_data);
    if (err != ErrorCode::OK) {
        return err;
    }

    uint32_t count = *reinterpret_cast<uint32_t*>(block_data);
    
    if (count > MAX_SNAPSHOTS) {
        count = MAX_SNAPSHOTS;
    }

    snapshots_.clear();
    snapshots_.reserve(count);

    SnapshotMeta* metas = reinterpret_cast<SnapshotMeta*>(block_data + 8);

    for (uint32_t i = 0; i < count; ++i) {
        if (metas[i].isValid()) {
            SnapshotInfo info;
            info.name = metas[i].getName();
            info.create_time = metas[i].create_time;
            info.root_inode = metas[i].root_inode;
            info.block_count = metas[i].block_count;
            info.valid = true;
            snapshots_.push_back(info);
        }
    }

    return ErrorCode::OK;
}

ErrorCode SnapshotManager::saveSnapshotList() {
    if (snapshots_.empty() && 
        (snapshot_list_block_ == 0 || snapshot_list_block_ == INVALID_BLOCK)) {
        return ErrorCode::OK;
    }
    
    if (snapshot_list_block_ == 0 || snapshot_list_block_ == INVALID_BLOCK) {
        ErrorCode err = allocSnapshotListBlock();
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    uint8_t block_data[BLOCK_SIZE];
    std::memset(block_data, 0, BLOCK_SIZE);

    *reinterpret_cast<uint32_t*>(block_data) = static_cast<uint32_t>(snapshots_.size());

    SnapshotMeta* metas = reinterpret_cast<SnapshotMeta*>(block_data + 8);

    for (size_t i = 0; i < snapshots_.size() && i < MAX_SNAPSHOTS; ++i) {
        const SnapshotInfo& info = snapshots_[i];
        
        std::memset(&metas[i], 0, sizeof(SnapshotMeta));
        
        size_t name_len = std::min(info.name.size(), static_cast<size_t>(MAX_SNAPSHOT_NAME_LEN - 1));
        std::memcpy(metas[i].name, info.name.c_str(), name_len);
        
        metas[i].create_time = info.create_time;
        metas[i].root_inode = info.root_inode;
        metas[i].block_count = info.block_count;
        metas[i].flags = info.valid ? 0x0001 : 0x0000;
    }

    return writeBlockInternal(snapshot_list_block_, block_data);
}

ErrorCode SnapshotManager::allocSnapshotListBlock() {
    auto result = alloc_->allocBlock();
    if (!result.ok()) {
        return result.error();
    }

    snapshot_list_block_ = result.value();

    Superblock& sb = alloc_->getSuperblockMutable();
    sb.snapshot_list_block = snapshot_list_block_;

    return ErrorCode::OK;
}

int32_t SnapshotManager::findSnapshotIndex(const std::string& name) const {
    for (size_t i = 0; i < snapshots_.size(); ++i) {
        if (snapshots_[i].name == name && snapshots_[i].valid) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

//==============================================================================
// 快照操作接口
//==============================================================================

ErrorCode SnapshotManager::createSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    // 验证名称
    if (name.empty() || name.size() > MAX_SNAPSHOT_NAME_LEN - 1) {
        return ErrorCode::E_NAME_TOO_LONG;
    }

    // 检查是否已存在
    if (findSnapshotIndex(name) >= 0) {
        return ErrorCode::E_SNAPSHOT_EXISTS;
    }

    // 检查数量限制
    if (snapshots_.size() >= MAX_SNAPSHOTS) {
        return ErrorCode::E_MAX_SNAPSHOTS;
    }

    // 获取当前根 inode
    const Superblock& sb = alloc_->getSuperblock();
    InodeId current_root = sb.root_inode;

    // 复制 inode 树（目录节点深拷贝，文件数据块共享）
    auto clone_result = cloneInodeTree(current_root);
    if (!clone_result.ok()) {
        return clone_result.error();
    }
    InodeId snapshot_root = clone_result.value();

    // 创建快照信息
    SnapshotInfo info;
    info.name = name;
    info.create_time = currentTime();
    info.root_inode = snapshot_root;
    auto root_result = alloc_->readInode(snapshot_root);
    if (root_result.ok()) {
        info.block_count = root_result.value().block_count;
    } else {
        info.block_count = 0;
    }
    info.valid = true;

    snapshots_.push_back(info);
    dirty_ = true;
    stats_.total_snapshots++;

    // 更新 superblock 中的快照计数
    Superblock& sb_mut = alloc_->getSuperblockMutable();
    sb_mut.snapshot_count = static_cast<uint32_t>(snapshots_.size());

    // 保存快照列表
    ErrorCode err = saveSnapshotList();
    if (err != ErrorCode::OK) {
        return err;
    }

    // 同步到磁盘
    err = alloc_->sync();
    if (err != ErrorCode::OK) {
        return err;
    }

    dirty_ = false;
    return ErrorCode::OK;
}

ErrorCode SnapshotManager::restoreSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    int32_t idx = findSnapshotIndex(name);
    if (idx < 0) {
        return ErrorCode::E_SNAPSHOT_NOT_FOUND;
    }

    const SnapshotInfo& snapshot = snapshots_[idx];

    // 读取快照的根 inode
    auto snapshot_root_result = alloc_->readInode(snapshot.root_inode);
    if (!snapshot_root_result.ok()) {
        return snapshot_root_result.error();
    }

    // 将快照的根 inode 内容复制到 ROOT_INODE
    Inode restored = snapshot_root_result.value();
    restored.ref_count = 1;
    
    ErrorCode err = alloc_->writeInode(ROOT_INODE, restored);
    if (err != ErrorCode::OK) {
        return err;
    }

    // 刷新缓存
    if (use_cached_disk_ && cached_disk_) {
        cached_disk_->clearCache();
    }

    return alloc_->sync();
}

ErrorCode SnapshotManager::deleteSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    int32_t idx = findSnapshotIndex(name);
    if (idx < 0) {
        return ErrorCode::E_SNAPSHOT_NOT_FOUND;
    }

    const SnapshotInfo& snapshot = snapshots_[idx];

    std::unordered_set<InodeId> visited;
    (void)freeSnapshotTree(snapshot.root_inode, visited);

    // 从列表中移除
    snapshots_.erase(snapshots_.begin() + idx);
    dirty_ = true;
    stats_.total_snapshots = static_cast<uint32_t>(snapshots_.size());

    // 更新 superblock
    Superblock& sb = alloc_->getSuperblockMutable();
    sb.snapshot_count = static_cast<uint32_t>(snapshots_.size());

    return saveSnapshotList();
}

std::vector<SnapshotInfo> SnapshotManager::listSnapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshots_;
}

Result<SnapshotInfo> SnapshotManager::getSnapshot(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    int32_t idx = findSnapshotIndex(name);
    if (idx < 0) {
        return Result<SnapshotInfo>::failure(ErrorCode::E_SNAPSHOT_NOT_FOUND);
    }

    return Result<SnapshotInfo>::success(snapshots_[idx]);
}

bool SnapshotManager::snapshotExists(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return findSnapshotIndex(name) >= 0;
}

uint32_t SnapshotManager::getSnapshotCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(snapshots_.size());
}

//==============================================================================
// COW (Copy-on-Write) 接口
//==============================================================================

bool SnapshotManager::needsCOW(BlockNo block_no) const {
    if (snapshots_.empty()) {
        return false;
    }
    uint32_t ref_count = alloc_->getBlockRef(block_no);
    return ref_count > 1;
}

Result<BlockNo> SnapshotManager::performCOW(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!needsCOW(block_no)) {
        return Result<BlockNo>::success(block_no);
    }

    // 分配新块
    auto alloc_result = alloc_->allocBlock();
    if (!alloc_result.ok()) {
        return alloc_result;
    }

    BlockNo new_block = alloc_result.value();

    // 复制数据
    uint8_t buffer[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(block_no, buffer);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(new_block);
        return Result<BlockNo>::failure(err);
    }

    err = writeBlockInternal(new_block, buffer);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(new_block);
        return Result<BlockNo>::failure(err);
    }

    // 减少原块引用计数
    alloc_->decBlockRef(block_no);
    stats_.cow_operations++;

    return Result<BlockNo>::success(new_block);
}

ErrorCode SnapshotManager::cowWriteBlock(BlockNo block_no, const void* data, 
                                          BlockNo& new_block_no) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (needsCOW(block_no)) {
        auto alloc_result = alloc_->allocBlock();
        if (!alloc_result.ok()) {
            return alloc_result.error();
        }

        new_block_no = alloc_result.value();

        ErrorCode err = writeBlockInternal(new_block_no, data);
        if (err != ErrorCode::OK) {
            alloc_->freeBlock(new_block_no);
            return err;
        }

        alloc_->decBlockRef(block_no);
        stats_.cow_operations++;
    } else {
        new_block_no = block_no;
        ErrorCode err = writeBlockInternal(block_no, data);
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    return ErrorCode::OK;
}

//==============================================================================
// 引用计数操作
//==============================================================================

ErrorCode SnapshotManager::incrementBlockRefs(const Inode& inode) {
    // 直接块
    for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
        if (inode.direct_blocks[i] != INVALID_BLOCK) {
            alloc_->incBlockRef(inode.direct_blocks[i]);
            stats_.shared_blocks++;
        }
    }

    // 一级间接块
    if (inode.single_indirect != INVALID_BLOCK) {
        alloc_->incBlockRef(inode.single_indirect);
        
        uint8_t block_data[BLOCK_SIZE];
        if (readBlockInternal(inode.single_indirect, block_data) == ErrorCode::OK) {
            BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != INVALID_BLOCK) {
                    alloc_->incBlockRef(ptrs[i]);
                    stats_.shared_blocks++;
                }
            }
        }
    }

    // 二级间接块
    if (inode.double_indirect != INVALID_BLOCK) {
        alloc_->incBlockRef(inode.double_indirect);
        
        uint8_t l1_data[BLOCK_SIZE];
        if (readBlockInternal(inode.double_indirect, l1_data) == ErrorCode::OK) {
            BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
            
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (l1_ptrs[i] != INVALID_BLOCK) {
                    alloc_->incBlockRef(l1_ptrs[i]);
                    
                    uint8_t l2_data[BLOCK_SIZE];
                    if (readBlockInternal(l1_ptrs[i], l2_data) == ErrorCode::OK) {
                        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
                        for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                            if (l2_ptrs[j] != INVALID_BLOCK) {
                                alloc_->incBlockRef(l2_ptrs[j]);
                                stats_.shared_blocks++;
                            }
                        }
                    }
                }
            }
        }
    }

    return ErrorCode::OK;
}

ErrorCode SnapshotManager::decrementBlockRefs(const Inode& inode) {
    // 直接块
    for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
        if (inode.direct_blocks[i] != INVALID_BLOCK) {
            alloc_->decBlockRef(inode.direct_blocks[i]);
        }
    }

    // 一级间接块
    if (inode.single_indirect != INVALID_BLOCK) {
        uint8_t block_data[BLOCK_SIZE];
        if (readBlockInternal(inode.single_indirect, block_data) == ErrorCode::OK) {
            BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != INVALID_BLOCK) {
                    alloc_->decBlockRef(ptrs[i]);
                }
            }
        }
        alloc_->decBlockRef(inode.single_indirect);
    }

    // 二级间接块
    if (inode.double_indirect != INVALID_BLOCK) {
        uint8_t l1_data[BLOCK_SIZE];
        if (readBlockInternal(inode.double_indirect, l1_data) == ErrorCode::OK) {
            BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
            
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (l1_ptrs[i] != INVALID_BLOCK) {
                    uint8_t l2_data[BLOCK_SIZE];
                    if (readBlockInternal(l1_ptrs[i], l2_data) == ErrorCode::OK) {
                        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
                        for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                            if (l2_ptrs[j] != INVALID_BLOCK) {
                                alloc_->decBlockRef(l2_ptrs[j]);
                            }
                        }
                    }
                    alloc_->decBlockRef(l1_ptrs[i]);
                }
            }
        }
        alloc_->decBlockRef(inode.double_indirect);
    }

    return ErrorCode::OK;
}

//==============================================================================
// 统计接口
//==============================================================================

SnapshotManager::SnapshotStats SnapshotManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void SnapshotManager::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.cow_operations = 0;
    stats_.shared_blocks = 0;
}

//==============================================================================
// 辅助方法
//==============================================================================

int64_t SnapshotManager::currentTime() {
    return static_cast<int64_t>(std::time(nullptr));
}

Result<InodeId> SnapshotManager::cloneInodeTree(InodeId src_inode) {
    std::unordered_map<InodeId, InodeId> inode_map;
    return cloneInodeTreeInternal(src_inode, INVALID_INODE, inode_map);
}

ErrorCode SnapshotManager::cloneDirectoryContents(const Inode& dir_inode, InodeId new_dir_id) {
    (void)dir_inode;
    (void)new_dir_id;
    return ErrorCode::OK;
}

Result<InodeId> SnapshotManager::cloneInodeTreeInternal(
    InodeId src_inode, InodeId parent_inode,
    std::unordered_map<InodeId, InodeId>& inode_map) {
    auto it = inode_map.find(src_inode);
    if (it != inode_map.end()) {
        return Result<InodeId>::success(it->second);
    }

    auto src_result = alloc_->readInode(src_inode);
    if (!src_result.ok()) {
        return Result<InodeId>::failure(src_result.error());
    }

    auto new_inode_result = alloc_->allocInode();
    if (!new_inode_result.ok()) {
        return Result<InodeId>::failure(new_inode_result.error());
    }

    InodeId new_inode_id = new_inode_result.value();
    inode_map[src_inode] = new_inode_id;

    Inode src = src_result.value();
    Inode dst = src;
    dst.ref_count = 1;

    if (dst.isDirectory()) {
        for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
            dst.direct_blocks[i] = INVALID_BLOCK;
        }
        dst.single_indirect = INVALID_BLOCK;
        dst.double_indirect = INVALID_BLOCK;
        dst.block_count = 0;

        uint32_t num_blocks = (src.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (num_blocks == 0) num_blocks = 1;

        DirEntry entries[DIRENTRIES_PER_BLOCK];
        for (uint32_t bi = 0; bi < num_blocks; ++bi) {
            auto block_result = getFileBlock(src, bi);
            if (!block_result.ok()) {
                continue;
            }

            ErrorCode err = readBlockInternal(block_result.value(), entries);
            if (err != ErrorCode::OK) {
                return Result<InodeId>::failure(err);
            }

            for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
                if (!entries[i].isValid()) continue;
                std::string name = entries[i].getName();
                if (name == ".") {
                    entries[i].inode = new_inode_id;
                } else if (name == "..") {
                    entries[i].inode = (parent_inode == INVALID_INODE ? new_inode_id : parent_inode);
                } else {
                    auto child_result = cloneInodeTreeInternal(entries[i].inode, new_inode_id, inode_map);
                    if (!child_result.ok()) {
                        return Result<InodeId>::failure(child_result.error());
                    }
                    entries[i].inode = child_result.value();
                }
            }

            auto new_block_result = alloc_->allocBlock();
            if (!new_block_result.ok()) {
                return Result<InodeId>::failure(new_block_result.error());
            }
            BlockNo new_block = new_block_result.value();
            err = writeBlockInternal(new_block, entries);
            if (err != ErrorCode::OK) {
                alloc_->freeBlock(new_block);
                return Result<InodeId>::failure(err);
            }

            err = setFileBlock(dst, new_inode_id, bi, new_block);
            if (err != ErrorCode::OK) {
                alloc_->freeBlock(new_block);
                return Result<InodeId>::failure(err);
            }
        }
    } else {
        ErrorCode err = incrementBlockRefs(src);
        if (err != ErrorCode::OK) {
            return Result<InodeId>::failure(err);
        }
    }

    ErrorCode err = alloc_->writeInode(new_inode_id, dst);
    if (err != ErrorCode::OK) {
        return Result<InodeId>::failure(err);
    }

    return Result<InodeId>::success(new_inode_id);
}

ErrorCode SnapshotManager::freeSnapshotTree(InodeId inode_id, std::unordered_set<InodeId>& visited) {
    if (inode_id == INVALID_INODE) {
        return ErrorCode::OK;
    }
    if (visited.find(inode_id) != visited.end()) {
        return ErrorCode::OK;
    }
    visited.insert(inode_id);

    auto inode_result = alloc_->readInode(inode_id);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    Inode inode = inode_result.value();

    if (inode.isDirectory()) {
        uint32_t num_blocks = (inode.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        if (num_blocks == 0) num_blocks = 1;
        DirEntry entries[DIRENTRIES_PER_BLOCK];

        for (uint32_t bi = 0; bi < num_blocks; ++bi) {
            auto block_result = getFileBlock(inode, bi);
            if (!block_result.ok()) {
                continue;
            }
            ErrorCode err = readBlockInternal(block_result.value(), entries);
            if (err != ErrorCode::OK) {
                return err;
            }
            for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
                if (!entries[i].isValid()) continue;
                std::string name = entries[i].getName();
                if (name == "." || name == "..") continue;
                err = freeSnapshotTree(entries[i].inode, visited);
                if (err != ErrorCode::OK) {
                    return err;
                }
            }
        }
    }

    (void)decrementBlockRefs(inode);
    (void)alloc_->freeInode(inode_id);
    return ErrorCode::OK;
}

Result<BlockNo> SnapshotManager::getFileBlock(const Inode& inode, uint32_t block_index) const {
    if (block_index < NUM_DIRECT_BLOCKS) {
        BlockNo block = inode.direct_blocks[block_index];
        if (block == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        return Result<BlockNo>::success(block);
    }

    block_index -= NUM_DIRECT_BLOCKS;

    if (block_index < PTRS_PER_BLOCK) {
        if (inode.single_indirect == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        uint8_t block_data[BLOCK_SIZE];
        ErrorCode err = readBlockInternal(inode.single_indirect, block_data);
        if (err != ErrorCode::OK) {
            return Result<BlockNo>::failure(err);
        }
        BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
        if (ptrs[block_index] == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        return Result<BlockNo>::success(ptrs[block_index]);
    }

    block_index -= PTRS_PER_BLOCK;

    if (block_index < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode.double_indirect == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        uint32_t l1_index = block_index / PTRS_PER_BLOCK;
        uint32_t l2_index = block_index % PTRS_PER_BLOCK;

        uint8_t l1_data[BLOCK_SIZE];
        ErrorCode err = readBlockInternal(inode.double_indirect, l1_data);
        if (err != ErrorCode::OK) {
            return Result<BlockNo>::failure(err);
        }
        BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
        if (l1_ptrs[l1_index] == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }

        uint8_t l2_data[BLOCK_SIZE];
        err = readBlockInternal(l1_ptrs[l1_index], l2_data);
        if (err != ErrorCode::OK) {
            return Result<BlockNo>::failure(err);
        }
        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
        if (l2_ptrs[l2_index] == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }

        return Result<BlockNo>::success(l2_ptrs[l2_index]);
    }

    return Result<BlockNo>::failure(ErrorCode::E_FILE_TOO_LARGE);
}

ErrorCode SnapshotManager::setFileBlock(Inode& inode, InodeId inode_id,
                                        uint32_t block_index, BlockNo block_no) {
    (void)inode_id;
    if (block_index < NUM_DIRECT_BLOCKS) {
        if (inode.direct_blocks[block_index] == INVALID_BLOCK) {
            inode.block_count++;
        }
        inode.direct_blocks[block_index] = block_no;
        return ErrorCode::OK;
    }

    block_index -= NUM_DIRECT_BLOCKS;

    if (block_index < PTRS_PER_BLOCK) {
        if (inode.single_indirect == INVALID_BLOCK) {
            auto alloc_result = alloc_->allocBlock();
            if (!alloc_result.ok()) {
                return alloc_result.error();
            }
            inode.single_indirect = alloc_result.value();
            inode.block_count++;
            ErrorCode err = initIndirectBlock(inode.single_indirect);
            if (err != ErrorCode::OK) {
                return err;
            }
        }

        uint8_t block_data[BLOCK_SIZE];
        ErrorCode err = readBlockInternal(inode.single_indirect, block_data);
        if (err != ErrorCode::OK) {
            return err;
        }
        BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
        if (ptrs[block_index] == INVALID_BLOCK) {
            inode.block_count++;
        }
        ptrs[block_index] = block_no;
        return writeBlockInternal(inode.single_indirect, block_data);
    }

    block_index -= PTRS_PER_BLOCK;

    if (block_index < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode.double_indirect == INVALID_BLOCK) {
            auto alloc_result = alloc_->allocBlock();
            if (!alloc_result.ok()) {
                return alloc_result.error();
            }
            inode.double_indirect = alloc_result.value();
            inode.block_count++;
            ErrorCode err = initIndirectBlock(inode.double_indirect);
            if (err != ErrorCode::OK) {
                return err;
            }
        }

        uint32_t l1_index = block_index / PTRS_PER_BLOCK;
        uint32_t l2_index = block_index % PTRS_PER_BLOCK;

        uint8_t l1_data[BLOCK_SIZE];
        ErrorCode err = readBlockInternal(inode.double_indirect, l1_data);
        if (err != ErrorCode::OK) {
            return err;
        }
        BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
        if (l1_ptrs[l1_index] == INVALID_BLOCK) {
            auto alloc_result = alloc_->allocBlock();
            if (!alloc_result.ok()) {
                return alloc_result.error();
            }
            l1_ptrs[l1_index] = alloc_result.value();
            inode.block_count++;
            err = initIndirectBlock(l1_ptrs[l1_index]);
            if (err != ErrorCode::OK) {
                return err;
            }
            err = writeBlockInternal(inode.double_indirect, l1_data);
            if (err != ErrorCode::OK) {
                return err;
            }
        }

        BlockNo l2_block = l1_ptrs[l1_index];
        uint8_t l2_data[BLOCK_SIZE];
        err = readBlockInternal(l2_block, l2_data);
        if (err != ErrorCode::OK) {
            return err;
        }
        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
        if (l2_ptrs[l2_index] == INVALID_BLOCK) {
            inode.block_count++;
        }
        l2_ptrs[l2_index] = block_no;
        return writeBlockInternal(l2_block, l2_data);
    }

    return ErrorCode::E_FILE_TOO_LARGE;
}

ErrorCode SnapshotManager::initIndirectBlock(BlockNo block_no) {
    uint8_t block_data[BLOCK_SIZE];
    BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        ptrs[i] = INVALID_BLOCK;
    }
    return writeBlockInternal(block_no, block_data);
}

} // namespace fs
