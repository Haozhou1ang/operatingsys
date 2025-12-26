// filesystem/include/DiskImage.h
#ifndef DISKIMAGE_H
#define DISKIMAGE_H

#include "FSTypes.h"
#include <string>
#include <fstream>
#include <mutex>
#include <vector>
#include <memory>

namespace fs {

/**
 * DiskImage - 磁盘镜像管理类
 * 
 * 负责：
 * 1. 磁盘镜像文件的创建、打开、关闭
 * 2. 块级别的读写操作
 * 3. Superblock 的加载和保存
 * 4. 线程安全的 I/O 操作
 */
class DiskImage {
public:
    DiskImage();
    ~DiskImage();

    // 禁止拷贝
    DiskImage(const DiskImage&) = delete;
    DiskImage& operator=(const DiskImage&) = delete;

    //==========================================================================
    // 文件管理接口
    //==========================================================================

    /**
     * 创建新的磁盘镜像文件
     * @param path 文件路径
     * @param total_blocks 总块数
     * @return 成功返回 OK
     */
    ErrorCode create(const std::string& path, uint32_t total_blocks);

    /**
     * 打开已存在的磁盘镜像文件
     * @param path 文件路径
     * @return 成功返回 OK
     */
    ErrorCode open(const std::string& path);

    /**
     * 关闭磁盘镜像文件
     */
    void close();

    /**
     * 检查是否已打开
     */
    bool isOpen() const { return is_open_; }

    /**
     * 同步所有数据到磁盘
     */
    ErrorCode sync();

    //==========================================================================
    // 块级读写接口
    //==========================================================================

    /**
     * 读取一个块
     * @param block_no 块编号
     * @param buffer 输出缓冲区，大小必须 >= BLOCK_SIZE
     * @return 成功返回 OK
     */
    ErrorCode readBlock(BlockNo block_no, void* buffer);

    /**
     * 写入一个块
     * @param block_no 块编号
     * @param buffer 输入缓冲区，大小必须 >= BLOCK_SIZE
     * @return 成功返回 OK
     */
    ErrorCode writeBlock(BlockNo block_no, const void* buffer);

    /**
     * 读取多个连续块
     * @param start_block 起始块编号
     * @param count 块数量
     * @param buffer 输出缓冲区
     * @return 成功返回 OK
     */
    ErrorCode readBlocks(BlockNo start_block, uint32_t count, void* buffer);

    /**
     * 写入多个连续块
     * @param start_block 起始块编号
     * @param count 块数量
     * @param buffer 输入缓冲区
     * @return 成功返回 OK
     */
    ErrorCode writeBlocks(BlockNo start_block, uint32_t count, const void* buffer);

    /**
     * 清零一个块
     * @param block_no 块编号
     * @return 成功返回 OK
     */
    ErrorCode zeroBlock(BlockNo block_no);

    /**
     * 清零多个连续块
     * @param start_block 起始块编号
     * @param count 块数量
     * @return 成功返回 OK
     */
    ErrorCode zeroBlocks(BlockNo start_block, uint32_t count);

    //==========================================================================
    // Superblock 专用接口
    //==========================================================================

    /**
     * 加载 Superblock
     * @param sb 输出 Superblock
     * @return 成功返回 OK
     */
    ErrorCode loadSuperblock(Superblock& sb);

    /**
     * 保存 Superblock
     * @param sb 输入 Superblock
     * @return 成功返回 OK
     */
    ErrorCode saveSuperblock(const Superblock& sb);

    //==========================================================================
    // 状态查询接口
    //==========================================================================

    /**
     * 获取总块数
     */
    uint32_t getTotalBlocks() const { return total_blocks_; }

    /**
     * 获取文件路径
     */
    const std::string& getPath() const { return path_; }

    /**
     * 获取 I/O 统计
     */
    struct IOStats {
        uint64_t reads;          // 读取块次数
        uint64_t writes;         // 写入块次数
        uint64_t bytes_read;     // 读取字节数
        uint64_t bytes_written;  // 写入字节数
    };
    IOStats getIOStats() const;
    void resetIOStats();

private:
    // 内部辅助方法
    ErrorCode seekToBlock(BlockNo block_no);
    bool validateBlockNo(BlockNo block_no) const;

    // 成员变量
    std::string path_;              // 文件路径
    std::fstream file_;             // 文件流
    bool is_open_;                  // 是否已打开
    uint32_t total_blocks_;         // 总块数

    // I/O 统计
    mutable std::mutex stats_mutex_;
    IOStats stats_;

    // 文件操作互斥锁
    mutable std::mutex io_mutex_;

    // 零块缓冲区（用于清零操作）
    static const uint8_t zero_block_[BLOCK_SIZE];
};

//==============================================================================
// mkfs - 文件系统格式化工具
//==============================================================================

/**
 * 格式化选项
 */
struct MkfsOptions {
    uint32_t total_blocks = DEFAULT_TOTAL_BLOCKS;   // 总块数
    uint32_t total_inodes = DEFAULT_TOTAL_INODES;   // 总 inode 数
    bool force = false;                              // 强制覆盖已存在文件
    bool verbose = false;                            // 输出详细信息
};

/**
 * 格式化结果信息
 */
struct MkfsResult {
    ErrorCode error = ErrorCode::OK;
    uint32_t total_blocks = 0;
    uint32_t total_inodes = 0;
    uint32_t data_blocks = 0;
    BlockNo data_start = 0;
    std::string message;
};

/**
 * 格式化磁盘镜像，创建新的文件系统
 * @param path 磁盘镜像路径
 * @param options 格式化选项
 * @return 格式化结果
 */
MkfsResult mkfs(const std::string& path, const MkfsOptions& options = MkfsOptions());

/**
 * 检查文件系统格式是否有效
 * @param path 磁盘镜像路径
 * @return 有效返回 true
 */
bool checkfs(const std::string& path);

} // namespace fs

#endif // DISKIMAGE_H