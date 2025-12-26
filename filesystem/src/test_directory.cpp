// filesystem/src/test_directory.cpp
#include "DiskImage.h"
#include "Allocator.h"
#include "Directory.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <functional> 

using namespace fs;

const char* TEST_DISK = "test_dir.img";

//==============================================================================
// 测试夹具类
//==============================================================================

class TestFixture {
public:
    DiskImage disk;
    Allocator* alloc;
    Directory* dir;
    
    TestFixture() : alloc(nullptr), dir(nullptr) {}
    
    ~TestFixture() {
        cleanup();
    }
    
    void cleanup() {
        delete dir;
        dir = nullptr;
        delete alloc;
        alloc = nullptr;
        disk.close();
    }
    
    bool setup() {
        cleanup();
        
        // 创建文件系统
        MkfsOptions opts;
        opts.total_blocks = 2048;
        opts.total_inodes = 128;
        opts.force = true;
        
        MkfsResult result = mkfs(TEST_DISK, opts);
        if (result.error != ErrorCode::OK) {
            std::cerr << "mkfs failed: " << errorCodeToString(result.error) << std::endl;
            return false;
        }
        
        // 打开磁盘
        if (disk.open(TEST_DISK) != ErrorCode::OK) {
            std::cerr << "Failed to open disk" << std::endl;
            return false;
        }
        
        // 创建分配器
        alloc = new Allocator(&disk);
        if (alloc->load() != ErrorCode::OK) {
            std::cerr << "Failed to load allocator" << std::endl;
            return false;
        }
        
        // 创建目录管理器
        dir = new Directory(alloc, &disk);
        
        return true;
    }
};

//==============================================================================
// 测试用例
//==============================================================================

void testPathOperations() {
    std::cout << "=== Test: Path Operations ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 测试根目录解析
    auto result = tf.dir->resolvePath("/");
    assert(result.ok());
    assert(result.value() == ROOT_INODE);
    std::cout << "  Root path resolved to inode " << result.value() << std::endl;
    
    // 测试根目录 stat
    auto stat = tf.dir->stat("/");
    assert(stat.ok());
    assert(stat.value().type == FileType::DIRECTORY);
    std::cout << "  Root directory size: " << stat.value().size << std::endl;
    
    // 测试不存在的路径
    result = tf.dir->resolvePath("/nonexistent");
    assert(!result.ok());
    assert(result.error() == ErrorCode::E_NOT_FOUND);
    std::cout << "  Nonexistent path correctly returns E_NOT_FOUND" << std::endl;
    
    // 测试 exists 函数
    assert(tf.dir->exists("/"));
    assert(!tf.dir->exists("/nonexistent"));
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testDirectoryListing() {
    std::cout << "=== Test: Directory Listing ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 列出根目录
    auto list_result = tf.dir->list("/");
    assert(list_result.ok());
    
    std::cout << "  Root directory entries (" << list_result.value().size() << "):" << std::endl;
    for (const auto& entry : list_result.value()) {
        const char* type_str = (entry.file_type == FileType::DIRECTORY) ? "DIR" : "FILE";
        std::cout << "    [" << type_str << "] " << entry.getName() 
                  << " (inode=" << entry.inode << ")" << std::endl;
    }
    
    // 应该有 . 和 ..
    bool has_dot = false, has_dotdot = false;
    for (const auto& entry : list_result.value()) {
        if (entry.getName() == ".") has_dot = true;
        if (entry.getName() == "..") has_dotdot = true;
    }
    assert(has_dot && has_dotdot);
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testMkdir() {
    std::cout << "=== Test: mkdir ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建目录
    auto result = tf.dir->mkdir("/testdir");
    assert(result.ok());
    std::cout << "  Created /testdir with inode " << result.value() << std::endl;
    
    // 验证存在
    assert(tf.dir->exists("/testdir"));
    assert(tf.dir->isDirectory("/testdir"));
    assert(!tf.dir->isFile("/testdir"));
    
    // 创建嵌套目录
    result = tf.dir->mkdir("/testdir/subdir");
    assert(result.ok());
    std::cout << "  Created /testdir/subdir with inode " << result.value() << std::endl;
    
    // 创建多层嵌套
    result = tf.dir->mkdir("/testdir/subdir/deep");
    assert(result.ok());
    std::cout << "  Created /testdir/subdir/deep" << std::endl;
    
    // 列出 testdir
    auto list_result = tf.dir->list("/testdir");
    assert(list_result.ok());
    std::cout << "  /testdir contents:" << std::endl;
    for (const auto& entry : list_result.value()) {
        std::cout << "    " << entry.getName() << std::endl;
    }
    
    // 尝试创建已存在的目录
    result = tf.dir->mkdir("/testdir");
    assert(!result.ok());
    assert(result.error() == ErrorCode::E_ALREADY_EXISTS);
    std::cout << "  Duplicate mkdir correctly rejected" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testRmdir() {
    std::cout << "=== Test: rmdir ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建目录
    tf.dir->mkdir("/toremove");
    assert(tf.dir->exists("/toremove"));
    
    // 删除空目录
    ErrorCode err = tf.dir->rmdir("/toremove");
    assert(err == ErrorCode::OK);
    assert(!tf.dir->exists("/toremove"));
    std::cout << "  Removed empty directory /toremove" << std::endl;
    
    // 创建非空目录
    tf.dir->mkdir("/nonempty");
    tf.dir->mkdir("/nonempty/child");
    
    // 尝试删除非空目录（应失败）
    err = tf.dir->rmdir("/nonempty");
    assert(err == ErrorCode::E_NOT_EMPTY);
    std::cout << "  Non-empty rmdir correctly rejected" << std::endl;
    
    // 先删除子目录，再删除父目录
    err = tf.dir->rmdir("/nonempty/child");
    assert(err == ErrorCode::OK);
    err = tf.dir->rmdir("/nonempty");
    assert(err == ErrorCode::OK);
    std::cout << "  Removed /nonempty after removing child" << std::endl;
    
    // 尝试删除根目录（应失败）
    err = tf.dir->rmdir("/");
    assert(err == ErrorCode::E_PERMISSION);
    std::cout << "  Root rmdir correctly rejected" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testCreateFile() {
    std::cout << "=== Test: Create File ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建文件
    auto result = tf.dir->createFile("/test.txt");
    assert(result.ok());
    std::cout << "  Created /test.txt with inode " << result.value() << std::endl;
    
    // 验证
    assert(tf.dir->exists("/test.txt"));
    assert(tf.dir->isFile("/test.txt"));
    assert(!tf.dir->isDirectory("/test.txt"));
    
    // 获取 stat
    auto stat = tf.dir->stat("/test.txt");
    assert(stat.ok());
    assert(stat.value().type == FileType::REGULAR);
    assert(stat.value().size == 0);
    std::cout << "  File size: " << stat.value().size << std::endl;
    
    // 创建目录中的文件
    tf.dir->mkdir("/docs");
    result = tf.dir->createFile("/docs/readme.md");
    assert(result.ok());
    std::cout << "  Created /docs/readme.md" << std::endl;
    
    // 列出 /docs
    auto list = tf.dir->list("/docs");
    assert(list.ok());
    std::cout << "  /docs contents: ";
    for (const auto& e : list.value()) {
        std::cout << e.getName() << " ";
    }
    std::cout << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testRemoveFile() {
    std::cout << "=== Test: Remove File ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建并删除文件
    tf.dir->createFile("/todelete.txt");
    assert(tf.dir->exists("/todelete.txt"));
    
    ErrorCode err = tf.dir->removeFile("/todelete.txt");
    assert(err == ErrorCode::OK);
    assert(!tf.dir->exists("/todelete.txt"));
    std::cout << "  Removed /todelete.txt" << std::endl;
    
    // 尝试删除不存在的文件
    err = tf.dir->removeFile("/nonexistent.txt");
    assert(err == ErrorCode::E_NOT_FOUND);
    std::cout << "  Nonexistent file removal correctly rejected" << std::endl;
    
    // 尝试用 removeFile 删除目录（应失败）
    tf.dir->mkdir("/adir");
    err = tf.dir->removeFile("/adir");
    assert(err == ErrorCode::E_IS_DIR);
    std::cout << "  Directory removal via removeFile correctly rejected" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testFileReadWrite() {
    std::cout << "=== Test: File Read/Write ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建文件
    tf.dir->createFile("/hello.txt");
    
    // 写入数据
    std::string test_data = "Hello, World!";
    auto write_result = tf.dir->writeFile("/hello.txt", test_data);
    assert(write_result.ok());
    assert(write_result.value() == test_data.size());
    std::cout << "  Wrote " << write_result.value() << " bytes" << std::endl;
    
    // 读取数据
    auto read_result = tf.dir->readFile("/hello.txt");
    assert(read_result.ok());
    
    std::string read_data(read_result.value().begin(), read_result.value().end());
    std::cout << "  Read: \"" << read_data << "\"" << std::endl;
    assert(read_data == test_data);
    
    // 检查文件大小
    auto stat = tf.dir->stat("/hello.txt");
    assert(stat.ok());
    assert(stat.value().size == test_data.size());
    std::cout << "  File size: " << stat.value().size << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testFileOffset() {
    std::cout << "=== Test: File Offset Read/Write ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    tf.dir->createFile("/offset.txt");
    
    // 写入初始数据
    std::string initial = "AAAAAAAAAA";  // 10 A's
    tf.dir->writeFile("/offset.txt", initial);
    
    // 在偏移位置写入
    std::string insert = "BBB";
    std::vector<uint8_t> insert_bytes(insert.begin(), insert.end());
    auto write_result = tf.dir->writeFile("/offset.txt", insert_bytes, 3);
    assert(write_result.ok());
    std::cout << "  Wrote 'BBB' at offset 3" << std::endl;
    
    // 读取验证
    auto read_result = tf.dir->readFile("/offset.txt");
    assert(read_result.ok());
    
    std::string content(read_result.value().begin(), read_result.value().end());
    std::cout << "  Full content: \"" << content << "\"" << std::endl;
    assert(content == "AAABBBAAAA");
    
    // 部分读取
    read_result = tf.dir->readFile("/offset.txt", 3, 3);
    assert(read_result.ok());
    std::string partial(read_result.value().begin(), read_result.value().end());
    std::cout << "  Partial read (offset=3, len=3): \"" << partial << "\"" << std::endl;
    assert(partial == "BBB");
    
    // 读取超出范围
    read_result = tf.dir->readFile("/offset.txt", 100, 10);
    assert(read_result.ok());
    assert(read_result.value().empty());  // 超出范围返回空
    std::cout << "  Read beyond EOF returns empty" << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testLargeFile() {
    std::cout << "=== Test: Large File (Multi-block) ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    tf.dir->createFile("/large.bin");
    
    // 写入多个块的数据 (3.5 blocks)
    size_t data_size = BLOCK_SIZE * 3 + 512;
    std::vector<uint8_t> data(data_size);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    
    std::cout << "  Writing " << data_size << " bytes..." << std::endl;
    auto write_result = tf.dir->writeFile("/large.bin", data);
    assert(write_result.ok());
    assert(write_result.value() == data_size);
    std::cout << "  Wrote " << write_result.value() << " bytes" << std::endl;
    
    // 读取验证
    auto read_result = tf.dir->readFile("/large.bin");
    assert(read_result.ok());
    assert(read_result.value().size() == data.size());
    assert(read_result.value() == data);
    std::cout << "  Read and verified " << read_result.value().size() << " bytes" << std::endl;
    
    // 检查块计数
    auto stat = tf.dir->stat("/large.bin");
    assert(stat.ok());
    std::cout << "  File size: " << stat.value().size 
              << ", blocks: " << stat.value().blocks << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testAppend() {
    std::cout << "=== Test: Append ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    tf.dir->createFile("/append.txt");
    
    // 初始写入
    std::vector<uint8_t> part1 = {'H', 'e', 'l', 'l', 'o'};
    tf.dir->writeFile("/append.txt", part1);
    std::cout << "  Initial write: 'Hello'" << std::endl;
    
    // 追加
    std::vector<uint8_t> part2 = {' ', 'W', 'o', 'r', 'l', 'd', '!'};
    auto append_result = tf.dir->appendFile("/append.txt", part2);
    assert(append_result.ok());
    std::cout << "  Appended: ' World!'" << std::endl;
    
    // 读取验证
    auto read_result = tf.dir->readFile("/append.txt");
    assert(read_result.ok());
    std::string content(read_result.value().begin(), read_result.value().end());
    std::cout << "  Final content: \"" << content << "\"" << std::endl;
    assert(content == "Hello World!");
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testTruncate() {
    std::cout << "=== Test: Truncate ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    tf.dir->createFile("/truncate.txt");
    tf.dir->writeFile("/truncate.txt", std::string("Hello World!"));
    
    auto stat = tf.dir->stat("/truncate.txt");
    std::cout << "  Initial size: " << stat.value().size << std::endl;
    
    // 截断
    ErrorCode err = tf.dir->truncate("/truncate.txt", 5);
    assert(err == ErrorCode::OK);
    
    stat = tf.dir->stat("/truncate.txt");
    assert(stat.value().size == 5);
    std::cout << "  After truncate(5): " << stat.value().size << std::endl;
    
    // 读取验证
    auto read = tf.dir->readFile("/truncate.txt");
    std::string content(read.value().begin(), read.value().end());
    std::cout << "  Content: \"" << content << "\"" << std::endl;
    assert(content == "Hello");
    
    // 扩展
    err = tf.dir->truncate("/truncate.txt", 10);
    assert(err == ErrorCode::OK);
    stat = tf.dir->stat("/truncate.txt");
    assert(stat.value().size == 10);
    std::cout << "  After truncate(10): " << stat.value().size << std::endl;
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testPersistence() {
    std::cout << "=== Test: Persistence ===" << std::endl;
    
    std::string saved_content = "This data should persist!";
    
    // Phase 1: Create and write
    {
        TestFixture tf;
        assert(tf.setup());
        
        tf.dir->mkdir("/persist");
        tf.dir->createFile("/persist/data.txt");
        tf.dir->writeFile("/persist/data.txt", saved_content);
        
        tf.dir->sync();
        std::cout << "  Phase 1: Created and wrote data" << std::endl;
    }
    
    // Phase 2: Reopen and verify
    {
        DiskImage disk;
        assert(disk.open(TEST_DISK) == ErrorCode::OK);
        
        Allocator alloc(&disk);
        assert(alloc.load() == ErrorCode::OK);
        
        Directory dir(&alloc, &disk);
        
        // Verify structure
        assert(dir.exists("/persist"));
        assert(dir.exists("/persist/data.txt"));
        
        // Verify content
        auto read = dir.readFile("/persist/data.txt");
        assert(read.ok());
        std::string content(read.value().begin(), read.value().end());
        assert(content == saved_content);
        
        std::cout << "  Phase 2: Verified persistence" << std::endl;
        std::cout << "  Content: \"" << content << "\"" << std::endl;
    }
    
    std::cout << "PASSED" << std::endl << std::endl;
}

void testComplexStructure() {
    std::cout << "=== Test: Complex Directory Structure ===" << std::endl;
    
    TestFixture tf;
    assert(tf.setup());
    
    // 创建模拟审稿系统的目录结构
    tf.dir->mkdir("/papers");
    tf.dir->mkdir("/papers/paper001");
    tf.dir->mkdir("/papers/paper001/versions");
    tf.dir->mkdir("/papers/paper001/reviews");
    tf.dir->mkdir("/users");
    tf.dir->mkdir("/config");
    
    // 创建一些文件
    tf.dir->createFile("/papers/paper001/metadata.json");
    tf.dir->writeFile("/papers/paper001/metadata.json", 
                      R"({"title": "Test Paper", "author": "John Doe"})");
    
    tf.dir->createFile("/papers/paper001/versions/v1.pdf");
    tf.dir->writeFile("/papers/paper001/versions/v1.pdf", 
                      std::string(1000, 'X'));  // Simulated PDF content
    
    tf.dir->createFile("/papers/paper001/reviews/review1.txt");
    tf.dir->writeFile("/papers/paper001/reviews/review1.txt",
                      "This is a great paper! Accept.");
    
    tf.dir->createFile("/config/settings.ini");
    tf.dir->writeFile("/config/settings.ini", "debug=true\nport=8080");
    
    std::cout << "  Created complex directory structure" << std::endl;
    
    // 遍历并打印结构
    std::function<void(const std::string&, int)> printTree;
    printTree = [&](const std::string& path, int depth) {
        auto list = tf.dir->list(path);
        if (!list.ok()) return;
        
        for (const auto& entry : list.value()) {
            if (entry.getName() == "." || entry.getName() == "..") continue;
            
            std::string indent(depth * 2, ' ');
            std::string full_path = (path == "/" ? "" : path) + "/" + entry.getName();
            
            if (entry.file_type == FileType::DIRECTORY) {
                std::cout << indent << "[DIR] " << entry.getName() << "/" << std::endl;
                printTree(full_path, depth + 1);
            } else {
                auto st = tf.dir->stat(full_path);
                uint32_t size = st.ok() ? st.value().size : 0;
                std::cout << indent << "[FILE] " << entry.getName() 
                          << " (" << size << " bytes)" << std::endl;
            }
        }
    };
    
    std::cout << "\n  Directory tree:" << std::endl;
    std::cout << "  /" << std::endl;
    printTree("/", 2);
    
    std::cout << "\nPASSED" << std::endl << std::endl;
}

void cleanup() {
    std::remove(TEST_DISK);
    std::cout << "=== Cleanup: Removed test disk ===" << std::endl;
}

//==============================================================================
// 主函数
//==============================================================================

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "       Directory Module Tests" << std::endl;
    std::cout << "==========================================" << std::endl << std::endl;
    
    try {
        testPathOperations();
        testDirectoryListing();
        testMkdir();
        testRmdir();
        testCreateFile();
        testRemoveFile();
        testFileReadWrite();
        testFileOffset();
        testLargeFile();
        testAppend();
        testTruncate();
        testPersistence();
        testComplexStructure();
        
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