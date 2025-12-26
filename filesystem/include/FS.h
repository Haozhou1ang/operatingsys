// filesystem/include/FS.h
#ifndef FS_H
#define FS_H

#include "FSTypes.h"
#include "DiskImage.h"
#include "Cache.h"
#include "Allocator.h"
#include "Directory.h"
#include "Snapshot.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <condition_variable>

namespace fs {

/**
 * 文件系统配置选项
 */
struct FSConfig {
    uint32_t cache_capacity = 64;        // 缓存容量（块数）
    bool enable_cache = true;            // 是否启用缓存
    bool write_through = false;          // 写穿透模式
    bool auto_sync = true;               // 自动同步
    uint32_t sync_interval = 0;          // 自动同步间隔（秒，0=禁用）
};

/**
 * 文件系统状态信息
 */
struct FSInfo {
    // 基本信息
    uint32_t block_size;                 // 块大小
    uint32_t total_blocks;               // 总块数
    uint32_t total_inodes;               // 总 inode 数
    
    // 使用情况
    uint32_t free_blocks;                // 空闲块数
    uint32_t used_blocks;                // 已用块数
    uint32_t free_inodes;                // 空闲 inode 数
    uint32_t used_inodes;                // 已用 inode 数
    
    // 容量信息
    uint64_t total_size;                 // 总容量（字节）
    uint64_t free_size;                  // 可用空间（字节）
    uint64_t used_size;                  // 已用空间（字节）
    
    // 快照信息
    uint32_t snapshot_count;             // 快照数量
    uint32_t max_snapshots;              // 最大快照数
    
    // 缓存信息
    CacheStats cache_stats;              // 缓存统计
    
    // 状态
    bool mounted;                        // 是否已挂载
    std::string mount_path;              // 磁盘镜像路径
};

/**
 * FileSystem - 统一的文件系统接口
 * 
 * 整合所有底层模块，提供简洁的 POSIX 风格 API
 */
class FileSystem {
public:
    FileSystem();
    ~FileSystem();

    // 禁止拷贝
    FileSystem(const FileSystem&) = delete;
    FileSystem& operator=(const FileSystem&) = delete;

    //==========================================================================
    // 文件系统生命周期管理
    //==========================================================================

    /**
     * 创建新的文件系统（格式化）
     * @param path 磁盘镜像路径
     * @param total_blocks 总块数
     * @param total_inodes 总 inode 数
     * @return 成功返回 OK
     */
    ErrorCode format(const std::string& path, 
                     uint32_t total_blocks = DEFAULT_TOTAL_BLOCKS,
                     uint32_t total_inodes = DEFAULT_TOTAL_INODES);

    /**
     * 挂载文件系统
     * @param path 磁盘镜像路径
     * @param config 配置选项
     * @return 成功返回 OK
     */
    ErrorCode mount(const std::string& path, const FSConfig& config = FSConfig());

    /**
     * 卸载文件系统
     * @return 成功返回 OK
     */
    ErrorCode unmount();

    /**
     * 检查是否已挂载
     */
    bool isMounted() const { return mounted_; }

    /**
     * 同步所有数据到磁盘
     * @return 成功返回 OK
     */
    ErrorCode sync();

    /**
     * 获取文件系统信息
     */
    FSInfo getInfo() const;

    //==========================================================================
    // 目录操作
    //==========================================================================

    /**
     * 创建目录
     * @param path 目录路径
     * @return 成功返回 OK
     */
    ErrorCode mkdir(const std::string& path);

    /**
     * 删除空目录
     * @param path 目录路径
     * @return 成功返回 OK
     */
    ErrorCode rmdir(const std::string& path);

    /**
     * 列出目录内容
     * @param path 目录路径
     * @return 成功返回目录项列表
     */
    Result<std::vector<DirEntry>> readdir(const std::string& path);

    /**
     * 递归创建目录（类似 mkdir -p）
     * @param path 目录路径
     * @return 成功返回 OK
     */
    ErrorCode mkdirp(const std::string& path);

    //==========================================================================
    // 文件操作
    //==========================================================================

    /**
     * 创建文件
     * @param path 文件路径
     * @return 成功返回 OK
     */
    ErrorCode create(const std::string& path);

    /**
     * 删除文件
     * @param path 文件路径
     * @return 成功返回 OK
     */
    ErrorCode unlink(const std::string& path);

    /**
     * 删除文件或空目录
     * @param path 路径
     * @return 成功返回 OK
     */
    ErrorCode remove(const std::string& path);

    /**
     * 读取文件内容
     * @param path 文件路径
     * @param offset 起始偏移（默认 0）
     * @param length 读取长度（默认 0 = 全部）
     * @return 成功返回文件数据
     */
    Result<std::vector<uint8_t>> readFile(const std::string& path,
                                           uint32_t offset = 0,
                                           uint32_t length = 0);

    /**
     * 读取文件为字符串
     * @param path 文件路径
     * @return 成功返回文件内容字符串
     */
    Result<std::string> readFileAsString(const std::string& path);

    /**
     * 写入文件内容
     * @param path 文件路径
     * @param data 数据
     * @param offset 起始偏移（默认 0）
     * @return 成功返回写入字节数
     */
    Result<uint32_t> writeFile(const std::string& path,
                                const std::vector<uint8_t>& data,
                                uint32_t offset = 0);

    /**
     * 写入字符串到文件
     * @param path 文件路径
     * @param content 字符串内容
     * @param offset 起始偏移（默认 0）
     * @return 成功返回写入字节数
     */
    Result<uint32_t> writeFile(const std::string& path,
                                const std::string& content,
                                uint32_t offset = 0);

    /**
     * 追加数据到文件
     * @param path 文件路径
     * @param data 数据
     * @return 成功返回写入字节数
     */
    Result<uint32_t> appendFile(const std::string& path,
                                 const std::vector<uint8_t>& data);

    /**
     * 追加字符串到文件
     * @param path 文件路径
     * @param content 字符串内容
     * @return 成功返回写入字节数
     */
    Result<uint32_t> appendFile(const std::string& path,
                                 const std::string& content);

    /**
     * 截断文件
     * @param path 文件路径
     * @param size 新大小
     * @return 成功返回 OK
     */
    ErrorCode truncate(const std::string& path, uint32_t size);

    /**
     * 复制文件
     * @param src 源路径
     * @param dst 目标路径
     * @return 成功返回 OK
     */
    ErrorCode copyFile(const std::string& src, const std::string& dst);

    /**
     * 移动/重命名文件
     * @param src 源路径
     * @param dst 目标路径
     * @return 成功返回 OK
     */
    ErrorCode moveFile(const std::string& src, const std::string& dst);

    //==========================================================================
    // 文件/目录信息
    //==========================================================================

    /**
     * 获取文件/目录状态
     * @param path 路径
     * @return 成功返回 FileStat
     */
    Result<FileStat> stat(const std::string& path);

    /**
     * 检查路径是否存在
     * @param path 路径
     * @return 存在返回 true
     */
    bool exists(const std::string& path);

    /**
     * 检查是否是目录
     * @param path 路径
     * @return 是目录返回 true
     */
    bool isDirectory(const std::string& path);

    /**
     * 检查是否是文件
     * @param path 路径
     * @return 是文件返回 true
     */
    bool isFile(const std::string& path);

    /**
     * 获取文件大小
     * @param path 文件路径
     * @return 成功返回文件大小
     */
    Result<uint32_t> getFileSize(const std::string& path);

    //==========================================================================
    // 快照操作
    //==========================================================================

    /**
     * 创建快照
     * @param name 快照名称
     * @return 成功返回 OK
     */
    ErrorCode createSnapshot(const std::string& name);

    /**
     * 恢复快照
     * @param name 快照名称
     * @return 成功返回 OK
     */
    ErrorCode restoreSnapshot(const std::string& name);

    /**
     * 删除快照
     * @param name 快照名称
     * @return 成功返回 OK
     */
    ErrorCode deleteSnapshot(const std::string& name);

    /**
     * 列出所有快照
     * @return 快照信息列表
     */
    std::vector<SnapshotInfo> listSnapshots() const;

    /**
     * 检查快照是否存在
     * @param name 快照名称
     * @return 存在返回 true
     */
    bool snapshotExists(const std::string& name) const;

    //==========================================================================
    // 缓存控制
    //==========================================================================

    /**
     * 获取缓存统计
     */
    CacheStats getCacheStats() const;

    /**
     * 重置缓存统计
     */
    void resetCacheStats();

    /**
     * 清空缓存
     * @return 成功返回 OK
     */
    ErrorCode clearCache();

    /**
     * 设置缓存容量
     * @param capacity 新容量（块数）
     */
    void setCacheCapacity(uint32_t capacity);

    /**
     * 启用/禁用缓存
     * @param enabled 是否启用
     */
    void setCacheEnabled(bool enabled);

    //==========================================================================
    // 实用工具
    //==========================================================================

    /**
     * 递归遍历目录
     * @param path 起始路径
     * @param callback 回调函数 (path, stat) -> bool，返回 false 停止遍历
     * @return 成功返回 OK
     */
    ErrorCode walk(const std::string& path,
                   std::function<bool(const std::string&, const FileStat&)> callback);

    /**
     * 递归删除目录及内容
     * @param path 目录路径
     * @return 成功返回 OK
     */
    ErrorCode removeRecursive(const std::string& path);

    /**
     * 获取目录占用空间
     * @param path 目录路径
     * @return 成功返回总字节数
     */
    Result<uint64_t> getDirSize(const std::string& path);

    /**
     * 检查文件系统一致性
     * @param fix 是否尝试修复
     * @return 一致返回 OK
     */
    ErrorCode checkConsistency(bool fix = false);

    /**
     * 打印目录树（调试用）
     * @param path 起始路径
     * @param max_depth 最大深度（0=无限）
     */
    void printTree(const std::string& path = "/", int max_depth = 0);

private:
    void endActiveOp();
    //==========================================================================
    // 内部辅助方法
    //==========================================================================

    ErrorCode ensureMounted();
    std::string normalizePath(const std::string& path);
    std::vector<std::string> splitPath(const std::string& path);
    std::string getParentPath(const std::string& path);
    std::string getBaseName(const std::string& path);
    void printTreeRecursive(const std::string& path, const std::string& prefix, 
                            int depth, int max_depth);

    //==========================================================================
    // 成员变量
    //==========================================================================

    std::unique_ptr<DiskImage> disk_;
    std::unique_ptr<CachedDisk> cached_disk_;
    std::unique_ptr<Allocator> alloc_;
    std::unique_ptr<Directory> dir_;
    std::unique_ptr<SnapshotManager> snap_;

    bool mounted_;
    std::string mount_path_;
    FSConfig config_;

    mutable std::mutex mutex_;
    std::condition_variable op_cv_;
    std::mutex op_mu_;
    std::atomic<int> active_ops_{0};
    bool unmounting_ = false;
};

//==============================================================================
// 便捷函数
//==============================================================================

/**
 * 快速创建并格式化文件系统
 */
inline ErrorCode quickFormat(const std::string& path, 
                             uint32_t size_mb = 16,
                             uint32_t inodes = 1024) {
    FileSystem fs;
    uint32_t blocks = (size_mb * 1024 * 1024) / BLOCK_SIZE;
    return fs.format(path, blocks, inodes);
}

/**
 * 检查文件系统是否有效
 */
inline bool isValidFS(const std::string& path) {
    return checkfs(path);
}

} // namespace fs

#endif // FS_H
