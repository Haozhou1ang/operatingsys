// filesystem/src/test_fs.cpp
#include "FS.h"
#include <iostream>
#include <cassert>

using namespace fs;

const char* TEST_DISK = "test_fs.img";

void testFormatAndMount() {
    std::cout << "=== Test: Format and Mount ===" << std::endl;
    
    FileSystem fs;
    
    // 格式化
    ErrorCode err = fs.format(TEST_DISK, 2048, 256);
    assert(err == ErrorCode::OK);
    std::cout << "  Format: OK" << std::endl;
    
    // 挂载
    err = fs.mount(TEST_DISK);
    assert(err == ErrorCode::OK);
    assert(fs.isMounted());
    std::cout << "  Mount: OK" << std::endl;
    
    // 获取信息
    FSInfo info = fs.getInfo();
    std::cout << "  FS Info:" << std::endl;
    std::cout << "    Total blocks: " << info.total_blocks << std::endl;
    std::cout << "    Free blocks: " << info.free_blocks << std::endl;
    std::cout << "    Total inodes: " << info.total_inodes << std::endl;
    std::cout << "    Free inodes: " << info.free_inodes << std::endl;
    
    // 卸载
    err = fs.unmount();
    assert(err == ErrorCode::OK);
    assert(!fs.isMounted());
    std::cout << "  Unmount: OK" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testDirectoryOperations() {
    std::cout << "=== Test: Directory Operations ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    // mkdir
    assert(fs.mkdir("/dir1") == ErrorCode::OK);
    assert(fs.exists("/dir1"));
    assert(fs.isDirectory("/dir1"));
    std::cout << "  mkdir /dir1: OK" << std::endl;
    
    // mkdirp
    assert(fs.mkdirp("/a/b/c/d") == ErrorCode::OK);
    assert(fs.exists("/a/b/c/d"));
    std::cout << "  mkdirp /a/b/c/d: OK" << std::endl;
    
    // readdir
    auto list = fs.readdir("/");
    assert(list.ok());
    std::cout << "  Root contains " << list.value().size() << " entries" << std::endl;
    
    // rmdir
    assert(fs.rmdir("/dir1") == ErrorCode::OK);
    assert(!fs.exists("/dir1"));
    std::cout << "  rmdir /dir1: OK" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testFileOperations() {
    std::cout << "=== Test: File Operations ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    // create
    assert(fs.create("/test.txt") == ErrorCode::OK);
    assert(fs.exists("/test.txt"));
    assert(fs.isFile("/test.txt"));
    std::cout << "  create /test.txt: OK" << std::endl;
    
    // writeFile
    auto write_result = fs.writeFile("/test.txt", "Hello, World!");
    assert(write_result.ok());
    assert(write_result.value() == 13);
    std::cout << "  writeFile: wrote " << write_result.value() << " bytes" << std::endl;
    
    // readFile
    auto read_result = fs.readFileAsString("/test.txt");
    assert(read_result.ok());
    assert(read_result.value() == "Hello, World!");
    std::cout << "  readFile: \"" << read_result.value() << "\"" << std::endl;
    
    // appendFile
    fs.appendFile("/test.txt", " More data.");
    read_result = fs.readFileAsString("/test.txt");
    assert(read_result.value() == "Hello, World! More data.");
    std::cout << "  appendFile: OK" << std::endl;
    
    // getFileSize
    auto size = fs.getFileSize("/test.txt");
    assert(size.ok());
    std::cout << "  File size: " << size.value() << " bytes" << std::endl;
    
    // truncate
    fs.truncate("/test.txt", 5);
    read_result = fs.readFileAsString("/test.txt");
    assert(read_result.value() == "Hello");
    std::cout << "  truncate to 5: \"" << read_result.value() << "\"" << std::endl;
    
    // unlink
    assert(fs.unlink("/test.txt") == ErrorCode::OK);
    assert(!fs.exists("/test.txt"));
    std::cout << "  unlink: OK" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCopyAndMove() {
    std::cout << "=== Test: Copy and Move ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    fs.create("/original.txt");
    fs.writeFile("/original.txt", "Original content");
    
    // copyFile
    assert(fs.copyFile("/original.txt", "/copy.txt") == ErrorCode::OK);
    assert(fs.exists("/copy.txt"));
    auto content = fs.readFileAsString("/copy.txt");
    assert(content.value() == "Original content");
    std::cout << "  copyFile: OK" << std::endl;
    
    // moveFile
    assert(fs.moveFile("/copy.txt", "/moved.txt") == ErrorCode::OK);
    assert(!fs.exists("/copy.txt"));
    assert(fs.exists("/moved.txt"));
    content = fs.readFileAsString("/moved.txt");
    assert(content.value() == "Original content");
    std::cout << "  moveFile: OK" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testSnapshots() {
    std::cout << "=== Test: Snapshots ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    // 创建原始数据
    fs.create("/data.txt");
    fs.writeFile("/data.txt", "Version 1");
    
    // 创建快照
    assert(fs.createSnapshot("v1") == ErrorCode::OK);
    assert(fs.snapshotExists("v1"));
    std::cout << "  createSnapshot 'v1': OK" << std::endl;
    
    // 修改数据
    fs.writeFile("/data.txt", "Version 2 - Modified");
    
    // 列出快照
    auto snapshots = fs.listSnapshots();
    std::cout << "  Snapshots: " << snapshots.size() << std::endl;
    for (const auto& s : snapshots) {
        std::cout << "    - " << s.name << std::endl;
    }
    
    // 恢复快照
    assert(fs.restoreSnapshot("v1") == ErrorCode::OK);
    auto content = fs.readFileAsString("/data.txt");
    assert(content.ok());
    std::cout << "  After restore: \"" << content.value() << "\"" << std::endl;
    
    // 删除快照
    assert(fs.deleteSnapshot("v1") == ErrorCode::OK);
    assert(!fs.snapshotExists("v1"));
    std::cout << "  deleteSnapshot: OK" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCacheStats() {
    std::cout << "=== Test: Cache Stats ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    
    FSConfig config;
    config.cache_capacity = 32;
    config.enable_cache = true;
    fs.mount(TEST_DISK, config);
    
    // 执行一些操作
    fs.mkdir("/cache_test");
    for (int i = 0; i < 10; ++i) {
        std::string path = "/cache_test/file" + std::to_string(i) + ".txt";
        fs.create(path);
        fs.writeFile(path, "Content " + std::to_string(i));
    }
    
    // 多次读取
    for (int j = 0; j < 5; ++j) {
        for (int i = 0; i < 10; ++i) {
            std::string path = "/cache_test/file" + std::to_string(i) + ".txt";
            fs.readFile(path);
        }
    }
    
    auto stats = fs.getCacheStats();
    std::cout << "  Cache stats:" << std::endl;
    std::cout << "    Hits: " << stats.hits << std::endl;
    std::cout << "    Misses: " << stats.misses << std::endl;
    std::cout << "    Hit rate: " << (stats.hit_rate * 100) << "%" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testPrintTree() {
    std::cout << "=== Test: Print Tree ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    // 创建复杂结构
    fs.mkdirp("/papers/paper001/versions");
    fs.mkdirp("/papers/paper001/reviews");
    fs.mkdirp("/users/admin");
    fs.mkdirp("/users/reviewer1");
    fs.mkdir("/config");
    
    fs.create("/papers/paper001/metadata.json");
    fs.writeFile("/papers/paper001/metadata.json", R"({"title": "Test Paper"})");
    
    fs.create("/papers/paper001/versions/v1.pdf");
    fs.writeFile("/papers/paper001/versions/v1.pdf", std::string(1000, 'X'));
    
    fs.create("/papers/paper001/reviews/review1.txt");
    fs.writeFile("/papers/paper001/reviews/review1.txt", "Great paper!");
    
    fs.create("/config/settings.ini");
    fs.writeFile("/config/settings.ini", "debug=true");
    
    std::cout << "  Directory tree:" << std::endl;
    fs.printTree("/");
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testRemoveRecursive() {
    std::cout << "=== Test: Remove Recursive ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    // 创建目录结构
    fs.mkdirp("/to_delete/sub1/sub2");
    fs.create("/to_delete/file1.txt");
    fs.create("/to_delete/sub1/file2.txt");
    fs.create("/to_delete/sub1/sub2/file3.txt");
    
    assert(fs.exists("/to_delete"));
    std::cout << "  Created structure" << std::endl;
    
    // 递归删除
    assert(fs.removeRecursive("/to_delete") == ErrorCode::OK);
    assert(!fs.exists("/to_delete"));
    std::cout << "  removeRecursive: OK" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testGetDirSize() {
    std::cout << "=== Test: Get Dir Size ===" << std::endl;
    
    FileSystem fs;
    fs.format(TEST_DISK, 2048, 256);
    fs.mount(TEST_DISK);
    
    fs.mkdirp("/measure/sub");
    fs.create("/measure/file1.txt");
    fs.writeFile("/measure/file1.txt", std::string(100, 'A'));
    fs.create("/measure/file2.txt");
    fs.writeFile("/measure/file2.txt", std::string(200, 'B'));
    fs.create("/measure/sub/file3.txt");
    fs.writeFile("/measure/sub/file3.txt", std::string(300, 'C'));
    
    auto size = fs.getDirSize("/measure");
    assert(size.ok());
    assert(size.value() == 600);
    std::cout << "  Total size: " << size.value() << " bytes" << std::endl;
    
    fs.unmount();
    std::cout << "PASSED" << std::endl << std::endl;
}

void testPersistence() {
    std::cout << "=== Test: Persistence ===" << std::endl;
    
    // Phase 1: 创建数据
    {
        FileSystem fs;
        fs.format(TEST_DISK, 2048, 256);
        fs.mount(TEST_DISK);
        
        fs.mkdir("/persist");
        fs.create("/persist/data.txt");
        fs.writeFile("/persist/data.txt", "Persistent Data!");
        
        fs.sync();
        fs.unmount();
        
        std::cout << "  Phase 1: Data created" << std::endl;
    }
    
    // Phase 2: 验证数据
    {
        FileSystem fs;
        fs.mount(TEST_DISK);
        
        assert(fs.exists("/persist/data.txt"));
        auto content = fs.readFileAsString("/persist/data.txt");
        assert(content.ok());
        assert(content.value() == "Persistent Data!");
        
        std::cout << "  Phase 2: Data verified: \"" << content.value() << "\"" << std::endl;
        
        fs.unmount();
    }
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup complete ===" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "       FileSystem Unified API Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    try {
        testFormatAndMount();
        testDirectoryOperations();
        testFileOperations();
        testCopyAndMove();
        testSnapshots();
        testCacheStats();
        testPrintTree();
        testRemoveRecursive();
        testGetDirSize();
        testPersistence();
        
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