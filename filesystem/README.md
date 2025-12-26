# FileSystem API 文档

```markdown
# ReviewSystem FileSystem API 文档

## 概述

ReviewSystem FileSystem 是一个简化的类 POSIX 文件系统，专为科研审稿系统设计。
提供完整的文件/目录操作、快照备份、LRU 缓存等功能。

## 快速开始

```cpp
#include "FS.h"
using namespace fs;

int main() {
    FileSystem fs;
    
    // 格式化并挂载
    fs.format("disk.img", 16384, 1024);  // 16MB, 1024 inodes
    fs.mount("disk.img");
    
    // 文件操作
    fs.mkdir("/data");
    fs.create("/data/test.txt");
    fs.writeFile("/data/test.txt", "Hello World");
    
    auto content = fs.readFileAsString("/data/test.txt");
    std::cout << content.value() << std::endl;
    
    fs.unmount();
    return 0;
}
```

---

## 目录

1. [文件系统生命周期](#1-文件系统生命周期)
2. [目录操作](#2-目录操作)
3. [文件操作](#3-文件操作)
4. [文件信息查询](#4-文件信息查询)
5. [快照管理](#5-快照管理)
6. [缓存控制](#6-缓存控制)
7. [实用工具](#7-实用工具)
8. [数据结构](#8-数据结构)
9. [错误码](#9-错误码)
10. [完整示例](#10-完整示例)

---

## 1. 文件系统生命周期

### format - 格式化文件系统

```cpp
ErrorCode format(const std::string& path, 
                 uint32_t total_blocks = 16384,
                 uint32_t total_inodes = 1024);
```

**参数：**
- `path` - 磁盘镜像文件路径
- `total_blocks` - 总块数（每块 1KB），默认 16384（16MB）
- `total_inodes` - 总 inode 数，默认 1024

**返回：** `ErrorCode::OK` 成功

**示例：**
```cpp
FileSystem fs;

// 创建 16MB 文件系统
fs.format("disk.img");

// 创建 64MB 文件系统，最多 4096 个文件
fs.format("large_disk.img", 65536, 4096);

// 创建小型文件系统（1MB）
fs.format("small.img", 1024, 128);
```

---

### mount - 挂载文件系统

```cpp
ErrorCode mount(const std::string& path, const FSConfig& config = FSConfig());
```

**参数：**
- `path` - 磁盘镜像文件路径
- `config` - 配置选项（可选）

**FSConfig 结构：**
```cpp
struct FSConfig {
    uint32_t cache_capacity = 64;    // 缓存块数
    bool enable_cache = true;         // 启用缓存
    bool write_through = false;       // 写穿透模式
    bool auto_sync = true;            // 自动同步
};
```

**示例：**
```cpp
FileSystem fs;

// 默认配置挂载
fs.mount("disk.img");

// 自定义配置
FSConfig config;
config.cache_capacity = 128;      // 128KB 缓存
config.write_through = true;      // 立即写入磁盘
fs.mount("disk.img", config);

// 禁用缓存
FSConfig no_cache;
no_cache.enable_cache = false;
fs.mount("disk.img", no_cache);
```

---

### unmount - 卸载文件系统

```cpp
ErrorCode unmount();
```

**说明：** 同步所有数据并释放资源。

**示例：**
```cpp
fs.unmount();
```

---

### isMounted - 检查挂载状态

```cpp
bool isMounted() const;
```

**示例：**
```cpp
if (fs.isMounted()) {
    std::cout << "文件系统已挂载" << std::endl;
}
```

---

### sync - 同步数据

```cpp
ErrorCode sync();
```

**说明：** 将所有缓存数据写入磁盘。

**示例：**
```cpp
// 定期同步
fs.sync();
```

---

### getInfo - 获取文件系统信息

```cpp
FSInfo getInfo() const;
```

**FSInfo 结构：**
```cpp
struct FSInfo {
    uint32_t block_size;         // 块大小（字节）
    uint32_t total_blocks;       // 总块数
    uint32_t total_inodes;       // 总 inode 数
    uint32_t free_blocks;        // 空闲块数
    uint32_t used_blocks;        // 已用块数
    uint32_t free_inodes;        // 空闲 inode 数
    uint32_t used_inodes;        // 已用 inode 数
    uint64_t total_size;         // 总容量（字节）
    uint64_t free_size;          // 可用空间（字节）
    uint64_t used_size;          // 已用空间（字节）
    uint32_t snapshot_count;     // 快照数量
    uint32_t max_snapshots;      // 最大快照数
    CacheStats cache_stats;      // 缓存统计
    bool mounted;                // 挂载状态
    std::string mount_path;      // 挂载路径
};
```

**示例：**
```cpp
FSInfo info = fs.getInfo();
std::cout << "总容量: " << info.total_size / 1024 << " KB" << std::endl;
std::cout << "可用: " << info.free_size / 1024 << " KB" << std::endl;
std::cout << "已用: " << (info.used_size * 100 / info.total_size) << "%" << std::endl;
std::cout << "文件数: " << info.used_inodes << "/" << info.total_inodes << std::endl;
```

---

## 2. 目录操作

### mkdir - 创建目录

```cpp
ErrorCode mkdir(const std::string& path);
```

**示例：**
```cpp
fs.mkdir("/documents");
fs.mkdir("/documents/papers");

// 错误处理
ErrorCode err = fs.mkdir("/documents");
if (err == ErrorCode::E_ALREADY_EXISTS) {
    std::cout << "目录已存在" << std::endl;
}
```

---

### mkdirp - 递归创建目录

```cpp
ErrorCode mkdirp(const std::string& path);
```

**说明：** 类似 `mkdir -p`，自动创建父目录。

**示例：**
```cpp
// 一次创建多级目录
fs.mkdirp("/papers/2024/conference/submitted");
fs.mkdirp("/users/admin/config");
```

---

### rmdir - 删除空目录

```cpp
ErrorCode rmdir(const std::string& path);
```

**示例：**
```cpp
ErrorCode err = fs.rmdir("/empty_dir");
if (err == ErrorCode::E_NOT_EMPTY) {
    std::cout << "目录非空，无法删除" << std::endl;
}
```

---

### readdir - 读取目录内容

```cpp
Result<std::vector<DirEntry>> readdir(const std::string& path);
```

**DirEntry 结构：**
```cpp
struct DirEntry {
    InodeId inode;           // inode 编号
    uint8_t name_len;        // 文件名长度
    FileType file_type;      // 文件类型
    char name[56];           // 文件名
    
    std::string getName() const;
    bool isValid() const;
};
```

**示例：**
```cpp
auto result = fs.readdir("/documents");
if (result.ok()) {
    for (const auto& entry : result.value()) {
        std::string type = (entry.file_type == FileType::DIRECTORY) ? "[DIR]" : "[FILE]";
        std::cout << type << " " << entry.getName() << std::endl;
    }
}

// 统计文件数量
auto entries = fs.readdir("/").value();
int file_count = 0;
int dir_count = 0;
for (const auto& e : entries) {
    if (e.getName() == "." || e.getName() == "..") continue;
    if (e.file_type == FileType::DIRECTORY) dir_count++;
    else file_count++;
}
std::cout << "文件: " << file_count << ", 目录: " << dir_count << std::endl;
```

---

## 3. 文件操作

### create - 创建空文件

```cpp
ErrorCode create(const std::string& path);
```

**示例：**
```cpp
fs.create("/documents/readme.txt");
fs.create("/config/settings.ini");
```

---

### unlink - 删除文件

```cpp
ErrorCode unlink(const std::string& path);
```

**示例：**
```cpp
fs.unlink("/temp/old_file.txt");
```

---

### remove - 删除文件或空目录

```cpp
ErrorCode remove(const std::string& path);
```

**示例：**
```cpp
// 自动判断类型
fs.remove("/some_file.txt");    // 删除文件
fs.remove("/empty_folder");     // 删除空目录
```

---

### readFile - 读取文件（字节数组）

```cpp
Result<std::vector<uint8_t>> readFile(const std::string& path,
                                       uint32_t offset = 0,
                                       uint32_t length = 0);
```

**参数：**
- `path` - 文件路径
- `offset` - 起始偏移（默认 0）
- `length` - 读取长度（默认 0 = 读取全部）

**示例：**
```cpp
// 读取整个文件
auto result = fs.readFile("/data/binary.bin");
if (result.ok()) {
    std::vector<uint8_t>& data = result.value();
    std::cout << "读取 " << data.size() << " 字节" << std::endl;
}

// 读取部分内容（偏移 100，长度 50）
auto partial = fs.readFile("/data/large.bin", 100, 50);

// 读取文件头
auto header = fs.readFile("/data/file.dat", 0, 256);
```

---

### readFileAsString - 读取文件为字符串

```cpp
Result<std::string> readFileAsString(const std::string& path);
```

**示例：**
```cpp
auto result = fs.readFileAsString("/config/settings.ini");
if (result.ok()) {
    std::cout << result.value() << std::endl;
}

// 解析配置文件
auto content = fs.readFileAsString("/app/config.json");
if (content.ok()) {
    // 解析 JSON...
}
```

---

### writeFile - 写入文件（字节数组）

```cpp
Result<uint32_t> writeFile(const std::string& path,
                            const std::vector<uint8_t>& data,
                            uint32_t offset = 0);
```

**返回：** 实际写入的字节数

**示例：**
```cpp
std::vector<uint8_t> data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};
auto result = fs.writeFile("/data/binary.bin", data);
std::cout << "写入 " << result.value() << " 字节" << std::endl;

// 在偏移位置写入
std::vector<uint8_t> patch = {0xFF, 0xFF};
fs.writeFile("/data/file.bin", patch, 100);  // 从偏移 100 开始写入
```

---

### writeFile - 写入文件（字符串）

```cpp
Result<uint32_t> writeFile(const std::string& path,
                            const std::string& content,
                            uint32_t offset = 0);
```

**示例：**
```cpp
// 写入文本
fs.writeFile("/documents/hello.txt", "Hello, World!");

// 覆盖部分内容
fs.writeFile("/documents/hello.txt", "Hi", 0);  // 结果: "Hillo, World!"

// 写入 JSON
std::string json = R"({
    "name": "test",
    "value": 123
})";
fs.writeFile("/config/data.json", json);
```

---

### appendFile - 追加内容

```cpp
Result<uint32_t> appendFile(const std::string& path,
                             const std::vector<uint8_t>& data);
Result<uint32_t> appendFile(const std::string& path,
                             const std::string& content);
```

**示例：**
```cpp
// 日志追加
fs.appendFile("/logs/app.log", "2024-01-01 10:00:00 INFO Started\n");
fs.appendFile("/logs/app.log", "2024-01-01 10:00:01 INFO Running\n");

// 追加二进制数据
std::vector<uint8_t> record = {0x01, 0x02, 0x03, 0x04};
fs.appendFile("/data/records.bin", record);
```

---

### truncate - 截断文件

```cpp
ErrorCode truncate(const std::string& path, uint32_t size);
```

**示例：**
```cpp
// 清空文件
fs.truncate("/logs/app.log", 0);

// 截断到 100 字节
fs.truncate("/data/file.txt", 100);

// 扩展文件（用零填充）
fs.truncate("/data/sparse.bin", 10000);
```

---

### copyFile - 复制文件

```cpp
ErrorCode copyFile(const std::string& src, const std::string& dst);
```

**示例：**
```cpp
fs.copyFile("/documents/original.txt", "/documents/backup.txt");
fs.copyFile("/papers/paper.pdf", "/archive/paper_v1.pdf");
```

---

### moveFile - 移动/重命名文件

```cpp
ErrorCode moveFile(const std::string& src, const std::string& dst);
```

**示例：**
```cpp
// 重命名
fs.moveFile("/temp/draft.txt", "/temp/final.txt");

// 移动到其他目录
fs.moveFile("/inbox/new.txt", "/processed/done.txt");
```

---

## 4. 文件信息查询

### stat - 获取文件状态

```cpp
Result<FileStat> stat(const std::string& path);
```

**FileStat 结构：**
```cpp
struct FileStat {
    InodeId inode;           // inode 编号
    FileType type;           // 文件类型
    uint32_t size;           // 文件大小（字节）
    uint16_t link_count;     // 链接计数
    int64_t create_time;     // 创建时间（Unix 时间戳）
    int64_t modify_time;     // 修改时间
    int64_t access_time;     // 访问时间
    uint32_t blocks;         // 占用块数
};
```

**示例：**
```cpp
auto result = fs.stat("/documents/report.pdf");
if (result.ok()) {
    FileStat& st = result.value();
    std::cout << "大小: " << st.size << " 字节" << std::endl;
    std::cout << "块数: " << st.blocks << std::endl;
    std::cout << "类型: " << (st.type == FileType::DIRECTORY ? "目录" : "文件") << std::endl;
    
    // 格式化时间
    std::time_t mtime = static_cast<std::time_t>(st.modify_time);
    std::cout << "修改时间: " << std::ctime(&mtime);
}
```

---

### exists - 检查路径是否存在

```cpp
bool exists(const std::string& path);
```

**示例：**
```cpp
if (fs.exists("/config/settings.ini")) {
    // 加载配置
} else {
    // 创建默认配置
    fs.create("/config/settings.ini");
    fs.writeFile("/config/settings.ini", "default=true");
}
```

---

### isDirectory - 检查是否为目录

```cpp
bool isDirectory(const std::string& path);
```

**示例：**
```cpp
if (fs.isDirectory("/papers")) {
    auto files = fs.readdir("/papers");
    // 处理文件列表
}
```

---

### isFile - 检查是否为文件

```cpp
bool isFile(const std::string& path);
```

**示例：**
```cpp
std::string path = "/documents/unknown";
if (fs.isFile(path)) {
    auto content = fs.readFile(path);
} else if (fs.isDirectory(path)) {
    auto entries = fs.readdir(path);
}
```

---

### getFileSize - 获取文件大小

```cpp
Result<uint32_t> getFileSize(const std::string& path);
```

**示例：**
```cpp
auto size = fs.getFileSize("/papers/thesis.pdf");
if (size.ok()) {
    std::cout << "文件大小: " << size.value() << " 字节" << std::endl;
    std::cout << "约 " << size.value() / 1024 << " KB" << std::endl;
}
```

---

## 5. 快照管理

### createSnapshot - 创建快照

```cpp
ErrorCode createSnapshot(const std::string& name);
```

**说明：** 创建文件系统的时间点快照，用于备份和恢复。

**示例：**
```cpp
// 在重要操作前创建快照
fs.createSnapshot("before_upgrade");

// 带时间戳的快照
auto now = std::time(nullptr);
std::string name = "backup_" + std::to_string(now);
fs.createSnapshot(name);

// 每日备份
fs.createSnapshot("daily_2024_01_15");
```

---

### restoreSnapshot - 恢复快照

```cpp
ErrorCode restoreSnapshot(const std::string& name);
```

**说明：** 将文件系统恢复到快照时的状态。

**示例：**
```cpp
// 出错时恢复
ErrorCode err = performRiskyOperation();
if (err != ErrorCode::OK) {
    fs.restoreSnapshot("before_upgrade");
    std::cout << "已回滚到升级前状态" << std::endl;
}
```

---

### deleteSnapshot - 删除快照

```cpp
ErrorCode deleteSnapshot(const std::string& name);
```

**示例：**
```cpp
// 删除旧快照释放空间
fs.deleteSnapshot("old_backup");

// 保留最近 N 个快照
auto snapshots = fs.listSnapshots();
while (snapshots.size() > 5) {
    fs.deleteSnapshot(snapshots[0].name);
    snapshots.erase(snapshots.begin());
}
```

---

### listSnapshots - 列出所有快照

```cpp
std::vector<SnapshotInfo> listSnapshots() const;
```

**SnapshotInfo 结构：**
```cpp
struct SnapshotInfo {
    std::string name;        // 快照名称
    int64_t create_time;     // 创建时间
    InodeId root_inode;      // 根 inode
    uint32_t block_count;    // 占用块数
    bool valid;              // 是否有效
};
```

**示例：**
```cpp
auto snapshots = fs.listSnapshots();
std::cout << "快照列表 (" << snapshots.size() << " 个):" << std::endl;
for (const auto& snap : snapshots) {
    std::time_t t = static_cast<std::time_t>(snap.create_time);
    std::cout << "  " << snap.name << " - " << std::ctime(&t);
}
```

---

### snapshotExists - 检查快照是否存在

```cpp
bool snapshotExists(const std::string& name) const;
```

**示例：**
```cpp
if (!fs.snapshotExists("backup")) {
    fs.createSnapshot("backup");
}
```

---

## 6. 缓存控制

### getCacheStats - 获取缓存统计

```cpp
CacheStats getCacheStats() const;
```

**CacheStats 结构：**
```cpp
struct CacheStats {
    uint64_t hits;           // 命中次数
    uint64_t misses;         // 未命中次数
    uint64_t evictions;      // 淘汰次数
    uint32_t capacity;       // 缓存容量（块数）
    uint32_t current_size;   // 当前缓存块数
    double hit_rate;         // 命中率 (0.0 - 1.0)
};
```

**示例：**
```cpp
auto stats = fs.getCacheStats();
std::cout << "缓存统计:" << std::endl;
std::cout << "  命中: " << stats.hits << std::endl;
std::cout << "  未命中: " << stats.misses << std::endl;
std::cout << "  命中率: " << (stats.hit_rate * 100) << "%" << std::endl;
std::cout << "  使用: " << stats.current_size << "/" << stats.capacity << " 块" << std::endl;
```

---

### resetCacheStats - 重置缓存统计

```cpp
void resetCacheStats();
```

**示例：**
```cpp
fs.resetCacheStats();
// 执行测试操作
performOperations();
// 查看这些操作的缓存效果
auto stats = fs.getCacheStats();
```

---

### clearCache - 清空缓存

```cpp
ErrorCode clearCache();
```

**示例：**
```cpp
// 在快照恢复后清空缓存
fs.restoreSnapshot("backup");
fs.clearCache();
```

---

### setCacheCapacity - 设置缓存容量

```cpp
void setCacheCapacity(uint32_t capacity);
```

**示例：**
```cpp
// 增加缓存以提高性能
fs.setCacheCapacity(256);  // 256KB 缓存

// 减少缓存以节省内存
fs.setCacheCapacity(32);   // 32KB 缓存
```

---

### setCacheEnabled - 启用/禁用缓存

```cpp
void setCacheEnabled(bool enabled);
```

**示例：**
```cpp
// 禁用缓存（调试用）
fs.setCacheEnabled(false);

// 重新启用
fs.setCacheEnabled(true);
```

---

## 7. 实用工具

### walk - 遍历目录树

```cpp
ErrorCode walk(const std::string& path,
               std::function<bool(const std::string&, const FileStat&)> callback);
```

**说明：** 递归遍历目录，对每个文件/目录调用回调函数。回调返回 `false` 停止遍历。

**示例：**
```cpp
// 列出所有文件
fs.walk("/", [](const std::string& path, const FileStat& st) {
    if (st.type == FileType::REGULAR) {
        std::cout << path << " (" << st.size << " bytes)" << std::endl;
    }
    return true;  // 继续遍历
});

// 查找大文件
std::vector<std::string> large_files;
fs.walk("/", [&large_files](const std::string& path, const FileStat& st) {
    if (st.type == FileType::REGULAR && st.size > 10000) {
        large_files.push_back(path);
    }
    return true;
});

// 查找特定文件（找到后停止）
std::string target;
fs.walk("/", [&target](const std::string& path, const FileStat& st) {
    if (path.find("config.ini") != std::string::npos) {
        target = path;
        return false;  // 停止遍历
    }
    return true;
});

// 统计文件类型
std::map<std::string, int> ext_count;
fs.walk("/", [&ext_count](const std::string& path, const FileStat& st) {
    if (st.type == FileType::REGULAR) {
        auto pos = path.rfind('.');
        std::string ext = (pos != std::string::npos) ? path.substr(pos) : "(none)";
        ext_count[ext]++;
    }
    return true;
});
```

---

### removeRecursive - 递归删除

```cpp
ErrorCode removeRecursive(const std::string& path);
```

**说明：** 删除目录及其所有内容（类似 `rm -rf`）。

**示例：**
```cpp
// 删除整个项目目录
fs.removeRecursive("/old_project");

// 清空临时目录
fs.removeRecursive("/temp");
fs.mkdir("/temp");  // 重新创建空目录
```

---

### getDirSize - 获取目录大小

```cpp
Result<uint64_t> getDirSize(const std::string& path);
```

**说明：** 计算目录下所有文件的总大小。

**示例：**
```cpp
auto size = fs.getDirSize("/papers");
if (size.ok()) {
    double mb = size.value() / (1024.0 * 1024.0);
    std::cout << "论文目录占用: " << std::fixed << std::setprecision(2) << mb << " MB" << std::endl;
}
```

---

### checkConsistency - 检查一致性

```cpp
ErrorCode checkConsistency(bool fix = false);
```

**说明：** 检查文件系统一致性，可选择自动修复。

**示例：**
```cpp
// 只检查
ErrorCode err = fs.checkConsistency(false);
if (err != ErrorCode::OK) {
    std::cout << "发现不一致" << std::endl;
    
    // 尝试修复
    err = fs.checkConsistency(true);
    if (err == ErrorCode::OK) {
        std::cout << "已修复" << std::endl;
    }
}
```

---

### printTree - 打印目录树

```cpp
void printTree(const std::string& path = "/", int max_depth = 0);
```

**说明：** 以树形结构打印目录内容（调试用）。

**示例：**
```cpp
// 打印完整目录树
fs.printTree("/");

// 从特定目录开始
fs.printTree("/papers");

// 限制深度
fs.printTree("/", 2);  // 只显示 2 层
```

**输出示例：**
```
/
├── papers/
│   ├── paper001/
│   │   ├── metadata.json (256 bytes)
│   │   ├── versions/
│   │   │   └── v1.pdf (10240 bytes)
│   │   └── reviews/
│   │       └── review1.txt (128 bytes)
│   └── paper002/
│       └── draft.pdf (8192 bytes)
├── users/
│   ├── admin/
│   └── reviewer1/
└── config/
    └── settings.ini (64 bytes)
```

---

## 8. 数据结构

### FileType - 文件类型

```cpp
enum class FileType : uint8_t {
    FREE = 0,        // 空闲（未使用）
    REGULAR = 1,     // 普通文件
    DIRECTORY = 2,   // 目录
    SYMLINK = 3,     // 符号链接（预留）
};
```

---

### Result<T> - 结果封装

```cpp
template<typename T>
class Result {
public:
    bool ok() const;              // 是否成功
    ErrorCode error() const;      // 获取错误码
    T& value();                   // 获取值
    const T& value() const;
};

// 用法
Result<std::string> result = fs.readFileAsString("/test.txt");
if (result.ok()) {
    std::cout << result.value() << std::endl;
} else {
    std::cerr << "错误: " << errorCodeToString(result.error()) << std::endl;
}
```

---

## 9. 错误码

```cpp
enum class ErrorCode : int32_t {
    OK = 0,                      // 成功
    
    // 通用错误
    E_IO = -1,                   // I/O 错误
    E_INTERNAL = -2,             // 内部错误
    E_INVALID_PARAM = -3,        // 参数无效
    
    // 路径/文件错误
    E_NOT_FOUND = -10,           // 文件/目录不存在
    E_ALREADY_EXISTS = -11,      // 文件/目录已存在
    E_NOT_DIR = -12,             // 不是目录
    E_IS_DIR = -13,              // 是目录（期望文件）
    E_NOT_EMPTY = -14,           // 目录非空
    E_INVALID_PATH = -15,        // 路径格式无效
    E_NAME_TOO_LONG = -16,       // 文件名过长
    
    // 资源错误
    E_NO_SPACE = -20,            // 磁盘空间不足
    E_NO_INODE = -21,            // inode 耗尽
    E_FILE_TOO_LARGE = -22,      // 文件过大
    
    // 权限错误
    E_PERMISSION = -30,          // 权限不足
    
    // 快照错误
    E_SNAPSHOT_NOT_FOUND = -40,  // 快照不存在
    E_SNAPSHOT_EXISTS = -41,     // 快照已存在
    E_MAX_SNAPSHOTS = -42,       // 快照数量已达上限
};

// 错误码转字符串
const char* errorCodeToString(ErrorCode code);
```

**错误处理示例：**
```cpp
ErrorCode err = fs.mkdir("/test");
switch (err) {
    case ErrorCode::OK:
        std::cout << "创建成功" << std::endl;
        break;
    case ErrorCode::E_ALREADY_EXISTS:
        std::cout << "目录已存在" << std::endl;
        break;
    case ErrorCode::E_NO_SPACE:
        std::cout << "磁盘空间不足" << std::endl;
        break;
    default:
        std::cout << "错误: " << errorCodeToString(err) << std::endl;
}
```

---

## 10. 完整示例

### 示例 1：科研审稿系统目录结构

```cpp
#include "FS.h"
#include <iostream>
#include <ctime>

using namespace fs;

int main() {
    FileSystem fs;
    
    // 初始化文件系统
    fs.format("review_system.img", 32768, 2048);  // 32MB
    fs.mount("review_system.img");
    
    // 创建目录结构
    fs.mkdirp("/papers");
    fs.mkdirp("/users");
    fs.mkdirp("/reviews");
    fs.mkdirp("/config");
    
    // 创建用户
    fs.mkdirp("/users/admin");
    fs.mkdirp("/users/author1");
    fs.mkdirp("/users/reviewer1");
    fs.mkdirp("/users/reviewer2");
    
    // 创建论文
    std::string paper_id = "paper_001";
    fs.mkdirp("/papers/" + paper_id + "/versions");
    fs.mkdirp("/papers/" + paper_id + "/reviews");
    
    // 保存论文元数据
    std::string metadata = R"({
        "id": "paper_001",
        "title": "A Novel Approach to File Systems",
        "author": "author1",
        "status": "submitted",
        "submit_time": )" + std::to_string(std::time(nullptr)) + R"(
    })";
    fs.writeFile("/papers/" + paper_id + "/metadata.json", metadata);
    
    // 保存论文内容
    fs.writeFile("/papers/" + paper_id + "/versions/v1.txt", 
                 "This is the first version of the paper...");
    
    // 创建备份快照
    fs.createSnapshot("initial_state");
    
    // 添加评审
    fs.writeFile("/papers/" + paper_id + "/reviews/reviewer1.txt",
                 "Score: 8/10\nComments: Good paper, minor revisions needed.");
    fs.writeFile("/papers/" + paper_id + "/reviews/reviewer2.txt",
                 "Score: 7/10\nComments: Interesting approach, needs more experiments.");
    
    // 显示目录结构
    std::cout << "=== 目录结构 ===" << std::endl;
    fs.printTree("/");
    
    // 显示文件系统信息
    std::cout << "\n=== 文件系统信息 ===" << std::endl;
    FSInfo info = fs.getInfo();
    std::cout << "使用空间: " << info.used_size / 1024 << " KB" << std::endl;
    std::cout << "剩余空间: " << info.free_size / 1024 << " KB" << std::endl;
    std::cout << "文件数: " << info.used_inodes << std::endl;
    std::cout << "快照数: " << info.snapshot_count << std::endl;
    
    // 显示缓存统计
    std::cout << "\n=== 缓存统计 ===" << std::endl;
    auto cache = fs.getCacheStats();
    std::cout << "命中率: " << (cache.hit_rate * 100) << "%" << std::endl;
    
    fs.unmount();
    return 0;
}
```

---

### 示例 2：文件备份与恢复

```cpp
#include "FS.h"
#include <iostream>

using namespace fs;

void backupAndRestore() {
    FileSystem fs;
    fs.format("backup_demo.img");
    fs.mount("backup_demo.img");
    
    // 创建原始数据
    fs.mkdir("/important");
    fs.writeFile("/important/data.txt", "Original important data");
    fs.writeFile("/important/config.ini", "setting=value");
    
    std::cout << "=== 原始数据 ===" << std::endl;
    auto content = fs.readFileAsString("/important/data.txt");
    std::cout << content.value() << std::endl;
    
    // 创建备份
    std::cout << "\n创建备份快照..." << std::endl;
    fs.createSnapshot("backup_v1");
    
    // 模拟误操作
    std::cout << "模拟数据损坏..." << std::endl;
    fs.writeFile("/important/data.txt", "CORRUPTED DATA!!!");
    fs.unlink("/important/config.ini");
    
    std::cout << "\n=== 损坏后的数据 ===" << std::endl;
    content = fs.readFileAsString("/important/data.txt");
    std::cout << content.value() << std::endl;
    std::cout << "config.ini 存在: " << (fs.exists("/important/config.ini") ? "是" : "否") << std::endl;
    
    // 恢复备份
    std::cout << "\n恢复备份..." << std::endl;
    fs.restoreSnapshot("backup_v1");
    
    std::cout << "\n=== 恢复后的数据 ===" << std::endl;
    content = fs.readFileAsString("/important/data.txt");
    std::cout << content.value() << std::endl;
    std::cout << "config.ini 存在: " << (fs.exists("/important/config.ini") ? "是" : "否") << std::endl;
    
    fs.unmount();
}
```

---

### 示例 3：批量文件处理

```cpp
#include "FS.h"
#include <iostream>
#include <vector>

using namespace fs;

void batchProcess() {
    FileSystem fs;
    fs.format("batch.img");
    fs.mount("batch.img");
    
    // 创建测试文件
    fs.mkdir("/data");
    for (int i = 0; i < 100; ++i) {
        std::string path = "/data/file_" + std::to_string(i) + ".txt";
        std::string content = "Content of file " + std::to_string(i);
        fs.create(path);
        fs.writeFile(path, content);
    }
    
    // 统计文件
    uint64_t total_size = 0;
    int file_count = 0;
    
    fs.walk("/data", [&](const std::string& path, const FileStat& st) {
        if (st.type == FileType::REGULAR) {
            total_size += st.size;
            file_count++;
        }
        return true;
    });
    
    std::cout << "文件数: " << file_count << std::endl;
    std::cout << "总大小: " << total_size << " 字节" << std::endl;
    
    // 批量处理（添加前缀）
    auto entries = fs.readdir("/data").value();
    for (const auto& entry : entries) {
        if (entry.file_type != FileType::REGULAR) continue;
        
        std::string old_path = "/data/" + entry.getName();
        std::string new_path = "/data/processed_" + entry.getName();
        
        // 读取、处理、写入新文件
        auto content = fs.readFileAsString(old_path);
        if (content.ok()) {
            std::string processed = "[PROCESSED] " + content.value();
            fs.create(new_path);
            fs.writeFile(new_path, processed);
        }
    }
    
    std::cout << "\n处理完成！" << std::endl;
    
    fs.unmount();
}
```

---

## 编译说明

```bash
# 编译库
cd filesystem/build
cmake ..
make

# 链接使用
g++ -std=c++17 my_app.cpp -I../include -L. -lreviewfs -o my_app
```

---

## 限制说明

- 块大小：1024 字节（1KB）
- 最大文件大小：约 64MB（12直接块 + 1级间接 + 2级间接）
- 最大文件名长度：56 字符
- 最大快照数：15
- 路径必须以 `/` 开头（绝对路径）

---

## 线程安全

FileSystem 类内部使用互斥锁保护，可以在多线程环境中安全使用。
但建议对于复杂的事务操作使用外部锁定机制。

---

## 版本信息

- 版本：1.0
- 魔数：0x53465352 ("RSFS")
- 块大小：1024 字节

---