// filesystem/src/test_cache.cpp
#include "Cache.h"
#include "DiskImage.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>
#include <random>

using namespace fs;

const char* TEST_DISK = "test_cache.img";

//==============================================================================
// LRU 缓存单元测试
//==============================================================================

void testLRUBasic() {
    std::cout << "=== Test: LRU Basic Operations ===" << std::endl;
    
    LRUCache cache(4);  // 容量 4 块
    
    // 初始状态
    assert(cache.getCapacity() == 4);
    assert(cache.getCurrentSize() == 0);
    
    // 放入块
    uint8_t data1[BLOCK_SIZE] = {1, 2, 3, 4};
    uint8_t data2[BLOCK_SIZE] = {5, 6, 7, 8};
    
    cache.put(100, data1);
    cache.put(200, data2);
    
    assert(cache.getCurrentSize() == 2);
    assert(cache.contains(100));
    assert(cache.contains(200));
    assert(!cache.contains(300));
    
    std::cout << "  Basic put/contains: OK" << std::endl;
    
    // 获取块
    uint8_t buffer[BLOCK_SIZE];
    bool hit = cache.get(100, buffer);
    assert(hit);
    assert(buffer[0] == 1 && buffer[1] == 2);
    
    hit = cache.get(300, buffer);
    assert(!hit);
    
    std::cout << "  Basic get: OK" << std::endl;
    
    // 统计
    auto stats = cache.getStats();
    assert(stats.hits == 1);
    assert(stats.misses == 1);
    
    std::cout << "  Stats: hits=" << stats.hits << ", misses=" << stats.misses << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testLRUEviction() {
    std::cout << "=== Test: LRU Eviction ===" << std::endl;
    
    LRUCache cache(3);  // 容量 3 块
    
    uint8_t data[BLOCK_SIZE];
    
    // 放入 3 块（达到容量）
    for (int i = 0; i < 3; ++i) {
        std::memset(data, i + 1, BLOCK_SIZE);
        cache.put(i, data);
    }
    
    assert(cache.getCurrentSize() == 3);
    std::cout << "  Filled cache to capacity" << std::endl;
    
    // LRU 顺序应该是: 2, 1, 0 (最近到最久)
    auto order = cache.getLRUOrder();
    assert(order[0] == 2);  // 最近使用
    assert(order[2] == 0);  // 最久未使用
    
    // 访问块 0，使其变为最近使用
    uint8_t buffer[BLOCK_SIZE];
    cache.get(0, buffer);
    
    order = cache.getLRUOrder();
    assert(order[0] == 0);  // 现在块 0 最近使用
    assert(order[2] == 1);  // 块 1 最久未使用
    
    std::cout << "  LRU order after access: ";
    for (auto b : order) std::cout << b << " ";
    std::cout << std::endl;
    
    // 放入新块，应该淘汰块 1
    std::memset(data, 99, BLOCK_SIZE);
    cache.put(99, data);
    
    assert(cache.getCurrentSize() == 3);
    assert(cache.contains(99));
    assert(!cache.contains(1));  // 块 1 被淘汰
    assert(cache.contains(0));
    assert(cache.contains(2));
    
    std::cout << "  Block 1 evicted correctly" << std::endl;
    
    auto stats = cache.getStats();
    assert(stats.evictions == 1);
    std::cout << "  Evictions: " << stats.evictions << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testLRUDirty() {
    std::cout << "=== Test: LRU Dirty Blocks ===" << std::endl;
    
    LRUCache cache(4);
    
    uint8_t data[BLOCK_SIZE];
    
    // 放入干净块
    std::memset(data, 1, BLOCK_SIZE);
    cache.put(100, data, false);
    assert(!cache.isDirty(100));
    
    // 放入脏块
    std::memset(data, 2, BLOCK_SIZE);
    cache.put(200, data, true);
    assert(cache.isDirty(200));
    
    // 标记为脏
    cache.markDirty(100);
    assert(cache.isDirty(100));
    
    std::cout << "  Dirty marking: OK" << std::endl;
    
    // 获取脏块列表
    auto dirty = cache.getDirtyBlocks();
    assert(dirty.size() == 2);
    std::cout << "  Dirty blocks count: " << dirty.size() << std::endl;
    
    // 清除脏标志
    cache.clearDirty(100);
    assert(!cache.isDirty(100));
    
    cache.clearAllDirty();
    dirty = cache.getDirtyBlocks();
    assert(dirty.empty());
    
    std::cout << "  Clear dirty: OK" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testLRUCapacityChange() {
    std::cout << "=== Test: LRU Capacity Change ===" << std::endl;
    
    LRUCache cache(8);
    
    uint8_t data[BLOCK_SIZE];
    
    // 放入 8 块
    for (int i = 0; i < 8; ++i) {
        std::memset(data, i, BLOCK_SIZE);
        cache.put(i, data);
    }
    
    assert(cache.getCurrentSize() == 8);
    std::cout << "  Initial size: " << cache.getCurrentSize() << std::endl;
    
    // 减小容量
    cache.setCapacity(4);
    assert(cache.getCapacity() == 4);
    assert(cache.getCurrentSize() == 4);
    std::cout << "  After resize to 4: " << cache.getCurrentSize() << std::endl;
    
    // 最近的 4 块应该保留
    assert(cache.contains(7));
    assert(cache.contains(6));
    assert(cache.contains(5));
    assert(cache.contains(4));
    assert(!cache.contains(0));
    
    std::cout << "PASSED" << std::endl << std::endl;
}

//==============================================================================
// CachedDisk 集成测试
//==============================================================================

void testCachedDiskBasic() {
    std::cout << "=== Test: CachedDisk Basic ===" << std::endl;
    
    // 创建测试磁盘
    MkfsOptions opts;
    opts.total_blocks = 512;
    opts.total_inodes = 64;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    DiskImage disk;
    assert(disk.open(TEST_DISK) == ErrorCode::OK);
    
    CachedDisk cached(&disk, 16);  // 16 块缓存
    
    // 读取 superblock（块 0）
    Superblock sb;
    ErrorCode err = cached.readBlock(0, &sb);
    assert(err == ErrorCode::OK);
    assert(sb.validate());
    std::cout << "  Read superblock via cache" << std::endl;
    
    // 再次读取（应该命中缓存）
    err = cached.readBlock(0, &sb);
    assert(err == ErrorCode::OK);
    
    auto stats = cached.getCacheStats();
    assert(stats.hits >= 1);
    std::cout << "  Cache stats: hits=" << stats.hits 
              << ", misses=" << stats.misses << std::endl;
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCachedDiskWriteBack() {
    std::cout << "=== Test: CachedDisk Write-Back ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 512;
    opts.total_inodes = 64;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    DiskImage disk;
    assert(disk.open(TEST_DISK) == ErrorCode::OK);
    
    Superblock sb;
    disk.loadSuperblock(sb);
    BlockNo test_block = sb.data_block_start + 10;
    
    CachedDisk cached(&disk, 16);
    cached.setWriteThrough(false);  // 写回模式
    
    // 写入数据（只到缓存）
    uint8_t write_data[BLOCK_SIZE];
    std::memset(write_data, 0xAB, BLOCK_SIZE);
    
    ErrorCode err = cached.writeBlock(test_block, write_data);
    assert(err == ErrorCode::OK);
    std::cout << "  Wrote block " << test_block << " (cached)" << std::endl;
    
    // 验证缓存中有数据
    uint8_t read_data[BLOCK_SIZE];
    err = cached.readBlock(test_block, read_data);
    assert(err == ErrorCode::OK);
    assert(read_data[0] == 0xAB);
    
    // 直接从磁盘读取（应该还是旧数据或零）
    uint8_t disk_data[BLOCK_SIZE];
    disk.readBlock(test_block, disk_data);
    // 注意：此时磁盘上可能还是旧数据
    
    // Flush 到磁盘
    err = cached.flush();
    assert(err == ErrorCode::OK);
    std::cout << "  Flushed cache to disk" << std::endl;
    
    // 现在磁盘上应该有数据了
    disk.readBlock(test_block, disk_data);
    assert(disk_data[0] == 0xAB);
    std::cout << "  Verified data on disk after flush" << std::endl;
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCachedDiskWriteThrough() {
    std::cout << "=== Test: CachedDisk Write-Through ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 512;
    opts.total_inodes = 64;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    DiskImage disk;
    assert(disk.open(TEST_DISK) == ErrorCode::OK);
    
    Superblock sb;
    disk.loadSuperblock(sb);
    BlockNo test_block = sb.data_block_start + 20;
    
    CachedDisk cached(&disk, 16);
    cached.setWriteThrough(true);  // 写穿透模式
    
    // 写入数据（立即写到磁盘）
    uint8_t write_data[BLOCK_SIZE];
    std::memset(write_data, 0xCD, BLOCK_SIZE);
    
    ErrorCode err = cached.writeBlock(test_block, write_data);
    assert(err == ErrorCode::OK);
    
    // 直接从磁盘验证（不经过缓存）
    uint8_t disk_data[BLOCK_SIZE];
    disk.readBlock(test_block, disk_data);
    assert(disk_data[0] == 0xCD);
    std::cout << "  Write-through verified" << std::endl;
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCachePerformance() {
    std::cout << "=== Test: Cache Performance ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 1024;
    opts.total_inodes = 64;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    DiskImage disk;
    assert(disk.open(TEST_DISK) == ErrorCode::OK);
    
    Superblock sb;
    disk.loadSuperblock(sb);
    
    const int NUM_OPERATIONS = 1000;
    const int NUM_BLOCKS = 50;  // 访问的块范围
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dist(0, NUM_BLOCKS - 1);
    
    uint8_t buffer[BLOCK_SIZE];
    
    // 测试无缓存性能
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        BlockNo block = sb.data_block_start + dist(gen);
        disk.readBlock(block, buffer);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto no_cache_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "  Without cache: " << no_cache_time.count() << " us" << std::endl;
    
    // 测试有缓存性能
    CachedDisk cached(&disk, 32);
    
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < NUM_OPERATIONS; ++i) {
        BlockNo block = sb.data_block_start + dist(gen);
        cached.readBlock(block, buffer);
    }
    end = std::chrono::high_resolution_clock::now();
    auto cache_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    auto stats = cached.getCacheStats();
    std::cout << "  With cache: " << cache_time.count() << " us" << std::endl;
    std::cout << "  Cache stats: hits=" << stats.hits 
              << ", misses=" << stats.misses 
              << ", hit_rate=" << (stats.hit_rate * 100) << "%" << std::endl;
    
    double speedup = static_cast<double>(no_cache_time.count()) / cache_time.count();
    std::cout << "  Speedup: " << speedup << "x" << std::endl;
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCacheWithSequentialAccess() {
    std::cout << "=== Test: Sequential Access Pattern ===" << std::endl;
    
    LRUCache cache(8);
    uint8_t data[BLOCK_SIZE];
    uint8_t buffer[BLOCK_SIZE];
    
    // 顺序写入 20 块
    for (int i = 0; i < 20; ++i) {
        std::memset(data, i, BLOCK_SIZE);
        cache.put(i, data);
    }
    
    // 由于容量只有 8，只有最后 8 块在缓存中
    for (int i = 0; i < 12; ++i) {
        assert(!cache.contains(i));
    }
    for (int i = 12; i < 20; ++i) {
        assert(cache.contains(i));
    }
    
    std::cout << "  Sequential eviction: OK" << std::endl;
    
    auto stats = cache.getStats();
    std::cout << "  Evictions: " << stats.evictions << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCacheWithLocalityAccess() {
    std::cout << "=== Test: Locality Access Pattern ===" << std::endl;
    
    LRUCache cache(8);
    cache.resetStats();
    
    uint8_t data[BLOCK_SIZE];
    uint8_t buffer[BLOCK_SIZE];
    
    // 先填充缓存
    for (int i = 0; i < 8; ++i) {
        std::memset(data, i, BLOCK_SIZE);
        cache.put(i, data);
    }
    
    // 模拟具有局部性的访问模式（反复访问前 4 块）
    for (int round = 0; round < 100; ++round) {
        for (int i = 0; i < 4; ++i) {
            cache.get(i, buffer);
        }
    }
    
    auto stats = cache.getStats();
    std::cout << "  Locality access stats:" << std::endl;
    std::cout << "    Hits: " << stats.hits << std::endl;
    std::cout << "    Misses: " << stats.misses << std::endl;
    std::cout << "    Hit rate: " << (stats.hit_rate * 100) << "%" << std::endl;
    
    assert(stats.hit_rate > 0.99);  // 应该接近 100% 命中
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup complete ===" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "       Cache Module Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    try {
        // LRU 缓存单元测试
        testLRUBasic();
        testLRUEviction();
        testLRUDirty();
        testLRUCapacityChange();
        
        // CachedDisk 集成测试
        testCachedDiskBasic();
        testCachedDiskWriteBack();
        testCachedDiskWriteThrough();
        testCachePerformance();
        
        // 访问模式测试
        testCacheWithSequentialAccess();
        testCacheWithLocalityAccess();
        
        cleanup();
        
        std::cout << std::endl;
        std::cout << "==========================================" << std::endl;
        std::cout << "       All tests passed!" << std::endl;
        std::cout << "==========================================" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}