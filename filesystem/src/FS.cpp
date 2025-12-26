// filesystem/src/FS.cpp
#include "FS.h"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <ctime>

namespace fs {

//==============================================================================
// 构造与析构
//==============================================================================

FileSystem::FileSystem()
    : mounted_(false)
{
}

FileSystem::~FileSystem() {
    if (mounted_) {
        unmount();
    }
}

//==============================================================================
// 文件系统生命周期管理
//==============================================================================

ErrorCode FileSystem::format(const std::string& path,
                              uint32_t total_blocks,
                              uint32_t total_inodes) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 如果已挂载，先卸载
    if (mounted_) {
        ErrorCode err = unmount();
        if (err != ErrorCode::OK) {
            return err;
        }
    }

    MkfsOptions opts;
    opts.total_blocks = total_blocks;
    opts.total_inodes = total_inodes;
    opts.force = true;
    opts.verbose = false;

    MkfsResult result = mkfs(path, opts);
    return result.error;
}

ErrorCode FileSystem::mount(const std::string& path, const FSConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (mounted_) {
        return ErrorCode::E_ALREADY_EXISTS;
    }

    config_ = config;

    // 创建磁盘镜像
    disk_ = std::make_unique<DiskImage>();
    ErrorCode err = disk_->open(path);
    if (err != ErrorCode::OK) {
        disk_.reset();
        return err;
    }

    // 创建缓存层
    if (config.enable_cache) {
        cached_disk_ = std::make_unique<CachedDisk>(disk_.get(), config.cache_capacity);
        cached_disk_->setWriteThrough(config.write_through);
    }

    // 创建分配器
    if (config.enable_cache) {
        alloc_ = std::make_unique<Allocator>(cached_disk_.get());
    } else {
        alloc_ = std::make_unique<Allocator>(disk_.get());
    }

    err = alloc_->load();
    if (err != ErrorCode::OK) {
        alloc_.reset();
        cached_disk_.reset();
        disk_->close();
        disk_.reset();
        return err;
    }

    // 创建目录管理器
    if (config.enable_cache) {
        dir_ = std::make_unique<Directory>(alloc_.get(), cached_disk_.get());
    } else {
        dir_ = std::make_unique<Directory>(alloc_.get(), disk_.get());
    }

    // 创建快照管理器
    if (config.enable_cache) {
        snap_ = std::make_unique<SnapshotManager>(alloc_.get(), dir_.get(), cached_disk_.get());
    } else {
        snap_ = std::make_unique<SnapshotManager>(alloc_.get(), dir_.get(), disk_.get());
    }

    err = snap_->load();
    if (err != ErrorCode::OK) {
        snap_.reset();
        dir_.reset();
        alloc_.reset();
        cached_disk_.reset();
        disk_->close();
        disk_.reset();
        return err;
    }
    if (dir_) {
        dir_->setSnapshotManager(snap_.get());
    }

    mounted_ = true;
    mount_path_ = path;

    return ErrorCode::OK;
}

ErrorCode FileSystem::unmount() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!mounted_) {
        return ErrorCode::OK;
    }

    unmounting_ = true;
    lock.unlock();
    {
        std::unique_lock<std::mutex> op_lock(op_mu_);
        op_cv_.wait(op_lock, [this] { return active_ops_.load() == 0; });
    }
    lock.lock();

    // 同步所有数据
    ErrorCode err = ErrorCode::OK;

    if (snap_) {
        ErrorCode snap_err = snap_->sync();
        if (snap_err != ErrorCode::OK && err == ErrorCode::OK) err = snap_err;
    }

    if (dir_) {
        ErrorCode dir_err = dir_->sync();
        if (dir_err != ErrorCode::OK && err == ErrorCode::OK) err = dir_err;
    }

    if (alloc_) {
        ErrorCode alloc_err = alloc_->sync();
        if (alloc_err != ErrorCode::OK && err == ErrorCode::OK) err = alloc_err;
    }

    if (cached_disk_) {
        ErrorCode flush_err = cached_disk_->flush();
        if (flush_err != ErrorCode::OK && err == ErrorCode::OK) err = flush_err;
    }

    if (disk_) {
        ErrorCode disk_err = disk_->sync();
        if (disk_err != ErrorCode::OK && err == ErrorCode::OK) err = disk_err;
    }

    // 释放资源（顺序很重要）
    snap_.reset();
    dir_.reset();
    alloc_.reset();
    cached_disk_.reset();
    
    if (disk_) {
        disk_->close();
        disk_.reset();
    }

    mounted_ = false;
    mount_path_.clear();
    unmounting_ = false;

    return err;
}

ErrorCode FileSystem::sync() {
    std::unique_lock<std::mutex> lock(mutex_);

    if (!mounted_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    ErrorCode err = ErrorCode::OK;
    if (snap_) {
        ErrorCode snap_err = snap_->sync();
        if (snap_err != ErrorCode::OK && err == ErrorCode::OK) err = snap_err;
    }
    if (dir_) {
        ErrorCode dir_err = dir_->sync();
        if (dir_err != ErrorCode::OK && err == ErrorCode::OK) err = dir_err;
    }
    if (alloc_) {
        ErrorCode alloc_err = alloc_->sync();
        if (alloc_err != ErrorCode::OK && err == ErrorCode::OK) err = alloc_err;
    }
    if (cached_disk_) {
        ErrorCode flush_err = cached_disk_->flush();
        if (flush_err != ErrorCode::OK) err = flush_err;
    }
    if (disk_) {
        ErrorCode disk_err = disk_->sync();
        if (disk_err != ErrorCode::OK && err == ErrorCode::OK) err = disk_err;
    }

    return err;
}

FSInfo FileSystem::getInfo() const {
    std::unique_lock<std::mutex> lock(mutex_);

    FSInfo info = {};
    info.mounted = mounted_;
    info.mount_path = mount_path_;

    if (!mounted_ || !alloc_) {
        return info;
    }

    const Superblock& sb = alloc_->getSuperblock();

    info.block_size = sb.block_size;
    info.total_blocks = sb.total_blocks;
    info.total_inodes = sb.total_inodes;
    info.free_blocks = sb.free_blocks;
    info.used_blocks = sb.used_blocks;
    info.free_inodes = sb.free_inodes;
    info.used_inodes = sb.used_inodes;

    info.total_size = static_cast<uint64_t>(sb.data_block_count) * BLOCK_SIZE;
    info.free_size = static_cast<uint64_t>(sb.free_blocks) * BLOCK_SIZE;
    info.used_size = static_cast<uint64_t>(sb.used_blocks) * BLOCK_SIZE;

    if (snap_) {
        info.snapshot_count = snap_->getSnapshotCount();
        info.max_snapshots = snap_->getMaxSnapshots();
    }

    if (cached_disk_) {
        info.cache_stats = cached_disk_->getCacheStats();
    }

    return info;
}

//==============================================================================
// 目录操作
//==============================================================================

ErrorCode FileSystem::mkdir(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    auto result = dir_->mkdir(normalizePath(path));
    return result.ok() ? ErrorCode::OK : result.error();
}

ErrorCode FileSystem::rmdir(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return dir_->rmdir(normalizePath(path));
}

Result<std::vector<DirEntry>> FileSystem::readdir(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) {
        return Result<std::vector<DirEntry>>::failure(err);
    }

    return dir_->list(normalizePath(path));
}

ErrorCode FileSystem::mkdirp(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    std::string normalized = normalizePath(path);
    if (normalized == "/") {
        return ErrorCode::OK;
    }

    auto components = splitPath(normalized);
    std::string current = "";

    for (const auto& comp : components) {
        current += "/" + comp;
        
        if (!dir_->exists(current)) {
            auto result = dir_->mkdir(current);
            if (!result.ok()) {
                return result.error();
            }
        } else if (!dir_->isDirectory(current)) {
            return ErrorCode::E_NOT_DIR;
        }
    }

    return ErrorCode::OK;
}

//==============================================================================
// 文件操作
//==============================================================================

ErrorCode FileSystem::create(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    auto result = dir_->createFile(normalizePath(path));
    return result.ok() ? ErrorCode::OK : result.error();
}

ErrorCode FileSystem::unlink(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return dir_->removeFile(normalizePath(path));
}

ErrorCode FileSystem::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return dir_->remove(normalizePath(path));
}

Result<std::vector<uint8_t>> FileSystem::readFile(const std::string& path,
                                                   uint32_t offset,
                                                   uint32_t length) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) {
        return Result<std::vector<uint8_t>>::failure(err);
    }

    return dir_->readFile(normalizePath(path), offset, length);
}

Result<std::string> FileSystem::readFileAsString(const std::string& path) {
    auto result = readFile(path);
    if (!result.ok()) {
        return Result<std::string>::failure(result.error());
    }

    std::string content(result.value().begin(), result.value().end());
    return Result<std::string>::success(std::move(content));
}

Result<uint32_t> FileSystem::writeFile(const std::string& path,
                                        const std::vector<uint8_t>& data,
                                        uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) {
        return Result<uint32_t>::failure(err);
    }

    return dir_->writeFile(normalizePath(path), data, offset);
}

Result<uint32_t> FileSystem::writeFile(const std::string& path,
                                        const std::string& content,
                                        uint32_t offset) {
    std::vector<uint8_t> data(content.begin(), content.end());
    return writeFile(path, data, offset);
}

Result<uint32_t> FileSystem::appendFile(const std::string& path,
                                         const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) {
        return Result<uint32_t>::failure(err);
    }

    return dir_->appendFile(normalizePath(path), data);
}

Result<uint32_t> FileSystem::appendFile(const std::string& path,
                                         const std::string& content) {
    std::vector<uint8_t> data(content.begin(), content.end());
    return appendFile(path, data);
}

ErrorCode FileSystem::truncate(const std::string& path, uint32_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return dir_->truncate(normalizePath(path), size);
}

ErrorCode FileSystem::copyFile(const std::string& src, const std::string& dst) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    std::string src_path = normalizePath(src);
    std::string dst_path = normalizePath(dst);

    // 读取源文件
    auto read_result = dir_->readFile(src_path);
    if (!read_result.ok()) {
        return read_result.error();
    }

    // 如果目标不存在，创建它
    if (!dir_->exists(dst_path)) {
        auto create_result = dir_->createFile(dst_path);
        if (!create_result.ok()) {
            return create_result.error();
        }
    }

    // 写入目标文件
    auto write_result = dir_->writeFile(dst_path, read_result.value());
    return write_result.ok() ? ErrorCode::OK : write_result.error();
}

ErrorCode FileSystem::moveFile(const std::string& src, const std::string& dst) {
    // 简单实现：复制后删除
    ErrorCode err = copyFile(src, dst);
    if (err != ErrorCode::OK) {
        return err;
    }

    return unlink(src);
}

//==============================================================================
// 文件/目录信息
//==============================================================================

Result<FileStat> FileSystem::stat(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) {
        return Result<FileStat>::failure(err);
    }

    return dir_->stat(normalizePath(path));
}

bool FileSystem::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_) return false;
    return dir_->exists(normalizePath(path));
}

bool FileSystem::isDirectory(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_) return false;
    return dir_->isDirectory(normalizePath(path));
}

bool FileSystem::isFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_) return false;
    return dir_->isFile(normalizePath(path));
}

Result<uint32_t> FileSystem::getFileSize(const std::string& path) {
    auto result = stat(path);
    if (!result.ok()) {
        return Result<uint32_t>::failure(result.error());
    }
    return Result<uint32_t>::success(result.value().size);
}

//==============================================================================
// 快照操作
//==============================================================================

ErrorCode FileSystem::createSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    // 先同步所有数据
    if (dir_) dir_->sync();
    if (alloc_) alloc_->sync();
    if (cached_disk_) cached_disk_->flush();

    return snap_->createSnapshot(name);
}

ErrorCode FileSystem::restoreSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    err = snap_->restoreSnapshot(name);
    if (err != ErrorCode::OK) {
        return err;
    }

    // 重新加载分配器状态
    return alloc_->reload();
}

ErrorCode FileSystem::deleteSnapshot(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return snap_->deleteSnapshot(name);
}

std::vector<SnapshotInfo> FileSystem::listSnapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_ || !snap_) {
        return {};
    }

    return snap_->listSnapshots();
}

bool FileSystem::snapshotExists(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_ || !snap_) return false;
    return snap_->snapshotExists(name);
}

//==============================================================================
// 缓存控制
//==============================================================================

CacheStats FileSystem::getCacheStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_ || !cached_disk_) {
        return CacheStats{0, 0, 0, 0, 0, 0.0};
    }

    return cached_disk_->getCacheStats();
}

void FileSystem::resetCacheStats() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cached_disk_) {
        cached_disk_->resetCacheStats();
    }
}

ErrorCode FileSystem::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_) {
        return ErrorCode::E_INVALID_PARAM;
    }

    if (cached_disk_) {
        return cached_disk_->clearCache();
    }

    return ErrorCode::OK;
}

void FileSystem::setCacheCapacity(uint32_t capacity) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cached_disk_) {
        cached_disk_->setCacheCapacity(capacity);
    }
}

void FileSystem::setCacheEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (cached_disk_) {
        cached_disk_->setCacheEnabled(enabled);
    }
}

//==============================================================================
// 实用工具
//==============================================================================

ErrorCode FileSystem::walk(const std::string& path,
                           std::function<bool(const std::string&, const FileStat&)> callback) {
    std::unique_lock<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;
    if (unmounting_) return ErrorCode::E_INVALID_PARAM;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    auto op_guard = std::unique_ptr<void, std::function<void(void*)>>(
        this, [this](void*) { endActiveOp(); });

    std::string normalized = normalizePath(path);

    // 获取起始路径的 stat
    auto stat_result = dir_->stat(normalized);
    if (!stat_result.ok()) {
        return stat_result.error();
    }

    FileStat st = stat_result.value();
    lock.unlock();

    // 调用回调（不持锁，避免回调内再调用 FS API 死锁）
    if (!callback(normalized, st)) {
        return ErrorCode::OK;
    }

    // 如果是目录，递归遍历
    if (st.type == FileType::DIRECTORY) {
        lock.lock();
        auto list_result = dir_->list(normalized);
        if (!list_result.ok()) {
            return list_result.error();
        }

        std::vector<std::string> children;
        children.reserve(list_result.value().size());
        for (const auto& entry : list_result.value()) {
            std::string name = entry.getName();
            if (name == "." || name == "..") continue;

            std::string child_path = (normalized == "/") 
                ? "/" + name 
                : normalized + "/" + name;

            children.push_back(std::move(child_path));
        }
        lock.unlock();

        for (const auto& child_path : children) {
            err = walk(child_path, callback);
            if (err != ErrorCode::OK) {
                return err;
            }
        }
    }

    return ErrorCode::OK;
}

ErrorCode FileSystem::removeRecursive(const std::string& path) {
    std::unique_lock<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;
    if (unmounting_) return ErrorCode::E_INVALID_PARAM;
    active_ops_.fetch_add(1, std::memory_order_acq_rel);
    auto op_guard = std::unique_ptr<void, std::function<void(void*)>>(
        this, [this](void*) { endActiveOp(); });

    std::string normalized = normalizePath(path);

    if (normalized == "/") {
        return ErrorCode::E_PERMISSION;
    }

    // 检查是否是目录
    if (!dir_->isDirectory(normalized)) {
        return dir_->removeFile(normalized);
    }

    // 递归删除目录内容
    auto list_result = dir_->list(normalized);
    if (!list_result.ok()) {
        return list_result.error();
    }

    for (const auto& entry : list_result.value()) {
        std::string name = entry.getName();
        if (name == "." || name == "..") continue;

        std::string child_path = normalized + "/" + name;

        // 递归删除
        lock.unlock();
        err = removeRecursive(child_path);
        lock.lock();

        if (err != ErrorCode::OK) {
            return err;
        }
    }

    // 删除空目录
    return dir_->rmdir(normalized);
}

void FileSystem::endActiveOp() {
    int left = active_ops_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        op_cv_.notify_all();
    }
}

Result<uint64_t> FileSystem::getDirSize(const std::string& path) {
    uint64_t total_size = 0;

    ErrorCode err = walk(path, [&total_size](const std::string&, const FileStat& st) {
        if (st.type == FileType::REGULAR) {
            total_size += st.size;
        }
        return true;
    });

    if (err != ErrorCode::OK) {
        return Result<uint64_t>::failure(err);
    }

    return Result<uint64_t>::success(total_size);
}

ErrorCode FileSystem::checkConsistency(bool fix) {
    std::lock_guard<std::mutex> lock(mutex_);

    ErrorCode err = ensureMounted();
    if (err != ErrorCode::OK) return err;

    return alloc_->checkConsistency(fix);
}

void FileSystem::printTree(const std::string& path, int max_depth) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!mounted_) {
        std::cout << "(not mounted)" << std::endl;
        return;
    }

    std::string normalized = normalizePath(path);
    std::cout << normalized << std::endl;
    printTreeRecursive(normalized, "", 0, max_depth);
}

void FileSystem::printTreeRecursive(const std::string& path, 
                                     const std::string& prefix,
                                     int depth, int max_depth) {
    if (max_depth > 0 && depth >= max_depth) {
        return;
    }

    auto list_result = dir_->list(path);
    if (!list_result.ok()) {
        return;
    }

    // 过滤 . 和 ..
    std::vector<DirEntry> entries;
    for (const auto& e : list_result.value()) {
        std::string name = e.getName();
        if (name != "." && name != "..") {
            entries.push_back(e);
        }
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        bool is_last = (i == entries.size() - 1);
        const auto& entry = entries[i];
        std::string name = entry.getName();

        std::string connector = is_last ? "└── " : "├── ";
        std::string type_indicator = (entry.file_type == FileType::DIRECTORY) ? "/" : "";

        std::cout << prefix << connector << name << type_indicator;

        // 显示文件大小
        if (entry.file_type == FileType::REGULAR) {
            std::string child_path = (path == "/") ? "/" + name : path + "/" + name;
            auto stat = dir_->stat(child_path);
            if (stat.ok()) {
                std::cout << " (" << stat.value().size << " bytes)";
            }
        }

        std::cout << std::endl;

        // 递归目录
        if (entry.file_type == FileType::DIRECTORY) {
            std::string child_path = (path == "/") ? "/" + name : path + "/" + name;
            std::string new_prefix = prefix + (is_last ? "    " : "│   ");
            printTreeRecursive(child_path, new_prefix, depth + 1, max_depth);
        }
    }
}

//==============================================================================
// 内部辅助方法
//==============================================================================

ErrorCode FileSystem::ensureMounted() {
    if (!mounted_) {
        return ErrorCode::E_INVALID_PARAM;
    }
    return ErrorCode::OK;
}

std::string FileSystem::normalizePath(const std::string& path) {
    if (path.empty()) return "/";

    std::string result = path;

    // 确保以 / 开头
    if (result[0] != '/') {
        result = "/" + result;
    }

    // 移除末尾的 /
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }

    // 合并连续的 /
    std::string cleaned;
    bool last_was_slash = false;
    for (char c : result) {
        if (c == '/') {
            if (!last_was_slash) {
                cleaned += c;
                last_was_slash = true;
            }
        } else {
            cleaned += c;
            last_was_slash = false;
        }
    }

    return cleaned;
}

std::vector<std::string> FileSystem::splitPath(const std::string& path) {
    std::vector<std::string> components;
    std::string normalized = normalizePath(path);

    if (normalized == "/") {
        return components;
    }

    std::istringstream iss(normalized);
    std::string token;

    while (std::getline(iss, token, '/')) {
        if (!token.empty()) {
            components.push_back(token);
        }
    }

    return components;
}

std::string FileSystem::getParentPath(const std::string& path) {
    std::string normalized = normalizePath(path);
    
    if (normalized == "/") {
        return "/";
    }

    auto pos = normalized.rfind('/');
    if (pos == 0) {
        return "/";
    }

    return normalized.substr(0, pos);
}

std::string FileSystem::getBaseName(const std::string& path) {
    std::string normalized = normalizePath(path);

    if (normalized == "/") {
        return "/";
    }

    auto pos = normalized.rfind('/');
    return normalized.substr(pos + 1);
}

} // namespace fs
