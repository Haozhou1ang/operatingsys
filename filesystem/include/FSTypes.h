// filesystem/include/FSTypes.h
#ifndef FSTYPES_H
#define FSTYPES_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fs {

//==============================================================================
// 基础常量定义
//==============================================================================

constexpr uint32_t BLOCK_SIZE = 1024;                    // 块大小：1KB
constexpr uint32_t MAGIC_NUMBER = 0x53465352;            // "RSFS" - Review System FS
constexpr uint32_t FS_VERSION = 1;

// 默认文件系统规模（可在 mkfs 时调整）
constexpr uint32_t DEFAULT_TOTAL_BLOCKS = 16384;         // 16MB 总空间
constexpr uint32_t DEFAULT_TOTAL_INODES = 1024;          // 最多 1024 个文件/目录

// inode 中的块指针数量
constexpr uint32_t NUM_DIRECT_BLOCKS = 12;               // 12 个直接块指针
constexpr uint32_t NUM_SINGLE_INDIRECT = 1;              // 1 个一级间接块
constexpr uint32_t NUM_DOUBLE_INDIRECT = 1;              // 1 个二级间接块

// 每个间接块能存储的块指针数量
constexpr uint32_t PTRS_PER_BLOCK = BLOCK_SIZE / sizeof(uint32_t);  // 256

// 文件名最大长度
constexpr uint32_t MAX_FILENAME_LEN = 56;

// 特殊值
constexpr uint32_t INVALID_BLOCK = 0xFFFFFFFF;
constexpr uint32_t INVALID_INODE = 0xFFFFFFFF;
constexpr uint32_t ROOT_INODE = 0;                       // 根目录 inode 编号

//==============================================================================
// 类型别名
//==============================================================================

using BlockNo = uint32_t;                                // 块编号
using InodeId = uint32_t;                                // Inode 编号

//==============================================================================
// 错误码定义
//==============================================================================

enum class ErrorCode : int32_t {
    OK = 0,
    
    // 通用错误
    E_IO = -1,                    // I/O 错误
    E_INTERNAL = -2,              // 内部错误
    E_INVALID_PARAM = -3,         // 参数无效
    
    // 路径/文件错误
    E_NOT_FOUND = -10,            // 文件/目录不存在
    E_ALREADY_EXISTS = -11,       // 文件/目录已存在
    E_NOT_DIR = -12,              // 不是目录
    E_IS_DIR = -13,               // 是目录（期望文件）
    E_NOT_EMPTY = -14,            // 目录非空
    E_INVALID_PATH = -15,         // 路径格式无效
    E_NAME_TOO_LONG = -16,        // 文件名过长
    
    // 资源错误
    E_NO_SPACE = -20,             // 磁盘空间不足
    E_NO_INODE = -21,             // inode 耗尽
    E_FILE_TOO_LARGE = -22,       // 文件过大
    
    // 权限错误
    E_PERMISSION = -30,           // 权限不足
    
    // 快照错误
    E_SNAPSHOT_NOT_FOUND = -40,   // 快照不存在
    E_SNAPSHOT_EXISTS = -41,      // 快照已存在
    E_MAX_SNAPSHOTS = -42,        // 快照数量已达上限
};

// 错误码转字符串
inline const char* errorCodeToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::OK: return "Success";
        case ErrorCode::E_IO: return "I/O error";
        case ErrorCode::E_INTERNAL: return "Internal error";
        case ErrorCode::E_INVALID_PARAM: return "Invalid parameter";
        case ErrorCode::E_NOT_FOUND: return "Not found";
        case ErrorCode::E_ALREADY_EXISTS: return "Already exists";
        case ErrorCode::E_NOT_DIR: return "Not a directory";
        case ErrorCode::E_IS_DIR: return "Is a directory";
        case ErrorCode::E_NOT_EMPTY: return "Directory not empty";
        case ErrorCode::E_INVALID_PATH: return "Invalid path";
        case ErrorCode::E_NAME_TOO_LONG: return "Name too long";
        case ErrorCode::E_NO_SPACE: return "No space left";
        case ErrorCode::E_NO_INODE: return "No inode available";
        case ErrorCode::E_FILE_TOO_LARGE: return "File too large";
        case ErrorCode::E_PERMISSION: return "Permission denied";
        case ErrorCode::E_SNAPSHOT_NOT_FOUND: return "Snapshot not found";
        case ErrorCode::E_SNAPSHOT_EXISTS: return "Snapshot exists";
        case ErrorCode::E_MAX_SNAPSHOTS: return "Max snapshots reached";
        default: return "Unknown error";
    }
}

//==============================================================================
// Result 模板类 - 用于返回结果或错误
//==============================================================================

template<typename T>
class Result {
public:
    // 成功构造
    static Result success(T value) {
        Result r;
        r.value_ = std::move(value);
        r.error_ = ErrorCode::OK;
        return r;
    }
    
    // 失败构造
    static Result failure(ErrorCode error) {
        Result r;
        r.error_ = error;
        return r;
    }
    
    bool ok() const { return error_ == ErrorCode::OK; }
    ErrorCode error() const { return error_; }
    
    T& value() { return value_; }
    const T& value() const { return value_; }
    
    // 便捷访问
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    T& operator*() { return value_; }
    const T& operator*() const { return value_; }

private:
    T value_;
    ErrorCode error_ = ErrorCode::OK;
};

// void 特化
template<>
class Result<void> {
public:
    static Result success() {
        Result r;
        r.error_ = ErrorCode::OK;
        return r;
    }
    
    static Result failure(ErrorCode error) {
        Result r;
        r.error_ = error;
        return r;
    }
    
    bool ok() const { return error_ == ErrorCode::OK; }
    ErrorCode error() const { return error_; }

private:
    ErrorCode error_ = ErrorCode::OK;
};

//==============================================================================
// 文件类型枚举
//==============================================================================

enum class FileType : uint8_t {
    FREE = 0,           // 空闲 inode
    REGULAR = 1,        // 普通文件
    DIRECTORY = 2,      // 目录
    SYMLINK = 3,        // 符号链接（预留）
};

//==============================================================================
// 1. Superblock 结构 - 文件系统元信息 (Block 0)
//==============================================================================

#pragma pack(push, 1)

struct Superblock {
    // ===== 魔数与版本 (8 bytes) =====
    uint32_t magic;                      // 魔数，用于验证文件系统格式
    uint32_t version;                    // 文件系统版本号
    
    // ===== 几何信息 (20 bytes) =====
    uint32_t block_size;                 // 块大小（字节）
    uint32_t total_blocks;               // 总块数
    uint32_t total_inodes;               // 总 inode 数
    uint32_t blocks_per_group;           // 每组块数（预留，用于扩展）
    uint32_t inodes_per_group;           // 每组 inode 数（预留）
    
    // ===== 区域起始位置 (20 bytes) =====
    BlockNo inode_bitmap_start;          // inode 位图起始块
    uint32_t inode_bitmap_blocks;        // inode 位图占用块数
    BlockNo block_bitmap_start;          // 块位图起始块
    uint32_t block_bitmap_blocks;        // 块位图占用块数
    BlockNo inode_table_start;           // inode 表起始块
    
    // ===== 使用统计 (16 bytes) =====
    uint32_t free_blocks;                // 空闲块数
    uint32_t free_inodes;                // 空闲 inode 数
    uint32_t used_blocks;                // 已用块数
    uint32_t used_inodes;                // 已用 inode 数
    
    // ===== 数据区信息 (8 bytes) =====
    BlockNo data_block_start;            // 数据块区域起始
    uint32_t data_block_count;           // 数据块数量
    
    // ===== 快照信息 (8 bytes) =====
    uint32_t snapshot_count;             // 当前快照数量
    BlockNo snapshot_list_block;         // 快照列表所在块（0 表示无）
    
    // ===== 时间戳 (24 bytes) =====
    int64_t create_time;                 // 创建时间 (Unix timestamp)
    int64_t mount_time;                  // 最后挂载时间
    int64_t write_time;                  // 最后写入时间
    
    // ===== 状态标志 (4 bytes) =====
    uint32_t state;                      // 文件系统状态
    
    // ===== 根目录 (4 bytes) =====
    InodeId root_inode;                  // 根目录 inode 编号
    
    // ===== 预留空间 =====
    uint8_t reserved[BLOCK_SIZE - 112];  // 填充至 1024 字节
    
    // ===== 方法 =====
    void init(uint32_t total_blks, uint32_t total_inds);
    bool validate() const;
};

#pragma pack(pop)

static_assert(sizeof(Superblock) == BLOCK_SIZE, "Superblock must be exactly BLOCK_SIZE");

//==============================================================================
// 2. Inode 结构 - 文件/目录元数据 (128 bytes)
//==============================================================================

#pragma pack(push, 1)

struct Inode {
    // ===== 类型与权限 (4 bytes) =====
    FileType type;                       // 文件类型 (1 byte)
    uint8_t permissions;                 // 权限位 (1 byte)
    uint16_t flags;                      // 标志位 (2 bytes)
    
    // ===== 大小与链接 (8 bytes) =====
    uint32_t size;                       // 文件大小（字节）(4 bytes)
    uint16_t link_count;                 // 硬链接计数 (2 bytes)
    uint16_t ref_count;                  // 引用计数（用于快照 COW）(2 bytes)
    
    // ===== 时间戳 (24 bytes) =====
    int64_t create_time;                 // 创建时间 (8 bytes)
    int64_t modify_time;                 // 内容修改时间 (8 bytes)
    int64_t access_time;                 // 最后访问时间 (8 bytes)
    
    // ===== 块指针 (56 bytes) =====
    BlockNo direct_blocks[NUM_DIRECT_BLOCKS];   // 12 个直接块指针 (48 bytes)
    BlockNo single_indirect;                     // 一级间接块 (4 bytes)
    BlockNo double_indirect;                     // 二级间接块 (4 bytes)
    
    // ===== 预留与校验 (36 bytes) =====
    uint32_t block_count;                // 实际占用的块数 (4 bytes)
    uint32_t checksum;                   // 校验和 (4 bytes)
    uint8_t reserved[28];                // 预留空间 (28 bytes)
    
    // 总计: 4 + 8 + 24 + 56 + 36 = 128 bytes
    
    // ===== 方法 =====
    void init(FileType t);
    void clear();
    bool isValid() const { return type != FileType::FREE; }
    bool isDirectory() const { return type == FileType::DIRECTORY; }
    bool isRegularFile() const { return type == FileType::REGULAR; }
    
    // 计算文件最大可支持的块数
    static constexpr uint32_t maxBlocks() {
        return NUM_DIRECT_BLOCKS +                              // 直接块
               PTRS_PER_BLOCK +                                 // 一级间接
               PTRS_PER_BLOCK * PTRS_PER_BLOCK;                // 二级间接
    }
    
    // 计算文件最大大小
    static constexpr uint64_t maxFileSize() {
        return static_cast<uint64_t>(maxBlocks()) * BLOCK_SIZE;
    }
};

#pragma pack(pop)

constexpr uint32_t INODES_PER_BLOCK = BLOCK_SIZE / sizeof(Inode);
static_assert(sizeof(Inode) == 128, "Inode must be 128 bytes");
static_assert(INODES_PER_BLOCK == 8, "Should fit 8 inodes per block");

//==============================================================================
// 3. 目录项结构 - 目录内容 (64 bytes)
//==============================================================================

#pragma pack(push, 1)

struct DirEntry {
    InodeId inode;                       // 指向的 inode 编号 (4 bytes)
    uint8_t name_len;                    // 文件名长度 (1 byte)
    FileType file_type;                  // 文件类型 (1 byte)
    uint16_t rec_len;                    // 记录长度 (2 bytes)
    char name[MAX_FILENAME_LEN];         // 文件名 (56 bytes)
    
    // 总计: 4 + 1 + 1 + 2 + 56 = 64 bytes
    
    // ===== 方法 =====
    void init(InodeId ino, const std::string& n, FileType t);
    void clear();
    bool isValid() const { return inode != INVALID_INODE; }
    std::string getName() const { return std::string(name, name_len); }
};

#pragma pack(pop)

constexpr uint32_t DIRENTRIES_PER_BLOCK = BLOCK_SIZE / sizeof(DirEntry);
static_assert(sizeof(DirEntry) == 64, "DirEntry must be 64 bytes");
static_assert(DIRENTRIES_PER_BLOCK == 16, "Should fit 16 dir entries per block");

//==============================================================================
// 4. 位图辅助结构
//==============================================================================

class Bitmap {
public:
    explicit Bitmap(uint8_t* data, uint32_t bits)
        : data_(data), total_bits_(bits) {}
    
    bool get(uint32_t index) const {
        if (index >= total_bits_) return false;
        return (data_[index / 8] >> (index % 8)) & 1;
    }
    
    void set(uint32_t index) {
        if (index >= total_bits_) return;
        data_[index / 8] |= (1 << (index % 8));
    }
    
    void clear(uint32_t index) {
        if (index >= total_bits_) return;
        data_[index / 8] &= ~(1 << (index % 8));
    }
    
    int32_t findFirstFree() const {
        for (uint32_t i = 0; i < total_bits_; ++i) {
            if (!get(i)) return static_cast<int32_t>(i);
        }
        return -1;
    }
    
    uint32_t countUsed() const {
        uint32_t count = 0;
        for (uint32_t i = 0; i < total_bits_; ++i) {
            if (get(i)) ++count;
        }
        return count;
    }
    
    uint32_t countFree() const {
        return total_bits_ - countUsed();
    }
    
private:
    uint8_t* data_;
    uint32_t total_bits_;
};

//==============================================================================
// 5. 快照元数据结构
//==============================================================================

constexpr uint32_t MAX_SNAPSHOT_NAME_LEN = 32;
constexpr uint32_t MAX_SNAPSHOTS = 16;

#pragma pack(push, 1)

struct SnapshotMeta {
    char name[MAX_SNAPSHOT_NAME_LEN];    // 快照名称 (32 bytes)
    int64_t create_time;                 // 创建时间 (8 bytes)
    InodeId root_inode;                  // 快照时的根目录 inode (4 bytes)
    uint32_t block_count;                // 快照占用的块数 (4 bytes)
    uint32_t flags;                      // 标志位 (4 bytes)
    uint8_t reserved[12];                // 预留 (12 bytes)
    
    // 总计: 32 + 8 + 4 + 4 + 4 + 12 = 64 bytes
    
    bool isValid() const { return flags & 0x0001; }
    std::string getName() const { 
        return std::string(name, strnlen(name, MAX_SNAPSHOT_NAME_LEN)); 
    }
};

static_assert(sizeof(SnapshotMeta) == 64, "SnapshotMeta must be 64 bytes");

struct SnapshotListBlock {
    uint32_t count;                      // 有效快照数量 (4 bytes)
    uint32_t reserved;                   // 预留 (4 bytes)
    SnapshotMeta snapshots[MAX_SNAPSHOTS]; // 快照数组 (64 * 16 = 1024 bytes)
    
    // 注意：这会超过 BLOCK_SIZE，需要调整
};

#pragma pack(pop)

// 重新计算：1024 - 8 = 1016 bytes 可用于快照
// 每个快照 64 bytes，最多 15 个快照
constexpr uint32_t ACTUAL_MAX_SNAPSHOTS = (BLOCK_SIZE - 8) / sizeof(SnapshotMeta);

//==============================================================================
// 6. 间接块结构
//==============================================================================

#pragma pack(push, 1)

struct IndirectBlock {
    BlockNo pointers[PTRS_PER_BLOCK];
    
    void init() {
        for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
            pointers[i] = INVALID_BLOCK;
        }
    }
};

#pragma pack(pop)

static_assert(sizeof(IndirectBlock) == BLOCK_SIZE, "IndirectBlock must be BLOCK_SIZE");

//==============================================================================
// 7. 文件统计信息（对外接口用）
//==============================================================================

struct FileStat {
    InodeId inode;
    FileType type;
    uint32_t size;
    uint16_t link_count;
    int64_t create_time;
    int64_t modify_time;
    int64_t access_time;
    uint32_t blocks;
};

//==============================================================================
// 8. 缓存统计信息
//==============================================================================

struct CacheStats {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint32_t capacity;
    uint32_t current_size;
    double hit_rate;
};

//==============================================================================
// 方法实现（inline）
//==============================================================================

inline void Superblock::init(uint32_t total_blks, uint32_t total_inds) {
    std::memset(this, 0, sizeof(Superblock));
    
    magic = MAGIC_NUMBER;
    version = FS_VERSION;
    block_size = BLOCK_SIZE;
    total_blocks = total_blks;
    total_inodes = total_inds;
    
    inode_bitmap_start = 1;
    inode_bitmap_blocks = (total_inodes + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    
    block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks;
    block_bitmap_blocks = (total_blocks + BLOCK_SIZE * 8 - 1) / (BLOCK_SIZE * 8);
    
    inode_table_start = block_bitmap_start + block_bitmap_blocks;
    uint32_t inode_table_blocks = (total_inodes + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK;
    
    data_block_start = inode_table_start + inode_table_blocks;
    data_block_count = total_blocks - data_block_start;
    
    free_blocks = data_block_count;
    free_inodes = total_inodes;
    used_blocks = 0;
    used_inodes = 0;
    
    snapshot_count = 0;
    snapshot_list_block = 0;
    
    create_time = 0;
    mount_time = 0;
    write_time = 0;
    
    state = 0x0001;
    root_inode = ROOT_INODE;
}

inline bool Superblock::validate() const {
    if (magic != MAGIC_NUMBER) return false;
    if (version > FS_VERSION) return false;
    if (block_size != BLOCK_SIZE) return false;
    if (total_blocks == 0 || total_inodes == 0) return false;
    return true;
}

inline void Inode::init(FileType t) {
    std::memset(this, 0, sizeof(Inode));
    type = t;
    permissions = 0x07;
    link_count = 1;
    ref_count = 1;
    block_count = 0;
    
    for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
        direct_blocks[i] = INVALID_BLOCK;
    }
    single_indirect = INVALID_BLOCK;
    double_indirect = INVALID_BLOCK;
}

inline void Inode::clear() {
    std::memset(this, 0, sizeof(Inode));
    type = FileType::FREE;
    for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
        direct_blocks[i] = INVALID_BLOCK;
    }
    single_indirect = INVALID_BLOCK;
    double_indirect = INVALID_BLOCK;
}

inline void DirEntry::init(InodeId ino, const std::string& n, FileType t) {
    std::memset(this, 0, sizeof(DirEntry));
    inode = ino;
    file_type = t;
    name_len = static_cast<uint8_t>(std::min(n.size(), static_cast<size_t>(MAX_FILENAME_LEN)));
    rec_len = sizeof(DirEntry);
    std::memcpy(name, n.data(), name_len);
}

inline void DirEntry::clear() {
    std::memset(this, 0, sizeof(DirEntry));
    inode = INVALID_INODE;
}

} // namespace fs

#endif // FSTYPES_H