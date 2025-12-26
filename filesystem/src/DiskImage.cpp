// filesystem/src/DiskImage.cpp
#include "DiskImage.h"
#include <cstring>
#include <ctime>
#include <iostream>
#include <sys/stat.h>

namespace fs {

// 静态零块缓冲区
const uint8_t DiskImage::zero_block_[BLOCK_SIZE] = {0};

//==============================================================================
// 构造与析构
//==============================================================================

DiskImage::DiskImage()
    : is_open_(false)
    , total_blocks_(0)
    , stats_{0, 0, 0, 0}
{
}

DiskImage::~DiskImage() {
    close();
}

//==============================================================================
// 文件管理接口
//==============================================================================

ErrorCode DiskImage::create(const std::string& path, uint32_t total_blocks) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (is_open_) {
        close();
    }

    // 打开文件（二进制模式，截断已有内容）
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        // 如果文件不存在，尝试创建
        file_.clear();
        file_.open(path, std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            return ErrorCode::E_IO;
        }
        file_.close();
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_.is_open()) {
            return ErrorCode::E_IO;
        }
    }

    // 预分配文件空间
    uint64_t file_size = static_cast<uint64_t>(total_blocks) * BLOCK_SIZE;
    
    // 写入最后一个字节以扩展文件
    file_.seekp(file_size - 1);
    if (!file_.good()) {
        file_.close();
        return ErrorCode::E_IO;
    }
    
    char zero = 0;
    file_.write(&zero, 1);
    if (!file_.good()) {
        file_.close();
        return ErrorCode::E_IO;
    }

    // 初始化所有块为零
    file_.seekp(0);
    std::vector<uint8_t> zero_buffer(BLOCK_SIZE, 0);
    for (uint32_t i = 0; i < total_blocks; ++i) {
        file_.write(reinterpret_cast<char*>(zero_buffer.data()), BLOCK_SIZE);
        if (!file_.good()) {
            file_.close();
            return ErrorCode::E_IO;
        }
    }

    file_.flush();

    path_ = path;
    total_blocks_ = total_blocks;
    is_open_ = true;
    resetIOStats();

    return ErrorCode::OK;
}

ErrorCode DiskImage::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (is_open_) {
        close();
    }

    // 检查文件是否存在
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return ErrorCode::E_NOT_FOUND;
    }

    // 打开文件
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) {
        return ErrorCode::E_IO;
    }

    // 获取文件大小
    file_.seekg(0, std::ios::end);
    std::streampos file_size = file_.tellg();
    file_.seekg(0, std::ios::beg);

    if (file_size < static_cast<std::streampos>(BLOCK_SIZE)) {
        file_.close();
        return ErrorCode::E_INVALID_PARAM;
    }

    // 读取 Superblock 获取总块数
    Superblock sb;
    file_.read(reinterpret_cast<char*>(&sb), sizeof(Superblock));
    if (!file_.good()) {
        file_.close();
        return ErrorCode::E_IO;
    }

    // 验证 Superblock
    if (!sb.validate()) {
        file_.close();
        return ErrorCode::E_INVALID_PARAM;
    }

    path_ = path;
    total_blocks_ = sb.total_blocks;
    is_open_ = true;
    resetIOStats();

    return ErrorCode::OK;
}

void DiskImage::close() {
    // 注意：这里不加锁，因为可能在析构函数中调用
    // 调用者需要确保线程安全
    if (is_open_) {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        is_open_ = false;
        total_blocks_ = 0;
        path_.clear();
    }
}

ErrorCode DiskImage::sync() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    
    if (!is_open_) {
        return ErrorCode::E_IO;
    }

    file_.flush();
    return file_.good() ? ErrorCode::OK : ErrorCode::E_IO;
}

//==============================================================================
// 块级读写接口
//==============================================================================

ErrorCode DiskImage::readBlock(BlockNo block_no, void* buffer) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (!is_open_ || buffer == nullptr) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (!validateBlockNo(block_no)) {
        return ErrorCode::E_INVALID_PARAM;
    }

    ErrorCode err = seekToBlock(block_no);
    if (err != ErrorCode::OK) {
        return err;
    }

    file_.read(reinterpret_cast<char*>(buffer), BLOCK_SIZE);
    if (!file_.good() && !file_.eof()) {
        file_.clear();
        return ErrorCode::E_IO;
    }

    // 更新统计
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.reads++;
        stats_.bytes_read += BLOCK_SIZE;
    }

    return ErrorCode::OK;
}

ErrorCode DiskImage::writeBlock(BlockNo block_no, const void* buffer) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    if (!is_open_ || buffer == nullptr) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (!validateBlockNo(block_no)) {
        return ErrorCode::E_INVALID_PARAM;
    }

    ErrorCode err = seekToBlock(block_no);
    if (err != ErrorCode::OK) {
        return err;
    }

    file_.write(reinterpret_cast<const char*>(buffer), BLOCK_SIZE);
    if (!file_.good()) {
        file_.clear();
        return ErrorCode::E_IO;
    }

    // 更新统计
    {
        std::lock_guard<std::mutex> slock(stats_mutex_);
        stats_.writes++;
        stats_.bytes_written += BLOCK_SIZE;
    }

    return ErrorCode::OK;
}

ErrorCode DiskImage::readBlocks(BlockNo start_block, uint32_t count, void* buffer) {
    if (count == 0) {
        return ErrorCode::OK;
    }

    uint8_t* ptr = reinterpret_cast<uint8_t*>(buffer);
    for (uint32_t i = 0; i < count; ++i) {
        ErrorCode err = readBlock(start_block + i, ptr + i * BLOCK_SIZE);
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    return ErrorCode::OK;
}

ErrorCode DiskImage::writeBlocks(BlockNo start_block, uint32_t count, const void* buffer) {
    if (count == 0) {
        return ErrorCode::OK;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(buffer);
    for (uint32_t i = 0; i < count; ++i) {
        ErrorCode err = writeBlock(start_block + i, ptr + i * BLOCK_SIZE);
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    return ErrorCode::OK;
}

ErrorCode DiskImage::zeroBlock(BlockNo block_no) {
    return writeBlock(block_no, zero_block_);
}

ErrorCode DiskImage::zeroBlocks(BlockNo start_block, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        ErrorCode err = zeroBlock(start_block + i);
        if (err != ErrorCode::OK) {
            return err;
        }
    }
    return ErrorCode::OK;
}

//==============================================================================
// Superblock 专用接口
//==============================================================================

ErrorCode DiskImage::loadSuperblock(Superblock& sb) {
    ErrorCode err = readBlock(0, &sb);
    if (err != ErrorCode::OK) {
        return err;
    }

    if (!sb.validate()) {
        return ErrorCode::E_INVALID_PARAM;
    }

    return ErrorCode::OK;
}

ErrorCode DiskImage::saveSuperblock(const Superblock& sb) {
    if (!sb.validate()) {
        return ErrorCode::E_INVALID_PARAM;
    }
    return writeBlock(0, &sb);
}

//==============================================================================
// 统计接口
//==============================================================================

DiskImage::IOStats DiskImage::getIOStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}

void DiskImage::resetIOStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_ = {0, 0, 0, 0};
}

//==============================================================================
// 内部辅助方法
//==============================================================================

ErrorCode DiskImage::seekToBlock(BlockNo block_no) {
    std::streampos offset = static_cast<std::streampos>(block_no) * BLOCK_SIZE;
    file_.seekg(offset);
    file_.seekp(offset);
    
    if (!file_.good()) {
        file_.clear();
        return ErrorCode::E_IO;
    }
    
    return ErrorCode::OK;
}

bool DiskImage::validateBlockNo(BlockNo block_no) const {
    return block_no < total_blocks_;
}

//==============================================================================
// mkfs 实现
//==============================================================================

MkfsResult mkfs(const std::string& path, const MkfsOptions& options) {
    MkfsResult result;
    
    // 参数验证
    if (options.total_blocks < 100) {
        result.error = ErrorCode::E_INVALID_PARAM;
        result.message = "Total blocks must be at least 100";
        return result;
    }
    
    if (options.total_inodes < 16) {
        result.error = ErrorCode::E_INVALID_PARAM;
        result.message = "Total inodes must be at least 16";
        return result;
    }

    // 检查文件是否存在
    if (!options.force) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            result.error = ErrorCode::E_ALREADY_EXISTS;
            result.message = "File already exists. Use force=true to overwrite";
            return result;
        }
    }

    if (options.verbose) {
        std::cout << "Creating filesystem at: " << path << std::endl;
        std::cout << "  Total blocks: " << options.total_blocks << std::endl;
        std::cout << "  Total inodes: " << options.total_inodes << std::endl;
        std::cout << "  Block size: " << BLOCK_SIZE << " bytes" << std::endl;
    }

    // 创建磁盘镜像
    DiskImage disk;
    ErrorCode err = disk.create(path, options.total_blocks);
    if (err != ErrorCode::OK) {
        result.error = err;
        result.message = "Failed to create disk image";
        return result;
    }

    // 初始化 Superblock
    Superblock sb;
    sb.init(options.total_blocks, options.total_inodes);
    sb.create_time = std::time(nullptr);
    sb.mount_time = sb.create_time;
    sb.write_time = sb.create_time;

    if (options.verbose) {
        std::cout << "Layout:" << std::endl;
        std::cout << "  Superblock: block 0" << std::endl;
        std::cout << "  Inode bitmap: blocks " << sb.inode_bitmap_start 
                  << " - " << (sb.inode_bitmap_start + sb.inode_bitmap_blocks - 1) << std::endl;
        std::cout << "  Block bitmap: blocks " << sb.block_bitmap_start 
                  << " - " << (sb.block_bitmap_start + sb.block_bitmap_blocks - 1) << std::endl;
        std::cout << "  Inode table: blocks " << sb.inode_table_start 
                  << " - " << (sb.data_block_start - 1) << std::endl;
        std::cout << "  Data blocks: blocks " << sb.data_block_start 
                  << " - " << (options.total_blocks - 1) << std::endl;
        std::cout << "  Available data blocks: " << sb.data_block_count << std::endl;
    }

    // =========================================================================
    // 初始化 Inode 位图
    // =========================================================================
    
    // 分配 inode 0 给根目录
    std::vector<uint8_t> inode_bitmap(sb.inode_bitmap_blocks * BLOCK_SIZE, 0);
    Bitmap inode_bmap(inode_bitmap.data(), options.total_inodes);
    inode_bmap.set(ROOT_INODE);  // 分配 inode 0

    // 写入 inode 位图
    for (uint32_t i = 0; i < sb.inode_bitmap_blocks; ++i) {
        err = disk.writeBlock(sb.inode_bitmap_start + i, 
                             inode_bitmap.data() + i * BLOCK_SIZE);
        if (err != ErrorCode::OK) {
            result.error = err;
            result.message = "Failed to write inode bitmap";
            return result;
        }
    }

    // =========================================================================
    // 初始化 Block 位图
    // =========================================================================
    
    // 分配第一个数据块给根目录
    std::vector<uint8_t> block_bitmap(sb.block_bitmap_blocks * BLOCK_SIZE, 0);
    Bitmap block_bmap(block_bitmap.data(), sb.data_block_count);
    block_bmap.set(0);  // 分配第一个数据块

    // 写入 block 位图
    for (uint32_t i = 0; i < sb.block_bitmap_blocks; ++i) {
        err = disk.writeBlock(sb.block_bitmap_start + i, 
                             block_bitmap.data() + i * BLOCK_SIZE);
        if (err != ErrorCode::OK) {
            result.error = err;
            result.message = "Failed to write block bitmap";
            return result;
        }
    }

    // =========================================================================
    // 初始化根目录 Inode
    // =========================================================================
    
    // 准备 inode 表的第一个块
    std::vector<uint8_t> inode_block(BLOCK_SIZE, 0);
    Inode* inodes = reinterpret_cast<Inode*>(inode_block.data());

    // 设置根目录 inode
    Inode& root_inode = inodes[ROOT_INODE];
    root_inode.init(FileType::DIRECTORY);
    root_inode.create_time = sb.create_time;
    root_inode.modify_time = sb.create_time;
    root_inode.access_time = sb.create_time;
    root_inode.size = 2 * sizeof(DirEntry);  // . 和 ..
    root_inode.link_count = 2;  // 自身 + .
    root_inode.direct_blocks[0] = sb.data_block_start;  // 根目录内容在第一个数据块

    // 写入 inode 表第一个块
    err = disk.writeBlock(sb.inode_table_start, inode_block.data());
    if (err != ErrorCode::OK) {
        result.error = err;
        result.message = "Failed to write root inode";
        return result;
    }

    // =========================================================================
    // 初始化根目录内容
    // =========================================================================
    
    std::vector<uint8_t> root_dir_block(BLOCK_SIZE, 0);
    DirEntry* entries = reinterpret_cast<DirEntry*>(root_dir_block.data());

    // "." 指向自身
    entries[0].init(ROOT_INODE, ".", FileType::DIRECTORY);
    
    // ".." 指向自身（根目录的父目录是自身）
    entries[1].init(ROOT_INODE, "..", FileType::DIRECTORY);

    // 标记其余目录项为无效
    for (uint32_t i = 2; i < DIRENTRIES_PER_BLOCK; ++i) {
        entries[i].inode = INVALID_INODE;
    }

    // 写入根目录数据块
    err = disk.writeBlock(sb.data_block_start, root_dir_block.data());
    if (err != ErrorCode::OK) {
        result.error = err;
        result.message = "Failed to write root directory";
        return result;
    }

    // =========================================================================
    // 更新并写入 Superblock
    // =========================================================================
    
    sb.free_inodes = options.total_inodes - 1;  // 减去根目录
    sb.used_inodes = 1;
    sb.free_blocks = sb.data_block_count - 1;   // 减去根目录数据块
    sb.used_blocks = 1;

    err = disk.saveSuperblock(sb);
    if (err != ErrorCode::OK) {
        result.error = err;
        result.message = "Failed to write superblock";
        return result;
    }

    // 同步到磁盘
    disk.sync();
    disk.close();

    // 填充结果
    result.error = ErrorCode::OK;
    result.total_blocks = options.total_blocks;
    result.total_inodes = options.total_inodes;
    result.data_blocks = sb.data_block_count - 1;
    result.data_start = sb.data_block_start;
    result.message = "Filesystem created successfully";

    if (options.verbose) {
        std::cout << "Filesystem created successfully!" << std::endl;
        std::cout << "  Free data blocks: " << result.data_blocks << std::endl;
        std::cout << "  Free inodes: " << sb.free_inodes << std::endl;
    }

    return result;
}

bool checkfs(const std::string& path) {
    DiskImage disk;
    ErrorCode err = disk.open(path);
    if (err != ErrorCode::OK) {
        return false;
    }

    Superblock sb;
    err = disk.loadSuperblock(sb);
    if (err != ErrorCode::OK) {
        return false;
    }

    return sb.validate();
}

} // namespace fs