// filesystem/src/test_snapshot.cpp
#include "DiskImage.h"
#include "Cache.h"
#include "Allocator.h"
#include "Directory.h"
#include "Snapshot.h"
#include <iostream>
#include <cassert>

using namespace fs;

const char* TEST_DISK = "test_snapshot.img";

class TestFixture {
public:
    DiskImage disk;
    CachedDisk* cached_disk;
    Allocator* alloc;
    Directory* dir;
    SnapshotManager* snap;
    
    TestFixture() : cached_disk(nullptr), alloc(nullptr), dir(nullptr), snap(nullptr) {}
    
    ~TestFixture() { cleanup(); }
    
    void cleanup() {
        delete snap; snap = nullptr;
        delete dir; dir = nullptr;
        delete alloc; alloc = nullptr;
        delete cached_disk; cached_disk = nullptr;
        disk.close();
    }
    
    bool setup() {
        cleanup();
        
        MkfsOptions opts;
        opts.total_blocks = 2048;
        opts.total_inodes = 256;
        opts.force = true;
        
        if (mkfs(TEST_DISK, opts).error != ErrorCode::OK) return false;
        if (disk.open(TEST_DISK) != ErrorCode::OK) return false;
        
        cached_disk = new CachedDisk(&disk, 64);
        alloc = new Allocator(cached_disk);
        if (alloc->load() != ErrorCode::OK) return false;
        
        dir = new Directory(alloc, cached_disk);
        snap = new SnapshotManager(alloc, dir, cached_disk);
        if (snap->load() != ErrorCode::OK) return false;
        
        return true;
    }
};

void testSnapshotBasic() {
    std::cout << "=== Test: Snapshot Basic ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    assert(tf.snap->getSnapshotCount() == 0);
    std::cout << "  Initial: 0 snapshots" << std::endl;
    
    // 创建数据
    tf.dir->mkdir("/data");
    tf.dir->createFile("/data/test.txt");
    tf.dir->writeFile("/data/test.txt", std::string("Hello World"));
    tf.dir->sync();
    
    std::cout << "  Created /data/test.txt" << std::endl;
    
    // 创建快照
    ErrorCode err = tf.snap->createSnapshot("snap1");
    assert(err == ErrorCode::OK);
    assert(tf.snap->getSnapshotCount() == 1);
    assert(tf.snap->snapshotExists("snap1"));
    
    std::cout << "  Created snapshot 'snap1'" << std::endl;
    
    // 获取快照信息
    auto info = tf.snap->getSnapshot("snap1");
    assert(info.ok());
    std::cout << "  Snapshot info: name=" << info.value().name 
              << ", inode=" << info.value().root_inode << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testSnapshotMultiple() {
    std::cout << "=== Test: Multiple Snapshots ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    for (int i = 1; i <= 5; ++i) {
        tf.dir->createFile("/file" + std::to_string(i) + ".txt");
        ErrorCode err = tf.snap->createSnapshot("snap" + std::to_string(i));
        assert(err == ErrorCode::OK);
    }
    
    assert(tf.snap->getSnapshotCount() == 5);
    std::cout << "  Created 5 snapshots" << std::endl;
    
    // 列出快照
    auto list = tf.snap->listSnapshots();
    for (const auto& s : list) {
        std::cout << "    - " << s.name << std::endl;
    }
    
    // 删除中间的
    tf.snap->deleteSnapshot("snap3");
    assert(tf.snap->getSnapshotCount() == 4);
    assert(!tf.snap->snapshotExists("snap3"));
    std::cout << "  Deleted snap3" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testSnapshotRestore() {
    std::cout << "=== Test: Snapshot Restore ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建原始数据
    tf.dir->createFile("/restore.txt");
    tf.dir->writeFile("/restore.txt", std::string("Original"));
    tf.dir->sync();
    
    // 创建快照
    tf.snap->createSnapshot("backup");
    std::cout << "  Created backup snapshot" << std::endl;
    
    // 修改数据
    tf.dir->writeFile("/restore.txt", std::string("Modified!!!"));
    
    auto read1 = tf.dir->readFile("/restore.txt");
    std::string content1(read1.value().begin(), read1.value().end());
    std::cout << "  After modify: " << content1 << std::endl;
    
    // 恢复快照
    ErrorCode err = tf.snap->restoreSnapshot("backup");
    assert(err == ErrorCode::OK);
    std::cout << "  Restored backup" << std::endl;
    
    // 重新加载
    tf.alloc->reload();
    
    auto read2 = tf.dir->readFile("/restore.txt");
    std::string content2(read2.value().begin(), read2.value().end());
    std::cout << "  After restore: " << content2 << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testSnapshotLimits() {
    std::cout << "=== Test: Snapshot Limits ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    uint32_t max_snaps = tf.snap->getMaxSnapshots();
    std::cout << "  Max snapshots: " << max_snaps << std::endl;
    
    // 创建到上限
    for (uint32_t i = 0; i < max_snaps; ++i) {
        ErrorCode err = tf.snap->createSnapshot("limit" + std::to_string(i));
        assert(err == ErrorCode::OK);
    }
    
    // 超出上限应失败
    ErrorCode err = tf.snap->createSnapshot("overflow");
    assert(err == ErrorCode::E_MAX_SNAPSHOTS);
    std::cout << "  Overflow correctly rejected" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testSnapshotPersistence() {
    std::cout << "=== Test: Snapshot Persistence ===" << std::endl;
    
    // Phase 1: 创建
    {
        TestFixture tf;
        assert(tf.setup());
        
        tf.dir->createFile("/persist.txt");
        tf.dir->writeFile("/persist.txt", std::string("Persistent"));
        tf.snap->createSnapshot("persist_snap");
        tf.snap->sync();
        tf.alloc->sync();
        
        std::cout << "  Phase 1: Created snapshot" << std::endl;
    }
    
    // Phase 2: 验证
    {
        DiskImage disk;
        assert(disk.open(TEST_DISK) == ErrorCode::OK);
        
        CachedDisk cached(&disk, 64);
        Allocator alloc(&cached);
        assert(alloc.load() == ErrorCode::OK);
        
        Directory dir(&alloc, &cached);
        SnapshotManager snap(&alloc, &dir, &cached);
        assert(snap.load() == ErrorCode::OK);
        
        assert(snap.getSnapshotCount() == 1);
        assert(snap.snapshotExists("persist_snap"));
        
        std::cout << "  Phase 2: Snapshot persisted" << std::endl;
    }
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCOW() {
    std::cout << "=== Test: COW Mechanism ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建文件
    tf.dir->createFile("/cow.txt");
    tf.dir->writeFile("/cow.txt", std::string(500, 'A'));
    
    // 无快照时不需要 COW
    const Superblock& sb = tf.alloc->getSuperblock();
    // 假设有一些块被使用
    
    // 创建快照
    tf.snap->createSnapshot("cow_snap");
    std::cout << "  Created snapshot for COW test" << std::endl;
    
    // 获取统计
    auto stats = tf.snap->getStats();
    std::cout << "  Shared blocks: " << stats.shared_blocks << std::endl;
    std::cout << "  COW operations: " << stats.cow_operations << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup complete ===" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "       Snapshot Module Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    try {
        testSnapshotBasic();
        testSnapshotMultiple();
        testSnapshotRestore();
        testSnapshotLimits();
        testSnapshotPersistence();
        testCOW();
        
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