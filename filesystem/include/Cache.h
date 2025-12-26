// filesystem/include/Cache.h
#ifndef CACHE_H
#define CACHE_H

#include "FSTypes.h"
#include <unordered_map>
#include <list>
#include <mutex>
#include <memory>
#include <vector>
#include <cstring>

namespace fs {

/**
 * CacheBlock - 缓存块数据
 */
struct CacheBlock {
    BlockNo block_no;                    // 块编号
    uint8_t data[BLOCK_SIZE];            // 块数据
    bool dirty;                          // 是否被修改
    
    CacheBlock() : block_no(INVALID_BLOCK), dirty(false) {
        std::memset(data, 0, BLOCK_SIZE);
    }
    
    explicit CacheBlock(BlockNo no) : block_no(no), dirty(false) {
        std::memset(data, 0, BLOCK_SIZE);
    }
};

/**
 * LRUCache - LRU 块缓存
 * 
 * 功能：
 * 1. 缓存磁盘块，减少 I/O 操作
 * 2. LRU (Least Recently Used) 淘汰策略
 * 3. 支持脏块写回
 * 4. 缓存统计信息
 * 5. 线程安全
 */
class LRUCache {
public:
    /**
     * 构造函数
     * @param capacity 缓存容量（块数）
     */
    explicit LRUCache(uint32_t capacity = 64);
    ~LRUCache();

    // 禁止拷贝
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    //==========================================================================
    // 缓存操作接口
    //==========================================================================

    /**
     * 获取块（如果在缓存中）
     * @param block_no 块编号
     * @param data 输出缓冲区
     * @return 命中返回 true
     */
    bool get(BlockNo block_no, void* data);

    /**
     * 将块放入缓存
     * @param block_no 块编号
     * @param data 块数据
     * @param dirty 是否标记为脏
     */
    void put(BlockNo block_no, const void* data, bool dirty = false);

    /**
     * 检查块是否在缓存中
     * @param block_no 块编号
     * @return 在缓存中返回 true
     */
    bool contains(BlockNo block_no) const;

    /**
     * 标记块为脏
     * @param block_no 块编号
     * @return 块在缓存中返回 true
     */
    bool markDirty(BlockNo block_no);

    /**
     * 获取缓存块的脏标志
     * @param block_no 块编号
     * @return 脏返回 true，不在缓存中也返回 false
     */
    bool isDirty(BlockNo block_no) const;

    /**
     * 使单个块失效（不写回）
     * @param block_no 块编号
     */
    void invalidate(BlockNo block_no);

    /**
     * 清空所有缓存（不写回）
     */
    void clear();

    /**
     * 获取所有脏块列表
     * @return 脏块信息列表（块号和数据）
     */
    std::vector<std::pair<BlockNo, std::vector<uint8_t>>> getDirtyBlocks();

    /**
     * 清除指定块的脏标志
     * @param block_no 块编号
     */
    void clearDirty(BlockNo block_no);

    /**
     * 清除所有脏标志
     */
    void clearAllDirty();

    //==========================================================================
    // 配置接口
    //==========================================================================

    /**
     * 获取缓存容量
     */
    uint32_t getCapacity() const { return capacity_; }

    /**
     * 设置缓存容量（会清空现有缓存）
     * @param new_capacity 新容量
     */
    void setCapacity(uint32_t new_capacity);

    /**
     * 获取当前缓存块数
     */
    uint32_t getCurrentSize() const;

    //==========================================================================
    // 统计接口
    //==========================================================================

    /**
     * 获取缓存统计信息
     */
    CacheStats getStats() const;

    /**
     * 重置统计信息
     */
    void resetStats();

    /**
     * 获取命中率
     */
    double getHitRate() const;

    //==========================================================================
    // 调试接口
    //==========================================================================

    /**
     * 打印缓存状态（调试用）
     */
    void dump() const;

    /**
     * 获取 LRU 顺序的块列表（最近使用的在前）
     */
    std::vector<BlockNo> getLRUOrder() const;

private:
    //==========================================================================
    // 内部类型定义
    //==========================================================================
    
    // LRU 列表：front = 最近使用，back = 最久未使用
    using LRUList = std::list<std::shared_ptr<CacheBlock>>;
    using LRUIterator = LRUList::iterator;
    using CacheMap = std::unordered_map<BlockNo, LRUIterator>;

    //==========================================================================
    // 内部方法
    //==========================================================================

    // 移动块到 LRU 列表头部（最近使用）
    void moveToFront(LRUIterator it);

    // 淘汰最久未使用的块
    std::shared_ptr<CacheBlock> evictLRU();

    // 更新统计
    void recordHit();
    void recordMiss();
    void recordEviction();

    //==========================================================================
    // 成员变量
    //==========================================================================

    uint32_t capacity_;                  // 缓存容量
    LRUList lru_list_;                   // LRU 列表
    CacheMap cache_map_;                 // 块号 -> 迭代器映射

    // 统计信息
    mutable std::mutex stats_mutex_;
    uint64_t hits_;
    uint64_t misses_;
    uint64_t evictions_;

    // 主互斥锁
    mutable std::mutex mutex_;
};

/**
 * CachedDisk - 带缓存的磁盘访问层
 * 
 * 封装 DiskImage，在其上添加缓存层
 */
class DiskImage;  // 前向声明

class CachedDisk {
public:
    /**
     * 构造函数
     * @param disk 磁盘镜像指针
     * @param cache_capacity 缓存容量（块数）
     */
    CachedDisk(DiskImage* disk, uint32_t cache_capacity = 64);
    ~CachedDisk();

    //==========================================================================
    // 块级读写接口（带缓存）
    //==========================================================================

    /**
     * 读取块（优先从缓存读取）
     * @param block_no 块编号
     * @param buffer 输出缓冲区
     * @return 成功返回 OK
     */
    ErrorCode readBlock(BlockNo block_no, void* buffer);

    /**
     * 写入块（写入缓存，延迟写回）
     * @param block_no 块编号
     * @param buffer 输入缓冲区
     * @param write_through 是否立即写回磁盘
     * @return 成功返回 OK
     */
    ErrorCode writeBlock(BlockNo block_no, const void* buffer, 
                         bool write_through = false);

    /**
     * 读取多个块
     */
    ErrorCode readBlocks(BlockNo start_block, uint32_t count, void* buffer);

    /**
     * 写入多个块
     */
    ErrorCode writeBlocks(BlockNo start_block, uint32_t count, 
                          const void* buffer, bool write_through = false);

    //==========================================================================
    // 缓存控制接口
    //==========================================================================

    /**
     * 同步所有脏块到磁盘
     * @return 成功返回 OK
     */
    ErrorCode flush();

    /**
     * 使指定块缓存失效
     */
    void invalidate(BlockNo block_no);

    /**
     * 清空缓存（先 flush）
     */
    ErrorCode clearCache();

    /**
     * 设置缓存容量
     */
    void setCacheCapacity(uint32_t capacity);

    /**
     * 启用/禁用缓存
     */
    void setCacheEnabled(bool enabled) { cache_enabled_ = enabled; }
    bool isCacheEnabled() const { return cache_enabled_; }

    /**
     * 设置写回策略
     * @param write_through true = 写穿透，false = 写回
     */
    void setWriteThrough(bool write_through) { write_through_ = write_through; }
    bool isWriteThrough() const { return write_through_; }

    //==========================================================================
    // 统计接口
    //==========================================================================

    /**
     * 获取缓存统计信息
     */
    CacheStats getCacheStats() const;

    /**
     * 重置缓存统计
     */
    void resetCacheStats();

    /**
     * 获取底层磁盘
     */
    DiskImage* getDisk() { return disk_; }

    /**
     * 获取缓存对象
     */
    LRUCache& getCache() { return cache_; }

private:
    DiskImage* disk_;                    // 底层磁盘
    LRUCache cache_;                     // LRU 缓存
    bool cache_enabled_;                 // 是否启用缓存
    bool write_through_;                 // 写穿透模式
    mutable std::mutex mutex_;           // 操作互斥锁
};

} // namespace fs

#endif // CACHE_H