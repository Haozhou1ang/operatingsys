// filesystem/src/test_diskimage.cpp
#include "DiskImage.h"
#include <iostream>
#include <cstring>
#include <cassert>

using namespace fs;

void testMkfs() {
    std::cout << "=== Test mkfs ===" << std::endl;
    
    MkfsOptions opts;
    opts.total_blocks = 1024;  // 1MB
    opts.total_inodes = 128;
    opts.force = true;
    opts.verbose = true;
    
    MkfsResult result = mkfs("test_disk.img", opts);
    
    assert(result.error == ErrorCode::OK);
    std::cout << "mkfs passed!" << std::endl << std::endl;
}

void testOpenAndRead() {
    std::cout << "=== Test Open and Read ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open("test_disk.img");
    assert(err == ErrorCode::OK);
    assert(disk.isOpen());
    
    // 读取 Superblock
    Superblock sb;
    err = disk.loadSuperblock(sb);
    assert(err == ErrorCode::OK);
    assert(sb.validate());
    
    std::cout << "Superblock info:" << std::endl;
    std::cout << "  Magic: 0x" << std::hex << sb.magic << std::dec << std::endl;
    std::cout << "  Total blocks: " << sb.total_blocks << std::endl;
    std::cout << "  Total inodes: " << sb.total_inodes << std::endl;
    std::cout << "  Free blocks: " << sb.free_blocks << std::endl;
    std::cout << "  Free inodes: " << sb.free_inodes << std::endl;
    std::cout << "  Data block start: " << sb.data_block_start << std::endl;
    
    disk.close();
    std::cout << "Open and read passed!" << std::endl << std::endl;
}

void testRootDirectory() {
    std::cout << "=== Test Root Directory ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open("test_disk.img");
    assert(err == ErrorCode::OK);
    
    Superblock sb;
    err = disk.loadSuperblock(sb);
    assert(err == ErrorCode::OK);
    
    // 读取根目录 inode
    uint8_t inode_block[BLOCK_SIZE];
    err = disk.readBlock(sb.inode_table_start, inode_block);
    assert(err == ErrorCode::OK);
    
    Inode* inodes = reinterpret_cast<Inode*>(inode_block);
    Inode& root = inodes[ROOT_INODE];
    
    std::cout << "Root inode:" << std::endl;
    std::cout << "  Type: " << (root.isDirectory() ? "DIRECTORY" : "OTHER") << std::endl;
    std::cout << "  Size: " << root.size << std::endl;
    std::cout << "  Link count: " << root.link_count << std::endl;
    std::cout << "  First block: " << root.direct_blocks[0] << std::endl;
    
    assert(root.isDirectory());
    assert(root.direct_blocks[0] == sb.data_block_start);
    
    // 读取根目录内容
    uint8_t dir_block[BLOCK_SIZE];
    err = disk.readBlock(root.direct_blocks[0], dir_block);
    assert(err == ErrorCode::OK);
    
    DirEntry* entries = reinterpret_cast<DirEntry*>(dir_block);
    
    std::cout << "Root directory entries:" << std::endl;
    for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
        if (entries[i].isValid()) {
            std::cout << "  [" << i << "] " << entries[i].getName() 
                      << " -> inode " << entries[i].inode << std::endl;
        }
    }
    
    assert(entries[0].getName() == ".");
    assert(entries[0].inode == ROOT_INODE);
    assert(entries[1].getName() == "..");
    assert(entries[1].inode == ROOT_INODE);
    
    disk.close();
    std::cout << "Root directory test passed!" << std::endl << std::endl;
}

void testBlockReadWrite() {
    std::cout << "=== Test Block Read/Write ===" << std::endl;
    
    DiskImage disk;
    ErrorCode err = disk.open("test_disk.img");
    assert(err == ErrorCode::OK);
    
    Superblock sb;
    err = disk.loadSuperblock(sb);
    assert(err == ErrorCode::OK);
    
    // 使用一个未分配的数据块进行测试
    BlockNo test_block = sb.data_block_start + 10;
    
    // 写入测试数据
    uint8_t write_buf[BLOCK_SIZE];
    for (uint32_t i = 0; i < BLOCK_SIZE; ++i) {
        write_buf[i] = static_cast<uint8_t>(i % 256);
    }
    
    err = disk.writeBlock(test_block, write_buf);
    assert(err == ErrorCode::OK);
    
    // 读取并验证
    uint8_t read_buf[BLOCK_SIZE];
    err = disk.readBlock(test_block, read_buf);
    assert(err == ErrorCode::OK);
    
    assert(std::memcmp(write_buf, read_buf, BLOCK_SIZE) == 0);
    
    // 测试 zeroBlock
    err = disk.zeroBlock(test_block);
    assert(err == ErrorCode::OK);
    
    err = disk.readBlock(test_block, read_buf);
    assert(err == ErrorCode::OK);
    
    for (uint32_t i = 0; i < BLOCK_SIZE; ++i) {
        assert(read_buf[i] == 0);
    }
    
    // 显示 I/O 统计
    auto stats = disk.getIOStats();
    std::cout << "I/O Stats:" << std::endl;
    std::cout << "  Reads: " << stats.reads << std::endl;
    std::cout << "  Writes: " << stats.writes << std::endl;
    std::cout << "  Bytes read: " << stats.bytes_read << std::endl;
    std::cout << "  Bytes written: " << stats.bytes_written << std::endl;
    
    disk.close();
    std::cout << "Block read/write test passed!" << std::endl << std::endl;
}

void testCheckfs() {
    std::cout << "=== Test checkfs ===" << std::endl;
    
    assert(checkfs("test_disk.img") == true);
    assert(checkfs("nonexistent.img") == false);
    
    std::cout << "checkfs test passed!" << std::endl << std::endl;
}

int main() {
    std::cout << "DiskImage and mkfs Tests" << std::endl;
    std::cout << "========================" << std::endl << std::endl;
    
    testMkfs();
    testOpenAndRead();
    testRootDirectory();
    testBlockReadWrite();
    testCheckfs();
    
    std::cout << "All tests passed!" << std::endl;
    
    // 清理测试文件
    std::remove("test_disk.img");
    
    return 0;
}