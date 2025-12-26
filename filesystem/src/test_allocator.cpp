// filesystem/src/test_allocator.cpp
#include "DiskImage.h"
#include "Allocator.h"
#include <iostream>
#include <cassert>
#include <vector>

using namespace fs;

const char* TEST_DISK = "test_alloc.img";

void setup() {
    std::cout << "=== Setup: Creating test filesystem ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 1024;
    opts.total_inodes = 64;
    opts.force = true;
    opts.verbose = false;
    
    MkfsResult result = mkfs(TEST_DISK, opts);
    assert(result.error == ErrorCode::OK);
    
    std::cout << "Created filesystem with:" << std::endl;
    std::cout << "  Total blocks: " << result.total_blocks << std::endl;
    std::cout << "  Total inodes: " << result.total_inodes << std::endl;
    std::cout << "  Data blocks: " << result.data_blocks << std::endl;
    std::cout << std::endl;
}

void testLoadAndStats() {
    std::cout << "=== Test: Load and Stats ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    std::cout << "Allocator stats:" << std::endl;
    std::cout << "  Total inodes: " << alloc.getTotalInodeCount() << std::endl;
    std::cout << "  Used inodes: " << alloc.getUsedInodeCount() << std::endl;
    std::cout << "  Free inodes: " << alloc.getFreeInodeCount() << std::endl;
    std::cout << "  Total blocks: " << alloc.getTotalBlockCount() << std::endl;
    std::cout << "  Used blocks: " << alloc.getUsedBlockCount() << std::endl;
    std::cout << "  Free blocks: " << alloc.getFreeBlockCount() << std::endl;
    
    // 验证初始状态（根目录占用 1 个 inode 和 1 个 block）
    assert(alloc.getUsedInodeCount() == 1);
    assert(alloc.getUsedBlockCount() == 1);
    assert(alloc.isInodeAllocated(ROOT_INODE));
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testInodeAllocation() {
    std::cout << "=== Test: Inode Allocation ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    uint32_t initial_free = alloc.getFreeInodeCount();
    std::cout << "Initial free inodes: " << initial_free << std::endl;
    
    // 分配 5 个 inode
    std::vector<InodeId> allocated;
    for (int i = 0; i < 5; ++i) {
        auto result = alloc.allocInode();
        assert(result.ok());
        allocated.push_back(result.value());
        std::cout << "  Allocated inode: " << result.value() << std::endl;
    }
    
    assert(alloc.getFreeInodeCount() == initial_free - 5);
    assert(alloc.getUsedInodeCount() == 6);  // 1 root + 5 new
    
    // 验证都已分配
    for (InodeId id : allocated) {
        assert(alloc.isInodeAllocated(id));
    }
    
    // 释放其中 2 个
    err = alloc.freeInode(allocated[1]);
    assert(err == ErrorCode::OK);
    err = alloc.freeInode(allocated[3]);
    assert(err == ErrorCode::OK);
    
    assert(alloc.getFreeInodeCount() == initial_free - 3);
    assert(!alloc.isInodeAllocated(allocated[1]));
    assert(!alloc.isInodeAllocated(allocated[3]));
    
    // 尝试释放根目录（应失败）
    err = alloc.freeInode(ROOT_INODE);
    assert(err == ErrorCode::E_PERMISSION);
    
    // 同步到磁盘
    err = alloc.sync();
    assert(err == ErrorCode::OK);
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testBlockAllocation() {
    std::cout << "=== Test: Block Allocation ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    uint32_t initial_free = alloc.getFreeBlockCount();
    std::cout << "Initial free blocks: " << initial_free << std::endl;
    
    // 分配 10 个块
    std::vector<BlockNo> allocated;
    for (int i = 0; i < 10; ++i) {
        auto result = alloc.allocBlock();
        assert(result.ok());
        allocated.push_back(result.value());
        std::cout << "  Allocated block: " << result.value() << std::endl;
    }
    
    assert(alloc.getFreeBlockCount() == initial_free - 10);
    
    // 验证都已分配
    for (BlockNo b : allocated) {
        assert(alloc.isBlockAllocated(b));
    }
    
    // 释放其中 5 个
    for (int i = 0; i < 5; ++i) {
        err = alloc.freeBlock(allocated[i]);
        assert(err == ErrorCode::OK);
    }
    
    assert(alloc.getFreeBlockCount() == initial_free - 5);
    
    // 批量分配
    auto batch_result = alloc.allocBlocks(3);
    assert(batch_result.ok());
    assert(batch_result.value().size() == 3);
    std::cout << "  Batch allocated 3 blocks" << std::endl;
    
    // 同步
    err = alloc.sync();
    assert(err == ErrorCode::OK);
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testInodeReadWrite() {
    std::cout << "=== Test: Inode Read/Write ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    // 读取根目录 inode
    auto root_result = alloc.readInode(ROOT_INODE);
    assert(root_result.ok());
    
    Inode& root = root_result.value();
    std::cout << "Root inode:" << std::endl;
    std::cout << "  Type: " << (root.isDirectory() ? "DIRECTORY" : "OTHER") << std::endl;
    std::cout << "  Size: " << root.size << std::endl;
    std::cout << "  Links: " << root.link_count << std::endl;
    
    assert(root.isDirectory());
    
    // 分配新 inode 并设置为文件
    auto alloc_result = alloc.allocInode();
    assert(alloc_result.ok());
    InodeId new_id = alloc_result.value();
    
    // 读取新 inode
    auto new_result = alloc.readInode(new_id);
    assert(new_result.ok());
    
    Inode new_inode = new_result.value();
    new_inode.type = FileType::REGULAR;
    new_inode.size = 1234;
    new_inode.create_time = 1234567890;
    new_inode.modify_time = 1234567890;
    new_inode.access_time = 1234567890;
    
    // 写回
    err = alloc.writeInode(new_id, new_inode);
    assert(err == ErrorCode::OK);
    
    // 重新读取验证
    auto verify_result = alloc.readInode(new_id);
    assert(verify_result.ok());
    
    assert(verify_result.value().type == FileType::REGULAR);
    assert(verify_result.value().size == 1234);
    
    alloc.sync();
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testRefCount() {
    std::cout << "=== Test: Reference Counting ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    // 分配一个块
    auto alloc_result = alloc.allocBlock();
    assert(alloc_result.ok());
    BlockNo block = alloc_result.value();
    
    std::cout << "Allocated block: " << block << std::endl;
    
    // 初始引用计数应为 1
    assert(alloc.getBlockRef(block) == 1);
    
    // 增加引用计数
    auto inc_result = alloc.incBlockRef(block);
    assert(inc_result.ok());
    assert(inc_result.value() == 2);
    std::cout << "After incBlockRef: " << alloc.getBlockRef(block) << std::endl;
    
    // 再增加
    inc_result = alloc.incBlockRef(block);
    assert(inc_result.ok());
    assert(inc_result.value() == 3);
    
    // 减少引用计数（不应释放）
    auto dec_result = alloc.decBlockRef(block);
    assert(dec_result.ok());
    assert(dec_result.value() == 2);
    assert(alloc.isBlockAllocated(block));
    
    // 继续减少
    dec_result = alloc.decBlockRef(block);
    assert(dec_result.ok());
    assert(dec_result.value() == 1);
    
    // 最后一次减少（应该释放）
    dec_result = alloc.decBlockRef(block);
    assert(dec_result.ok());
    assert(dec_result.value() == 0);
    assert(!alloc.isBlockAllocated(block));
    
    std::cout << "Block released after refcount reached 0" << std::endl;
    
    alloc.sync();
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testConsistency() {
    std::cout << "=== Test: Consistency Check ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    // 检查一致性
    err = alloc.checkConsistency(false);
    std::cout << "Consistency check result: " 
              << (err == ErrorCode::OK ? "OK" : "ERRORS FOUND") << std::endl;
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testPersistence() {
    std::cout << "=== Test: Persistence ===" << std::endl;
    
    InodeId saved_inode;
    BlockNo saved_block;
    
    // 第一阶段：分配并保存
    {
        DiskImage disk;
        ErrorCode err = disk.open(TEST_DISK);
        assert(err == ErrorCode::OK);
        
        Allocator alloc(&disk);
        err = alloc.load();
        assert(err == ErrorCode::OK);
        
        auto inode_result = alloc.allocInode();
        assert(inode_result.ok());
        saved_inode = inode_result.value();
        
        auto block_result = alloc.allocBlock();
        assert(block_result.ok());
        saved_block = block_result.value();
        
        // 写入一些数据到 inode
        Inode test_inode;
        test_inode.init(FileType::REGULAR);
        test_inode.size = 9999;
        test_inode.direct_blocks[0] = saved_block;
        alloc.writeInode(saved_inode, test_inode);
        
        alloc.sync();
        disk.close();
        
        std::cout << "Saved inode: " << saved_inode << ", block: " << saved_block << std::endl;
    }
    
    // 第二阶段：重新打开并验证
    {
        DiskImage disk;
        ErrorCode err = disk.open(TEST_DISK);
        assert(err == ErrorCode::OK);
        
        Allocator alloc(&disk);
        err = alloc.load();
        assert(err == ErrorCode::OK);
        
        // 验证 inode 仍然分配
        assert(alloc.isInodeAllocated(saved_inode));
        
        // 验证 block 仍然分配
        assert(alloc.isBlockAllocated(saved_block));
        
        // 验证 inode 数据
        auto inode_result = alloc.readInode(saved_inode);
        assert(inode_result.ok());
        assert(inode_result.value().size == 9999);
        assert(inode_result.value().direct_blocks[0] == saved_block);
        
        disk.close();
        std::cout << "Verified persistence" << std::endl;
    }
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testAllocStats() {
    std::cout << "=== Test: Allocation Stats ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open(TEST_DISK);
    assert(err == ErrorCode::OK);
    
    Allocator alloc(&disk);
    err = alloc.load();
    assert(err == ErrorCode::OK);
    
    alloc.resetAllocStats();
    
    // 执行一些操作
    for (int i = 0; i < 5; ++i) {
        auto r = alloc.allocInode();
        assert(r.ok());
    }
    
    for (int i = 0; i < 8; ++i) {
        auto r = alloc.allocBlock();
        assert(r.ok());
    }
    
    auto stats = alloc.getAllocStats();
    std::cout << "Allocation stats:" << std::endl;
    std::cout << "  Inode allocs: " << stats.inode_allocs << std::endl;
    std::cout << "  Inode frees: " << stats.inode_frees << std::endl;
    std::cout << "  Block allocs: " << stats.block_allocs << std::endl;
    std::cout << "  Block frees: " << stats.block_frees << std::endl;
    std::cout << "  Bitmap reads: " << stats.bitmap_reads << std::endl;
    std::cout << "  Bitmap writes: " << stats.bitmap_writes << std::endl;
    
    assert(stats.inode_allocs == 5);
    assert(stats.block_allocs == 8);
    
    disk.close();
    std::cout << "PASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup complete ===" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "       Allocator Module Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    setup();
    
    testLoadAndStats();
    testInodeAllocation();
    testBlockAllocation();
    testInodeReadWrite();
    testRefCount();
    testConsistency();
    testPersistence();
    testAllocStats();
    
    cleanup();
    
    std::cout << std::endl;
    std::cout << "==========================================" << std::endl;
    std::cout << "       All tests passed!" << std::endl;
    std::cout << "==========================================" << std::endl;
    
    return 0;
}