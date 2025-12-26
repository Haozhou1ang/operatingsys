// filesystem/src/test_cache_integration.cpp
#include "DiskImage.h"
#include "Cache.h"
#include "Allocator.h"
#include "Directory.h"
#include <iostream>
#include <cassert>
#include <chrono>

using namespace fs;

const char* TEST_DISK = "test_cache_int.img";

void testFullStackWithCache() {
    std::cout << "=== Test: Full Stack with Cache ===" << std::endl;
    
    // 创建文件系统
    MkfsOptions opts;
    opts.total_blocks = 2048;
    opts.total_inodes = 128;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    // 打开磁盘
    DiskImage disk;
    assert(disk.open(TEST_DISK) == ErrorCode::OK);
    
    // 创建缓存层
    CachedDisk cached(&disk, 32);  // 32 块缓存
    
    // 创建分配器（使用缓存）
    Allocator alloc(&cached);
    assert(alloc.load() == ErrorCode::OK);
    assert(alloc.isCacheEnabled());
    std::cout << "  Allocator with cache: OK" << std::endl;
    
    // 创建目录管理器（使用缓存）
    Directory dir(&alloc, &cached);
    assert(dir.isCacheEnabled());
    std::cout << "  Directory with cache: OK" << std::endl;
    
    // 执行文件操作
    dir.mkdir("/test");
    dir.createFile("/test/file.txt");
    dir.writeFile("/test/file.txt", std::string("Hello, Cached World!"));
    
    auto read = dir.readFile("/test/file.txt");
    assert(read.ok());
    std::string content(read.value().begin(), read.value().end());
    assert(content == "Hello, Cached World!");
    std::cout << "  File operations: OK" << std::endl;
    
    // 查看缓存统计
    auto stats = cached.getCacheStats();
    std::cout << "  Cache stats:" << std::endl;
    std::cout << "    Hits: " << stats.hits << std::endl;
    std::cout << "    Misses: " << stats.misses << std::endl;
    std::cout << "    Hit rate: " << (stats.hit_rate * 100) << "%" << std::endl;
    
    // 同步并关闭
    dir.flushCache();
    alloc.sync();
    disk.close();
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCachePerformanceComparison() {
    std::cout << "=== Test: Cache Performance Comparison ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 2048;
    opts.total_inodes = 128;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    const int NUM_FILES = 20;
    const int NUM_READS = 5;
    
    // 测试无缓存
    int64_t no_cache_time;
    {
        DiskImage disk;
        disk.open(TEST_DISK);
        
        Allocator alloc(&disk);
        alloc.load();
        
        Directory dir(&alloc, &disk);
        
        // 创建文件
        dir.mkdir("/bench");
        for (int i = 0; i < NUM_FILES; ++i) {
            std::string path = "/bench/file" + std::to_string(i) + ".txt";
            dir.createFile(path);
            dir.writeFile(path, std::string(500, 'A' + (i % 26)));
        }
        
        // 计时读取
        auto start = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < NUM_READS; ++r) {
            for (int i = 0; i < NUM_FILES; ++i) {
                std::string path = "/bench/file" + std::to_string(i) + ".txt";
                dir.readFile(path);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        no_cache_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        alloc.sync();
        disk.close();
    }
    std::cout << "  Without cache: " << no_cache_time << " us" << std::endl;
    
    // 重新格式化
    mkfs(TEST_DISK, opts);
    
    // 测试有缓存
    int64_t cache_time;
    CacheStats final_stats;
    {
        DiskImage disk;
        disk.open(TEST_DISK);
        
        CachedDisk cached(&disk, 64);
        
        Allocator alloc(&cached);
        alloc.load();
        
        Directory dir(&alloc, &cached);
        
        // 创建文件
        dir.mkdir("/bench");
        for (int i = 0; i < NUM_FILES; ++i) {
            std::string path = "/bench/file" + std::to_string(i) + ".txt";
            dir.createFile(path);
            dir.writeFile(path, std::string(500, 'A' + (i % 26)));
        }
        
        // 重置统计
        cached.resetCacheStats();
        
        // 计时读取
        auto start = std::chrono::high_resolution_clock::now();
        for (int r = 0; r < NUM_READS; ++r) {
            for (int i = 0; i < NUM_FILES; ++i) {
                std::string path = "/bench/file" + std::to_string(i) + ".txt";
                dir.readFile(path);
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        cache_time = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        
        final_stats = cached.getCacheStats();
        
        cached.flush();
        alloc.sync();
        disk.close();
    }
    
    std::cout << "  With cache: " << cache_time << " us" << std::endl;
    std::cout << "  Cache stats:" << std::endl;
    std::cout << "    Hits: " << final_stats.hits << std::endl;
    std::cout << "    Misses: " << final_stats.misses << std::endl;
    std::cout << "    Hit rate: " << (final_stats.hit_rate * 100) << "%" << std::endl;
    
    double speedup = static_cast<double>(no_cache_time) / cache_time;
    std::cout << "  Speedup: " << speedup << "x" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testPersistenceWithCache() {
    std::cout << "=== Test: Persistence with Cache ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 1024;
    opts.total_inodes = 64;
    opts.force = true;
    mkfs(TEST_DISK, opts);
    
    std::string test_content = "This should persist through cache!";
    
    // Phase 1: 写入数据
    {
        DiskImage disk;
        disk.open(TEST_DISK);
        
        CachedDisk cached(&disk, 16);
        Allocator alloc(&cached);
        alloc.load();
        Directory dir(&alloc, &cached);
        
        dir.createFile("/persist.txt");
        dir.writeFile("/persist.txt", test_content);
        
        // 必须 flush 确保写入磁盘
        cached.flush();
        alloc.sync();
        
        std::cout << "  Phase 1: Written data" << std::endl;
    }
    
    // Phase 2: 重新打开并验证
    {
        DiskImage disk;
        disk.open(TEST_DISK);
        
        CachedDisk cached(&disk, 16);
        Allocator alloc(&cached);
        alloc.load();
        Directory dir(&alloc, &cached);
        
        assert(dir.exists("/persist.txt"));
        
        auto read = dir.readFile("/persist.txt");
        assert(read.ok());
        
        std::string content(read.value().begin(), read.value().end());
        assert(content == test_content);
        
        std::cout << "  Phase 2: Verified data" << std::endl;
    }
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup complete ===" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "    Cache Integration Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    try {
        testFullStackWithCache();
        testCachePerformanceComparison();
        testPersistenceWithCache();
        
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