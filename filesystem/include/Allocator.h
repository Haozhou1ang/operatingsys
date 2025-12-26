// filesystem/include/Allocator.h
#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include "FSTypes.h"
#include "DiskImage.h"
#include <vector>
#include <mutex>
#include <memory>

namespace fs {

// 前向声明
class CachedDisk;

/**
 * Allocator - inode 与数据块分配管理器
 */
class Allocator {
public:
    /**
     * 构造函数（使用裸磁盘）
     * @param disk 磁盘镜像指针
     */
    explicit Allocator(DiskImage* disk);
    
    /**
     * 构造函数（使用带缓存的磁盘）
     * @param cached_disk 带缓存的磁盘指针
     */
    explicit Allocator(CachedDisk* cached_disk);
    
    ~Allocator();

    // 禁止拷贝
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    //==========================================================================
    // 初始化接口
    //==========================================================================

    ErrorCode load();
    ErrorCode sync();
    ErrorCode reload();

    //==========================================================================
    // Inode 分配接口
    //==========================================================================

    Result<InodeId> allocInode();
    ErrorCode freeInode(InodeId inode_id);
    bool isInodeAllocated(InodeId inode_id) const;
    Result<Inode> readInode(InodeId inode_id);
    ErrorCode writeInode(InodeId inode_id, const Inode& inode);

    //==========================================================================
    // 数据块分配接口
    //==========================================================================

    Result<BlockNo> allocBlock();
    Result<std::vector<BlockNo>> allocBlocks(uint32_t count);
    ErrorCode freeBlock(BlockNo block_no);
    ErrorCode freeBlocks(const std::vector<BlockNo>& blocks);
    bool isBlockAllocated(BlockNo block_no) const;

    //==========================================================================
    // 引用计数接口
    //==========================================================================

    Result<uint32_t> incBlockRef(BlockNo block_no);
    Result<uint32_t> decBlockRef(BlockNo block_no);
    uint32_t getBlockRef(BlockNo block_no) const;

    //==========================================================================
    // 状态查询接口
    //==========================================================================

    uint32_t getFreeInodeCount() const;
    uint32_t getUsedInodeCount() const;
    uint32_t getTotalInodeCount() const;
    uint32_t getFreeBlockCount() const;
    uint32_t getUsedBlockCount() const;
    uint32_t getTotalBlockCount() const;

    const Superblock& getSuperblock() const { return superblock_; }
    Superblock& getSuperblockMutable() { return superblock_; }

    //==========================================================================
    // 一致性检查接口
    //==========================================================================

    ErrorCode checkConsistency(bool fix = false);

    struct AllocStats {
        uint32_t inode_allocs;
        uint32_t inode_frees;
        uint32_t block_allocs;
        uint32_t block_frees;
        uint32_t bitmap_reads;
        uint32_t bitmap_writes;
    };
    AllocStats getAllocStats() const;
    void resetAllocStats();

    //==========================================================================
    // 缓存相关
    //==========================================================================
    
    /**
     * 检查是否使用缓存
     */
    bool isCacheEnabled() const { return use_cached_disk_; }
    
    /**
     * 获取底层磁盘（用于特殊操作）
     */
    DiskImage* getDisk() { return disk_; }

private:
    //==========================================================================
    // 内部辅助方法
    //==========================================================================

    // 统一的块读写接口
    ErrorCode readBlockInternal(BlockNo block_no, void* buffer);
    ErrorCode writeBlockInternal(BlockNo block_no, const void* buffer);
    ErrorCode flushInternal();

    // 位图操作
    ErrorCode loadInodeBitmap();
    ErrorCode loadBlockBitmap();
    ErrorCode saveInodeBitmap();
    ErrorCode saveBlockBitmap();

    // 内部写入 inode（不加锁）
    ErrorCode writeInodeInternal(InodeId inode_id, const Inode& inode);

    // 数据块号转换
    BlockNo dataBlockToAbsolute(uint32_t data_block_index) const;
    uint32_t absoluteToDataBlock(BlockNo abs_block_no) const;
    bool isValidDataBlock(BlockNo abs_block_no) const;

    // inode 表操作
    BlockNo getInodeBlock(InodeId inode_id) const;
    uint32_t getInodeOffset(InodeId inode_id) const;

    // 位图中查找空闲位
    int32_t findFreeInode() const;
    int32_t findFreeBlock() const;

    // 更新 superblock 统计
    void updateInodeStats(int32_t delta);
    void updateBlockStats(int32_t delta);

    //==========================================================================
    // 成员变量
    //==========================================================================

    DiskImage* disk_;                    // 底层磁盘
    CachedDisk* cached_disk_;            // 可选的缓存磁盘
    bool use_cached_disk_;               // 是否使用缓存
    
    Superblock superblock_;
    bool loaded_;

    // 位图缓存
    std::vector<uint8_t> inode_bitmap_;
    std::vector<uint8_t> block_bitmap_;
    bool inode_bitmap_dirty_;
    bool block_bitmap_dirty_;
    bool superblock_dirty_;

    // 引用计数表
    std::vector<uint8_t> block_refcount_;
    bool refcount_enabled_;

    // 线程安全
    mutable std::mutex mutex_;

    // 统计信息
    AllocStats stats_;
};

} // namespace fs

#endif // ALLOCATOR_H