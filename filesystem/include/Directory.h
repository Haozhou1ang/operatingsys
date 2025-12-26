// filesystem/include/Directory.h
#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "FSTypes.h"
#include "Allocator.h"
#include <string>
#include <vector>
#include <mutex>

namespace fs {

// 前向声明
class CachedDisk;
class SnapshotManager;

/**
 * Directory - 目录操作管理器
 */
class Directory {
public:
    /**
     * 构造函数（使用裸磁盘）
     */
    Directory(Allocator* alloc, DiskImage* disk);
    
    /**
     * 构造函数（使用带缓存的磁盘）
     */
    Directory(Allocator* alloc, CachedDisk* cached_disk);
    
    ~Directory();

    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;

    //==========================================================================
    // 路径解析接口
    //==========================================================================

    Result<InodeId> resolvePath(const std::string& path);

    struct ParentInfo {
        InodeId parent_inode;
        std::string filename;
    };
    Result<ParentInfo> resolveParent(const std::string& path);

    Result<FileStat> stat(const std::string& path);
    Result<FileStat> statInode(InodeId inode_id);

    //==========================================================================
    // 目录操作接口
    //==========================================================================

    Result<DirEntry> lookup(InodeId dir_inode, const std::string& name);
    ErrorCode addEntry(InodeId dir_inode, const std::string& name, 
                       InodeId target_inode, FileType type);
    ErrorCode removeEntry(InodeId dir_inode, const std::string& name);
    Result<std::vector<DirEntry>> listDirectory(InodeId dir_inode);
    Result<std::vector<DirEntry>> list(const std::string& path);
    bool isDirectoryEmpty(InodeId dir_inode);

    //==========================================================================
    // 文件/目录创建与删除
    //==========================================================================

    Result<InodeId> mkdir(const std::string& path);
    ErrorCode rmdir(const std::string& path);
    Result<InodeId> createFile(const std::string& path);
    ErrorCode removeFile(const std::string& path);
    ErrorCode remove(const std::string& path);

    //==========================================================================
    // 文件读写接口
    //==========================================================================

    Result<std::vector<uint8_t>> readFile(const std::string& path, 
                                          uint32_t offset = 0, 
                                          uint32_t length = 0);
    Result<std::vector<uint8_t>> readFileByInode(InodeId inode_id,
                                                  uint32_t offset = 0,
                                                  uint32_t length = 0);
    Result<uint32_t> writeFile(const std::string& path,
                               const std::vector<uint8_t>& data,
                               uint32_t offset = 0);
    Result<uint32_t> writeFile(const std::string& path,
                               const std::string& data,
                               uint32_t offset = 0);
    Result<uint32_t> writeFileByInode(InodeId inode_id,
                                       const std::vector<uint8_t>& data,
                                       uint32_t offset = 0);
    ErrorCode truncate(const std::string& path, uint32_t new_size);
    Result<uint32_t> appendFile(const std::string& path,
                                const std::vector<uint8_t>& data);

    //==========================================================================
    // 辅助接口
    //==========================================================================

    bool exists(const std::string& path);
    bool isDirectory(const std::string& path);
    bool isFile(const std::string& path);
    ErrorCode sync();

    //==========================================================================
    // 缓存控制接口
    //==========================================================================

    /**
     * 获取缓存统计（如果使用缓存）
     */
    CacheStats getCacheStats() const;
    
    /**
     * 刷新缓存
     */
    ErrorCode flushCache();
    
    /**
     * 检查是否使用缓存
     */
    bool isCacheEnabled() const { return use_cached_disk_; }

    void setSnapshotManager(SnapshotManager* snap) { snap_ = snap; }

private:
    //==========================================================================
    // 内部辅助方法
    //==========================================================================

    // 路径处理
    std::vector<std::string> splitPath(const std::string& path);
    bool isValidPath(const std::string& path);
    bool isValidFilename(const std::string& name);
    std::string normalizePath(const std::string& path);

    // 内部查找（不加锁）
    Result<DirEntry> lookupInternal(InodeId dir_inode, const std::string& name);

    // 统一的块读写接口
    ErrorCode readBlockInternal(BlockNo block_no, void* buffer);
    ErrorCode writeBlockInternal(BlockNo block_no, const void* buffer);

    // 目录块操作
    ErrorCode readDirectoryBlock(BlockNo block_no, DirEntry* entries);
    ErrorCode writeDirectoryBlock(BlockNo block_no, const DirEntry* entries);

    // 文件块操作
    Result<BlockNo> getFileBlock(const Inode& inode, uint32_t block_index);
    Result<BlockNo> getOrAllocFileBlock(Inode& inode, InodeId inode_id, uint32_t block_index);
    ErrorCode freeFileBlocks(Inode& inode, uint32_t from_block);
    Result<BlockNo> cowDataBlockIfNeeded(Inode& inode, InodeId inode_id,
                                         uint32_t block_index, BlockNo block_no);
    ErrorCode updateFileBlockPointer(Inode& inode, InodeId inode_id,
                                     uint32_t block_index, BlockNo new_block);

    // 间接块操作
    Result<BlockNo> getIndirectBlock(BlockNo indirect_block, uint32_t index);
    ErrorCode setIndirectBlock(BlockNo indirect_block, uint32_t index, BlockNo value);
    Result<BlockNo> allocIndirectBlock();

    // inode 操作
    Result<Inode> readInode(InodeId inode_id);
    ErrorCode writeInode(InodeId inode_id, const Inode& inode);

    // 时间更新
    void updateAccessTime(Inode& inode);
    void updateModifyTime(Inode& inode);
    int64_t currentTime();

    //==========================================================================
    // 成员变量
    //==========================================================================

    Allocator* alloc_;
    DiskImage* disk_;
    CachedDisk* cached_disk_;
    bool use_cached_disk_;
    SnapshotManager* snap_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace fs

#endif // DIRECTORY_H
