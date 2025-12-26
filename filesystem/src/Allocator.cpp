// filesystem/src/Allocator.cpp
#include "Allocator.h"
#include "Cache.h"
#include <cstring>
#include <algorithm>
#include <iostream>

namespace fs {

//==============================================================================
// 构造与析构
//==============================================================================

Allocator::Allocator(DiskImage* disk)
    : disk_(disk)
    , cached_disk_(nullptr)
    , use_cached_disk_(false)
    , loaded_(false)
    , inode_bitmap_dirty_(false)
    , block_bitmap_dirty_(false)
    , superblock_dirty_(false)
    , refcount_enabled_(false)
    , stats_{0, 0, 0, 0, 0, 0}
{
    std::memset(&superblock_, 0, sizeof(Superblock));
}

Allocator::Allocator(CachedDisk* cached_disk)
    : disk_(cached_disk ? cached_disk->getDisk() : nullptr)
    , cached_disk_(cached_disk)
    , use_cached_disk_(cached_disk != nullptr)
    , loaded_(false)
    , inode_bitmap_dirty_(false)
    , block_bitmap_dirty_(false)
    , superblock_dirty_(false)
    , refcount_enabled_(false)
    , stats_{0, 0, 0, 0, 0, 0}
{
    std::memset(&superblock_, 0, sizeof(Superblock));
}

Allocator::~Allocator() {
    if (loaded_) {
        sync();
    }
}

//==============================================================================
// 统一的块读写接口
//==============================================================================

ErrorCode Allocator::readBlockInternal(BlockNo block_no, void* buffer) {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->readBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->readBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

ErrorCode Allocator::writeBlockInternal(BlockNo block_no, const void* buffer) {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->writeBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->writeBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

ErrorCode Allocator::flushInternal() {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->flush();
    }
    if (disk_) {
        return disk_->sync();
    }
    return ErrorCode::E_IO;
}

//==============================================================================
// 初始化接口
//==============================================================================

ErrorCode Allocator::load() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!disk_ && !cached_disk_) {
        return ErrorCode::E_IO;
    }
    
    if (disk_ && !disk_->isOpen()) {
        return ErrorCode::E_IO;
    }

    // 加载 superblock（直接从磁盘，不经过缓存，确保一致性）
    ErrorCode err;
    if (disk_) {
        err = disk_->loadSuperblock(superblock_);
    } else {
        return ErrorCode::E_IO;
    }
    
    if (err != ErrorCode::OK) {
        return err;
    }

    // 加载 inode 位图
    err = loadInodeBitmap();
    if (err != ErrorCode::OK) {
        return err;
    }

    // 加载 block 位图
    err = loadBlockBitmap();
    if (err != ErrorCode::OK) {
        return err;
    }

    // 初始化引用计数表
    block_refcount_.resize(superblock_.data_block_count, 1);
    refcount_enabled_ = true;

    loaded_ = true;
    inode_bitmap_dirty_ = false;
    block_bitmap_dirty_ = false;
    superblock_dirty_ = false;

    return ErrorCode::OK;
}

ErrorCode Allocator::sync() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    ErrorCode err = ErrorCode::OK;

    // 保存 inode 位图
    if (inode_bitmap_dirty_) {
        err = saveInodeBitmap();
        if (err != ErrorCode::OK) {
            return err;
        }
        inode_bitmap_dirty_ = false;
    }

    // 保存 block 位图
    if (block_bitmap_dirty_) {
        err = saveBlockBitmap();
        if (err != ErrorCode::OK) {
            return err;
        }
        block_bitmap_dirty_ = false;
    }

    // 保存 superblock（直接写磁盘，确保一致性）
    if (superblock_dirty_) {
        if (disk_) {
            err = disk_->saveSuperblock(superblock_);
        }
        if (err != ErrorCode::OK) {
            return err;
        }
        superblock_dirty_ = false;
    }

    return flushInternal();
}

ErrorCode Allocator::reload() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        loaded_ = false;
        inode_bitmap_dirty_ = false;
        block_bitmap_dirty_ = false;
        superblock_dirty_ = false;
    }

    return load();
}

//==============================================================================
// 位图加载与保存
//==============================================================================

ErrorCode Allocator::loadInodeBitmap() {
    uint32_t bitmap_size = superblock_.inode_bitmap_blocks * BLOCK_SIZE;
    inode_bitmap_.resize(bitmap_size);

    for (uint32_t i = 0; i < superblock_.inode_bitmap_blocks; ++i) {
        ErrorCode err = readBlockInternal(
            superblock_.inode_bitmap_start + i,
            inode_bitmap_.data() + i * BLOCK_SIZE
        );
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    stats_.bitmap_reads += superblock_.inode_bitmap_blocks;
    return ErrorCode::OK;
}

ErrorCode Allocator::loadBlockBitmap() {
    uint32_t bitmap_size = superblock_.block_bitmap_blocks * BLOCK_SIZE;
    block_bitmap_.resize(bitmap_size);

    for (uint32_t i = 0; i < superblock_.block_bitmap_blocks; ++i) {
        ErrorCode err = readBlockInternal(
            superblock_.block_bitmap_start + i,
            block_bitmap_.data() + i * BLOCK_SIZE
        );
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    stats_.bitmap_reads += superblock_.block_bitmap_blocks;
    return ErrorCode::OK;
}

ErrorCode Allocator::saveInodeBitmap() {
    for (uint32_t i = 0; i < superblock_.inode_bitmap_blocks; ++i) {
        ErrorCode err = writeBlockInternal(
            superblock_.inode_bitmap_start + i,
            inode_bitmap_.data() + i * BLOCK_SIZE
        );
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    stats_.bitmap_writes += superblock_.inode_bitmap_blocks;
    return ErrorCode::OK;
}

ErrorCode Allocator::saveBlockBitmap() {
    for (uint32_t i = 0; i < superblock_.block_bitmap_blocks; ++i) {
        ErrorCode err = writeBlockInternal(
            superblock_.block_bitmap_start + i,
            block_bitmap_.data() + i * BLOCK_SIZE
        );
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    stats_.bitmap_writes += superblock_.block_bitmap_blocks;
    return ErrorCode::OK;
}

//==============================================================================
// Inode 分配接口
//==============================================================================

Result<InodeId> Allocator::allocInode() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return Result<InodeId>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (superblock_.free_inodes == 0) {
        return Result<InodeId>::failure(ErrorCode::E_NO_INODE);
    }

    int32_t free_idx = findFreeInode();
    if (free_idx < 0) {
        return Result<InodeId>::failure(ErrorCode::E_NO_INODE);
    }

    InodeId inode_id = static_cast<InodeId>(free_idx);

    Bitmap bmap(inode_bitmap_.data(), superblock_.total_inodes);
    bmap.set(inode_id);
    inode_bitmap_dirty_ = true;

    Inode new_inode;
    new_inode.init(FileType::FREE);
    
    ErrorCode err = writeInodeInternal(inode_id, new_inode);
    if (err != ErrorCode::OK) {
        bmap.clear(inode_id);
        return Result<InodeId>::failure(err);
    }

    updateInodeStats(1);
    stats_.inode_allocs++;

    return Result<InodeId>::success(inode_id);
}

ErrorCode Allocator::freeInode(InodeId inode_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (inode_id >= superblock_.total_inodes) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (inode_id == ROOT_INODE) {
        return ErrorCode::E_PERMISSION;
    }

    Bitmap bmap(inode_bitmap_.data(), superblock_.total_inodes);

    if (!bmap.get(inode_id)) {
        return ErrorCode::E_INVALID_PARAM;
    }

    Inode empty_inode;
    empty_inode.clear();
    ErrorCode err = writeInodeInternal(inode_id, empty_inode);
    if (err != ErrorCode::OK) {
        return err;
    }

    bmap.clear(inode_id);
    inode_bitmap_dirty_ = true;

    updateInodeStats(-1);
    stats_.inode_frees++;

    return ErrorCode::OK;
}

bool Allocator::isInodeAllocated(InodeId inode_id) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_ || inode_id >= superblock_.total_inodes) {
        return false;
    }

    Bitmap bmap(const_cast<uint8_t*>(inode_bitmap_.data()), superblock_.total_inodes);
    return bmap.get(inode_id);
}

Result<Inode> Allocator::readInode(InodeId inode_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return Result<Inode>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (inode_id >= superblock_.total_inodes) {
        return Result<Inode>::failure(ErrorCode::E_INVALID_PARAM);
    }

    BlockNo block = getInodeBlock(inode_id);
    uint32_t offset = getInodeOffset(inode_id);

    uint8_t block_data[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(block, block_data);
    if (err != ErrorCode::OK) {
        return Result<Inode>::failure(err);
    }

    Inode* inodes = reinterpret_cast<Inode*>(block_data);
    return Result<Inode>::success(inodes[offset]);
}

ErrorCode Allocator::writeInode(InodeId inode_id, const Inode& inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    return writeInodeInternal(inode_id, inode);
}

ErrorCode Allocator::writeInodeInternal(InodeId inode_id, const Inode& inode) {
    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (inode_id >= superblock_.total_inodes) {
        return ErrorCode::E_INVALID_PARAM;
    }

    BlockNo block = getInodeBlock(inode_id);
    uint32_t offset = getInodeOffset(inode_id);

    uint8_t block_data[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(block, block_data);
    if (err != ErrorCode::OK) {
        return err;
    }

    Inode* inodes = reinterpret_cast<Inode*>(block_data);
    inodes[offset] = inode;

    return writeBlockInternal(block, block_data);
}

//==============================================================================
// 数据块分配接口
//==============================================================================

Result<BlockNo> Allocator::allocBlock() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return Result<BlockNo>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (superblock_.free_blocks == 0) {
        return Result<BlockNo>::failure(ErrorCode::E_NO_SPACE);
    }

    int32_t free_idx = findFreeBlock();
    if (free_idx < 0) {
        return Result<BlockNo>::failure(ErrorCode::E_NO_SPACE);
    }

    BlockNo abs_block = dataBlockToAbsolute(static_cast<uint32_t>(free_idx));

    Bitmap bmap(block_bitmap_.data(), superblock_.data_block_count);
    bmap.set(static_cast<uint32_t>(free_idx));
    block_bitmap_dirty_ = true;

    if (refcount_enabled_ && static_cast<size_t>(free_idx) < block_refcount_.size()) {
        block_refcount_[free_idx] = 1;
    }

    // 清零新分配的块
    uint8_t zero_block[BLOCK_SIZE] = {0};
    ErrorCode err = writeBlockInternal(abs_block, zero_block);
    if (err != ErrorCode::OK) {
        bmap.clear(static_cast<uint32_t>(free_idx));
        return Result<BlockNo>::failure(err);
    }

    updateBlockStats(1);
    stats_.block_allocs++;

    return Result<BlockNo>::success(abs_block);
}

Result<std::vector<BlockNo>> Allocator::allocBlocks(uint32_t count) {
    std::vector<BlockNo> blocks;
    blocks.reserve(count);

    for (uint32_t i = 0; i < count; ++i) {
        auto result = allocBlock();
        if (!result.ok()) {
            for (BlockNo b : blocks) {
                freeBlock(b);
            }
            return Result<std::vector<BlockNo>>::failure(result.error());
        }
        blocks.push_back(result.value());
    }

    return Result<std::vector<BlockNo>>::success(std::move(blocks));
}

ErrorCode Allocator::freeBlock(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (!isValidDataBlock(block_no)) {
        return ErrorCode::E_INVALID_PARAM;
    }

    uint32_t data_idx = absoluteToDataBlock(block_no);

    Bitmap bmap(block_bitmap_.data(), superblock_.data_block_count);

    if (!bmap.get(data_idx)) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (refcount_enabled_ && data_idx < block_refcount_.size()) {
        if (block_refcount_[data_idx] > 1) {
            block_refcount_[data_idx]--;
            return ErrorCode::OK;
        }
        block_refcount_[data_idx] = 0;
    }

    bmap.clear(data_idx);
    block_bitmap_dirty_ = true;

    updateBlockStats(-1);
    stats_.block_frees++;

    return ErrorCode::OK;
}

ErrorCode Allocator::freeBlocks(const std::vector<BlockNo>& blocks) {
    for (BlockNo b : blocks) {
        ErrorCode err = freeBlock(b);
        if (err != ErrorCode::OK) {
            return err;
        }
    }
    return ErrorCode::OK;
}

bool Allocator::isBlockAllocated(BlockNo block_no) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_ || !isValidDataBlock(block_no)) {
        return false;
    }

    uint32_t data_idx = absoluteToDataBlock(block_no);
    Bitmap bmap(const_cast<uint8_t*>(block_bitmap_.data()), superblock_.data_block_count);
    return bmap.get(data_idx);
}

//==============================================================================
// 引用计数接口
//==============================================================================

Result<uint32_t> Allocator::incBlockRef(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_ || !refcount_enabled_) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (!isValidDataBlock(block_no)) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    uint32_t data_idx = absoluteToDataBlock(block_no);
    if (data_idx >= block_refcount_.size()) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (block_refcount_[data_idx] >= 255) {
        return Result<uint32_t>::failure(ErrorCode::E_INTERNAL);
    }

    block_refcount_[data_idx]++;
    return Result<uint32_t>::success(block_refcount_[data_idx]);
}

Result<uint32_t> Allocator::decBlockRef(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_ || !refcount_enabled_) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (!isValidDataBlock(block_no)) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    uint32_t data_idx = absoluteToDataBlock(block_no);
    if (data_idx >= block_refcount_.size()) {
        return Result<uint32_t>::failure(ErrorCode::E_INVALID_PARAM);
    }

    if (block_refcount_[data_idx] == 0) {
        return Result<uint32_t>::failure(ErrorCode::E_INTERNAL);
    }

    block_refcount_[data_idx]--;

    if (block_refcount_[data_idx] == 0) {
        Bitmap bmap(block_bitmap_.data(), superblock_.data_block_count);
        bmap.clear(data_idx);
        block_bitmap_dirty_ = true;
        updateBlockStats(-1);
        stats_.block_frees++;
    }

    return Result<uint32_t>::success(block_refcount_[data_idx]);
}

uint32_t Allocator::getBlockRef(BlockNo block_no) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_ || !refcount_enabled_) {
        return 0;
    }

    if (!isValidDataBlock(block_no)) {
        return 0;
    }

    uint32_t data_idx = absoluteToDataBlock(block_no);
    if (data_idx >= block_refcount_.size()) {
        return 0;
    }

    return block_refcount_[data_idx];
}

//==============================================================================
// 状态查询接口
//==============================================================================

uint32_t Allocator::getFreeInodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.free_inodes : 0;
}

uint32_t Allocator::getUsedInodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.used_inodes : 0;
}

uint32_t Allocator::getTotalInodeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.total_inodes : 0;
}

uint32_t Allocator::getFreeBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.free_blocks : 0;
}

uint32_t Allocator::getUsedBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.used_blocks : 0;
}

uint32_t Allocator::getTotalBlockCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loaded_ ? superblock_.data_block_count : 0;
}

Allocator::AllocStats Allocator::getAllocStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void Allocator::resetAllocStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = {0, 0, 0, 0, 0, 0};
}

//==============================================================================
// 一致性检查
//==============================================================================

ErrorCode Allocator::checkConsistency(bool fix) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!loaded_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    bool has_error = false;

    Bitmap inode_bmap(inode_bitmap_.data(), superblock_.total_inodes);
    uint32_t actual_used_inodes = inode_bmap.countUsed();
    
    if (actual_used_inodes != superblock_.used_inodes) {
        std::cerr << "Inconsistency: inode count mismatch. "
                  << "Bitmap: " << actual_used_inodes 
                  << ", Superblock: " << superblock_.used_inodes << std::endl;
        has_error = true;
        
        if (fix) {
            superblock_.used_inodes = actual_used_inodes;
            superblock_.free_inodes = superblock_.total_inodes - actual_used_inodes;
            superblock_dirty_ = true;
        }
    }

    Bitmap block_bmap(block_bitmap_.data(), superblock_.data_block_count);
    uint32_t actual_used_blocks = block_bmap.countUsed();
    
    if (actual_used_blocks != superblock_.used_blocks) {
        std::cerr << "Inconsistency: block count mismatch. "
                  << "Bitmap: " << actual_used_blocks 
                  << ", Superblock: " << superblock_.used_blocks << std::endl;
        has_error = true;
        
        if (fix) {
            superblock_.used_blocks = actual_used_blocks;
            superblock_.free_blocks = superblock_.data_block_count - actual_used_blocks;
            superblock_dirty_ = true;
        }
    }

    if (!inode_bmap.get(ROOT_INODE)) {
        std::cerr << "Inconsistency: root inode not allocated" << std::endl;
        has_error = true;
        
        if (fix) {
            inode_bmap.set(ROOT_INODE);
            inode_bitmap_dirty_ = true;
        }
    }

    if (has_error && fix) {
        // 不在锁内调用 sync，避免死锁
    }

    return has_error ? ErrorCode::E_INTERNAL : ErrorCode::OK;
}

//==============================================================================
// 内部辅助方法
//==============================================================================

BlockNo Allocator::dataBlockToAbsolute(uint32_t data_block_index) const {
    return superblock_.data_block_start + data_block_index;
}

uint32_t Allocator::absoluteToDataBlock(BlockNo abs_block_no) const {
    return abs_block_no - superblock_.data_block_start;
}

bool Allocator::isValidDataBlock(BlockNo abs_block_no) const {
    return abs_block_no >= superblock_.data_block_start &&
           abs_block_no < superblock_.data_block_start + superblock_.data_block_count;
}

BlockNo Allocator::getInodeBlock(InodeId inode_id) const {
    return superblock_.inode_table_start + (inode_id / INODES_PER_BLOCK);
}

uint32_t Allocator::getInodeOffset(InodeId inode_id) const {
    return inode_id % INODES_PER_BLOCK;
}

int32_t Allocator::findFreeInode() const {
    Bitmap bmap(const_cast<uint8_t*>(inode_bitmap_.data()), superblock_.total_inodes);
    return bmap.findFirstFree();
}

int32_t Allocator::findFreeBlock() const {
    Bitmap bmap(const_cast<uint8_t*>(block_bitmap_.data()), superblock_.data_block_count);
    return bmap.findFirstFree();
}

void Allocator::updateInodeStats(int32_t delta) {
    if (delta > 0) {
        superblock_.used_inodes += static_cast<uint32_t>(delta);
        superblock_.free_inodes -= static_cast<uint32_t>(delta);
    } else {
        superblock_.used_inodes -= static_cast<uint32_t>(-delta);
        superblock_.free_inodes += static_cast<uint32_t>(-delta);
    }
    superblock_dirty_ = true;
}

void Allocator::updateBlockStats(int32_t delta) {
    if (delta > 0) {
        superblock_.used_blocks += static_cast<uint32_t>(delta);
        superblock_.free_blocks -= static_cast<uint32_t>(delta);
    } else {
        superblock_.used_blocks -= static_cast<uint32_t>(-delta);
        superblock_.free_blocks += static_cast<uint32_t>(-delta);
    }
    superblock_dirty_ = true;
}

} // namespace fs