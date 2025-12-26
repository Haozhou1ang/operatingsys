// filesystem/src/Cache.cpp
#include "Cache.h"
#include "DiskImage.h"
#include <iostream>
#include <algorithm>

namespace fs {

//==============================================================================
// LRUCache 实现
//==============================================================================

LRUCache::LRUCache(uint32_t capacity)
    : capacity_(capacity > 0 ? capacity : 1)
    , hits_(0)
    , misses_(0)
    , evictions_(0)
{
}

LRUCache::~LRUCache() {
    clear();
}

//==============================================================================
// 缓存操作接口
//==============================================================================

bool LRUCache::get(BlockNo block_no, void* data) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    if (it == cache_map_.end()) {
        recordMiss();
        return false;
    }
    
    // 命中：移到 LRU 列表头部
    moveToFront(it->second);
    
    // 复制数据
    std::memcpy(data, (*it->second)->data, BLOCK_SIZE);
    
    recordHit();
    return true;
}

void LRUCache::put(BlockNo block_no, const void* data, bool dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    
    if (it != cache_map_.end()) {
        // 已存在：更新数据并移到头部
        std::memcpy((*it->second)->data, data, BLOCK_SIZE);
        (*it->second)->dirty = (*it->second)->dirty || dirty;
        moveToFront(it->second);
        return;
    }
    
    // 不存在：需要插入新块
    
    // 检查容量，必要时淘汰
    while (lru_list_.size() >= capacity_) {
        evictLRU();
    }
    
    // 创建新缓存块
    auto block = std::make_shared<CacheBlock>(block_no);
    std::memcpy(block->data, data, BLOCK_SIZE);
    block->dirty = dirty;
    
    // 插入到 LRU 列表头部
    lru_list_.push_front(block);
    cache_map_[block_no] = lru_list_.begin();
}

bool LRUCache::contains(BlockNo block_no) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_map_.find(block_no) != cache_map_.end();
}

bool LRUCache::markDirty(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    if (it == cache_map_.end()) {
        return false;
    }
    
    (*it->second)->dirty = true;
    return true;
}

bool LRUCache::isDirty(BlockNo block_no) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    if (it == cache_map_.end()) {
        return false;
    }
    
    return (*it->second)->dirty;
}

void LRUCache::invalidate(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    if (it != cache_map_.end()) {
        lru_list_.erase(it->second);
        cache_map_.erase(it);
    }
}

void LRUCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    lru_list_.clear();
    cache_map_.clear();
}

std::vector<std::pair<BlockNo, std::vector<uint8_t>>> LRUCache::getDirtyBlocks() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::pair<BlockNo, std::vector<uint8_t>>> dirty_blocks;
    
    for (const auto& block : lru_list_) {
        if (block->dirty) {
            std::vector<uint8_t> data(block->data, block->data + BLOCK_SIZE);
            dirty_blocks.emplace_back(block->block_no, std::move(data));
        }
    }
    
    return dirty_blocks;
}

void LRUCache::clearDirty(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = cache_map_.find(block_no);
    if (it != cache_map_.end()) {
        (*it->second)->dirty = false;
    }
}

void LRUCache::clearAllDirty() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& block : lru_list_) {
        block->dirty = false;
    }
}

//==============================================================================
// 配置接口
//==============================================================================

void LRUCache::setCapacity(uint32_t new_capacity) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    capacity_ = (new_capacity > 0) ? new_capacity : 1;
    
    // 淘汰超出容量的块
    while (lru_list_.size() > capacity_) {
        evictLRU();
    }
}

uint32_t LRUCache::getCurrentSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(lru_list_.size());
}

//==============================================================================
// 统计接口
//==============================================================================

CacheStats LRUCache::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> slock(stats_mutex_);
    
    CacheStats stats;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.evictions = evictions_;
    stats.capacity = capacity_;
    stats.current_size = static_cast<uint32_t>(lru_list_.size());
    
    uint64_t total = hits_ + misses_;
    stats.hit_rate = (total > 0) ? static_cast<double>(hits_) / total : 0.0;
    
    return stats;
}

void LRUCache::resetStats() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    hits_ = 0;
    misses_ = 0;
    evictions_ = 0;
}

double LRUCache::getHitRate() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    uint64_t total = hits_ + misses_;
    return (total > 0) ? static_cast<double>(hits_) / total : 0.0;
}

//==============================================================================
// 调试接口
//==============================================================================

void LRUCache::dump() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "=== LRU Cache Dump ===" << std::endl;
    std::cout << "Capacity: " << capacity_ << std::endl;
    std::cout << "Current size: " << lru_list_.size() << std::endl;
    std::cout << "Blocks (MRU -> LRU):" << std::endl;
    
    int index = 0;
    for (const auto& block : lru_list_) {
        std::cout << "  [" << index++ << "] Block " << block->block_no
                  << (block->dirty ? " (dirty)" : "") << std::endl;
    }
    
    auto stats = getStats();
    std::cout << "Stats: hits=" << stats.hits 
              << ", misses=" << stats.misses
              << ", evictions=" << stats.evictions
              << ", hit_rate=" << (stats.hit_rate * 100) << "%" << std::endl;
}

std::vector<BlockNo> LRUCache::getLRUOrder() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<BlockNo> order;
    order.reserve(lru_list_.size());
    
    for (const auto& block : lru_list_) {
        order.push_back(block->block_no);
    }
    
    return order;
}

//==============================================================================
// 内部方法
//==============================================================================

void LRUCache::moveToFront(LRUIterator it) {
    // 将节点移到列表头部
    if (it != lru_list_.begin()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it);
    }
}

std::shared_ptr<CacheBlock> LRUCache::evictLRU() {
    if (lru_list_.empty()) {
        return nullptr;
    }
    
    // 获取最久未使用的块（列表尾部）
    auto block = lru_list_.back();
    
    // 从映射中移除
    cache_map_.erase(block->block_no);
    
    // 从列表中移除
    lru_list_.pop_back();
    
    recordEviction();
    
    return block;
}

void LRUCache::recordHit() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    hits_++;
}

void LRUCache::recordMiss() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    misses_++;
}

void LRUCache::recordEviction() {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    evictions_++;
}

//==============================================================================
// CachedDisk 实现
//==============================================================================

CachedDisk::CachedDisk(DiskImage* disk, uint32_t cache_capacity)
    : disk_(disk)
    , cache_(cache_capacity)
    , cache_enabled_(true)
    , write_through_(false)
{
}

CachedDisk::~CachedDisk() {
    // 确保脏数据写回
    flush();
}

//==============================================================================
// 块级读写接口
//==============================================================================

ErrorCode CachedDisk::readBlock(BlockNo block_no, void* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!disk_ || !buffer) {
        return ErrorCode::E_INVALID_PARAM;
    }
    
    // 尝试从缓存读取
    if (cache_enabled_ && cache_.get(block_no, buffer)) {
        return ErrorCode::OK;
    }
    
    // 缓存未命中，从磁盘读取
    ErrorCode err = disk_->readBlock(block_no, buffer);
    if (err != ErrorCode::OK) {
        return err;
    }
    
    // 放入缓存
    if (cache_enabled_) {
        cache_.put(block_no, buffer, false);
    }
    
    return ErrorCode::OK;
}

ErrorCode CachedDisk::writeBlock(BlockNo block_no, const void* buffer, 
                                  bool write_through) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!disk_ || !buffer) {
        return ErrorCode::E_INVALID_PARAM;
    }
    
    bool do_write_through = write_through || write_through_;
    
    if (cache_enabled_) {
        // 更新缓存
        cache_.put(block_no, buffer, !do_write_through);
    }
    
    // 写穿透模式或强制写回
    if (do_write_through || !cache_enabled_) {
        ErrorCode err = disk_->writeBlock(block_no, buffer);
        if (err != ErrorCode::OK) {
            return err;
        }
        
        if (cache_enabled_) {
            cache_.clearDirty(block_no);
        }
    }
    
    return ErrorCode::OK;
}

ErrorCode CachedDisk::readBlocks(BlockNo start_block, uint32_t count, void* buffer) {
    uint8_t* ptr = static_cast<uint8_t*>(buffer);
    
    for (uint32_t i = 0; i < count; ++i) {
        ErrorCode err = readBlock(start_block + i, ptr + i * BLOCK_SIZE);
        if (err != ErrorCode::OK) {
            return err;
        }
    }
    
    return ErrorCode::OK;
}

ErrorCode CachedDisk::writeBlocks(BlockNo start_block, uint32_t count,
                                   const void* buffer, bool write_through) {
    const uint8_t* ptr = static_cast<const uint8_t*>(buffer);
    
    for (uint32_t i = 0; i < count; ++i) {
        ErrorCode err = writeBlock(start_block + i, ptr + i * BLOCK_SIZE, write_through);
        if (err != ErrorCode::OK) {
            return err;
        }
    }
    
    return ErrorCode::OK;
}

//==============================================================================
// 缓存控制接口
//==============================================================================

ErrorCode CachedDisk::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!disk_) {
        return ErrorCode::E_INVALID_PARAM;
    }
    
    // 获取所有脏块
    auto dirty_blocks = cache_.getDirtyBlocks();
    
    // 写回磁盘
    for (const auto& [block_no, data] : dirty_blocks) {
        ErrorCode err = disk_->writeBlock(block_no, data.data());
        if (err != ErrorCode::OK) {
            return err;
        }
        cache_.clearDirty(block_no);
    }
    
    return disk_->sync();
}

void CachedDisk::invalidate(BlockNo block_no) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.invalidate(block_no);
}

ErrorCode CachedDisk::clearCache() {
    // 先 flush
    ErrorCode err = flush();
    if (err != ErrorCode::OK) {
        return err;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    
    return ErrorCode::OK;
}

void CachedDisk::setCacheCapacity(uint32_t capacity) {
    // 先 flush 再调整容量
    flush();
    
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.setCapacity(capacity);
}

//==============================================================================
// 统计接口
//==============================================================================

CacheStats CachedDisk::getCacheStats() const {
    return cache_.getStats();
}

void CachedDisk::resetCacheStats() {
    cache_.resetStats();
}

} // namespace fs