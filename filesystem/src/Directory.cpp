// filesystem/src/Directory.cpp
#include "Directory.h"
#include "Cache.h"
#include "Snapshot.h"
#include <cstring>
#include <algorithm>
#include <ctime>
#include <sstream>


namespace fs {

//==============================================================================
// 构造与析构
//==============================================================================

Directory::Directory(Allocator* alloc, DiskImage* disk)
    : alloc_(alloc)
    , disk_(disk)
    , cached_disk_(nullptr)
    , use_cached_disk_(false)
    , snap_(nullptr)
{
}

Directory::Directory(Allocator* alloc, CachedDisk* cached_disk)
    : alloc_(alloc)
    , disk_(cached_disk ? cached_disk->getDisk() : nullptr)
    , cached_disk_(cached_disk)
    , use_cached_disk_(cached_disk != nullptr)
    , snap_(nullptr)
{
}

Directory::~Directory() {
}

//==============================================================================
// 统一块读写接口
//==============================================================================

ErrorCode Directory::readBlockInternal(BlockNo block_no, void* buffer) {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->readBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->readBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

ErrorCode Directory::writeBlockInternal(BlockNo block_no, const void* buffer) {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->writeBlock(block_no, buffer);
    }
    if (disk_) {
        return disk_->writeBlock(block_no, buffer);
    }
    return ErrorCode::E_IO;
}

//==============================================================================
// 缓存控制接口
//==============================================================================

CacheStats Directory::getCacheStats() const {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->getCacheStats();
    }
    return CacheStats{0, 0, 0, 0, 0, 0.0};
}

ErrorCode Directory::flushCache() {
    if (use_cached_disk_ && cached_disk_) {
        return cached_disk_->flush();
    }
    return ErrorCode::OK;
}

//==============================================================================
// 路径处理辅助方法
//==============================================================================

std::vector<std::string> Directory::splitPath(const std::string& path) {
    std::vector<std::string> components;
    std::string normalized = normalizePath(path);
    
    if (normalized.empty() || normalized == "/") {
        return components;
    }
    
    std::istringstream iss(normalized);
    std::string token;
    
    while (std::getline(iss, token, '/')) {
        if (!token.empty() && token != "." ) {
            if (token == "..") {
                if (!components.empty()) {
                    components.pop_back();
                }
            } else {
                components.push_back(token);
            }
        }
    }
    
    return components;
}

std::string Directory::normalizePath(const std::string& path) {
    if (path.empty()) {
        return "/";
    }
    
    std::string result = path;
    
    if (result[0] != '/') {
        result = "/" + result;
    }
    
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    
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

bool Directory::isValidPath(const std::string& path) {
    if (path.empty()) return false;
    if (path[0] != '/') return false;
    
    auto components = splitPath(path);
    for (const auto& comp : components) {
        if (!isValidFilename(comp)) {
            return false;
        }
    }
    
    return true;
}

bool Directory::isValidFilename(const std::string& name) {
    if (name.empty()) return false;
    if (name.size() > MAX_FILENAME_LEN) return false;
    if (name == "." || name == "..") return false;
    
    for (char c : name) {
        if (c == '/' || c == '\0') {
            return false;
        }
    }
    
    return true;
}

//==============================================================================
// 路径解析接口
//==============================================================================

Result<InodeId> Directory::resolvePath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string normalized = normalizePath(path);
    
    if (normalized == "/") {
        return Result<InodeId>::success(ROOT_INODE);
    }
    
    auto components = splitPath(normalized);
    if (components.empty()) {
        return Result<InodeId>::success(ROOT_INODE);
    }
    
    InodeId current = ROOT_INODE;
    
    for (const auto& name : components) {
        auto inode_result = readInode(current);
        if (!inode_result.ok()) {
            return Result<InodeId>::failure(inode_result.error());
        }
        
        if (!inode_result.value().isDirectory()) {
            return Result<InodeId>::failure(ErrorCode::E_NOT_DIR);
        }
        
        auto entry_result = lookupInternal(current, name);
        if (!entry_result.ok()) {
            return Result<InodeId>::failure(ErrorCode::E_NOT_FOUND);
        }
        
        current = entry_result.value().inode;
    }
    
    return Result<InodeId>::success(current);
}

Result<Directory::ParentInfo> Directory::resolveParent(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string normalized = normalizePath(path);
    
    if (normalized == "/") {
        return Result<ParentInfo>::failure(ErrorCode::E_INVALID_PATH);
    }
    
    auto components = splitPath(normalized);
    if (components.empty()) {
        return Result<ParentInfo>::failure(ErrorCode::E_INVALID_PATH);
    }
    
    ParentInfo info;
    info.filename = components.back();
    components.pop_back();
    
    if (components.empty()) {
        info.parent_inode = ROOT_INODE;
    } else {
        InodeId current = ROOT_INODE;
        
        for (const auto& name : components) {
            auto inode_result = readInode(current);
            if (!inode_result.ok()) {
                return Result<ParentInfo>::failure(inode_result.error());
            }
            
            if (!inode_result.value().isDirectory()) {
                return Result<ParentInfo>::failure(ErrorCode::E_NOT_DIR);
            }
            
            auto entry_result = lookupInternal(current, name);
            if (!entry_result.ok()) {
                return Result<ParentInfo>::failure(ErrorCode::E_NOT_FOUND);
            }
            
            current = entry_result.value().inode;
        }
        
        info.parent_inode = current;
    }
    
    auto parent_inode = readInode(info.parent_inode);
    if (!parent_inode.ok()) {
        return Result<ParentInfo>::failure(parent_inode.error());
    }
    
    if (!parent_inode.value().isDirectory()) {
        return Result<ParentInfo>::failure(ErrorCode::E_NOT_DIR);
    }
    
    return Result<ParentInfo>::success(info);
}

Result<FileStat> Directory::stat(const std::string& path) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return Result<FileStat>::failure(inode_result.error());
    }
    
    return statInode(inode_result.value());
}

Result<FileStat> Directory::statInode(InodeId inode_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto inode_result = readInode(inode_id);
    if (!inode_result.ok()) {
        return Result<FileStat>::failure(inode_result.error());
    }
    
    const Inode& inode = inode_result.value();
    
    FileStat st;
    st.inode = inode_id;
    st.type = inode.type;
    st.size = inode.size;
    st.link_count = inode.link_count;
    st.create_time = inode.create_time;
    st.modify_time = inode.modify_time;
    st.access_time = inode.access_time;
    st.blocks = inode.block_count;
    
    return Result<FileStat>::success(st);
}

//==============================================================================
// 目录操作接口
//==============================================================================

Result<DirEntry> Directory::lookupInternal(InodeId dir_inode, const std::string& name) {
    auto inode_result = readInode(dir_inode);
    if (!inode_result.ok()) {
        return Result<DirEntry>::failure(inode_result.error());
    }
    
    const Inode& dir = inode_result.value();
    if (!dir.isDirectory()) {
        return Result<DirEntry>::failure(ErrorCode::E_NOT_DIR);
    }
    
    uint32_t num_blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (num_blocks == 0) num_blocks = 1;
    
    DirEntry entries[DIRENTRIES_PER_BLOCK];
    
    for (uint32_t bi = 0; bi < num_blocks; ++bi) {
        auto block_result = getFileBlock(dir, bi);
        if (!block_result.ok()) {
            continue;
        }
        
        ErrorCode err = readDirectoryBlock(block_result.value(), entries);
        if (err != ErrorCode::OK) {
            continue;
        }
        
        for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
            if (entries[i].isValid() && entries[i].getName() == name) {
                return Result<DirEntry>::success(entries[i]);
            }
        }
    }
    
    return Result<DirEntry>::failure(ErrorCode::E_NOT_FOUND);
}

Result<DirEntry> Directory::lookup(InodeId dir_inode, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return lookupInternal(dir_inode, name);
}

ErrorCode Directory::addEntry(InodeId dir_inode, const std::string& name,
                              InodeId target_inode, FileType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!isValidFilename(name)) {
        return ErrorCode::E_NAME_TOO_LONG;
    }
    
    auto existing = lookupInternal(dir_inode, name);
    if (existing.ok()) {
        return ErrorCode::E_ALREADY_EXISTS;
    }
    
    auto inode_result = readInode(dir_inode);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    Inode dir = inode_result.value();
    if (!dir.isDirectory()) {
        return ErrorCode::E_NOT_DIR;
    }
    
    uint32_t num_blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (num_blocks == 0) num_blocks = 1;
    
    DirEntry entries[DIRENTRIES_PER_BLOCK];
    
    for (uint32_t bi = 0; bi < num_blocks; ++bi) {
        auto block_result = getFileBlock(dir, bi);
        if (!block_result.ok()) {
            continue;
        }
        
        ErrorCode err = readDirectoryBlock(block_result.value(), entries);
        if (err != ErrorCode::OK) {
            continue;
        }
        
        for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
            if (!entries[i].isValid()) {
                entries[i].init(target_inode, name, type);
                
                err = writeDirectoryBlock(block_result.value(), entries);
                if (err != ErrorCode::OK) {
                    return err;
                }
                
                uint32_t new_size = bi * BLOCK_SIZE + (i + 1) * sizeof(DirEntry);
                if (new_size > dir.size) {
                    dir.size = new_size;
                }
                updateModifyTime(dir);
                
                return writeInode(dir_inode, dir);
            }
        }
    }
    
    auto new_block = getOrAllocFileBlock(dir, dir_inode, num_blocks);
    if (!new_block.ok()) {
        return new_block.error();
    }
    
    std::memset(entries, 0, sizeof(entries));
    for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
        entries[i].inode = INVALID_INODE;
    }
    
    entries[0].init(target_inode, name, type);
    
    ErrorCode err = writeDirectoryBlock(new_block.value(), entries);
    if (err != ErrorCode::OK) {
        return err;
    }
    
    inode_result = readInode(dir_inode);
    if (inode_result.ok()) {
        dir = inode_result.value();
        dir.size = num_blocks * BLOCK_SIZE + sizeof(DirEntry);
        updateModifyTime(dir);
    }
    
    return writeInode(dir_inode, dir);
}

ErrorCode Directory::removeEntry(InodeId dir_inode, const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (name == "." || name == "..") {
        return ErrorCode::E_PERMISSION;
    }
    
    auto inode_result = readInode(dir_inode);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    Inode dir = inode_result.value();
    if (!dir.isDirectory()) {
        return ErrorCode::E_NOT_DIR;
    }
    
    uint32_t num_blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    DirEntry entries[DIRENTRIES_PER_BLOCK];
    
    for (uint32_t bi = 0; bi < num_blocks; ++bi) {
        auto block_result = getFileBlock(dir, bi);
        if (!block_result.ok()) {
            continue;
        }
        
        ErrorCode err = readDirectoryBlock(block_result.value(), entries);
        if (err != ErrorCode::OK) {
            continue;
        }
        
        for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
            if (entries[i].isValid() && entries[i].getName() == name) {
                entries[i].clear();
                
                err = writeDirectoryBlock(block_result.value(), entries);
                if (err != ErrorCode::OK) {
                    return err;
                }
                
                updateModifyTime(dir);
                return writeInode(dir_inode, dir);
            }
        }
    }
    
    return ErrorCode::E_NOT_FOUND;
}

Result<std::vector<DirEntry>> Directory::listDirectory(InodeId dir_inode) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto inode_result = readInode(dir_inode);
    if (!inode_result.ok()) {
        return Result<std::vector<DirEntry>>::failure(inode_result.error());
    }
    
    const Inode& dir = inode_result.value();
    if (!dir.isDirectory()) {
        return Result<std::vector<DirEntry>>::failure(ErrorCode::E_NOT_DIR);
    }
    
    std::vector<DirEntry> result;
    uint32_t num_blocks = (dir.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    if (num_blocks == 0) num_blocks = 1;
    
    DirEntry entries[DIRENTRIES_PER_BLOCK];
    
    for (uint32_t bi = 0; bi < num_blocks; ++bi) {
        auto block_result = getFileBlock(dir, bi);
        if (!block_result.ok()) {
            continue;
        }
        
        ErrorCode err = readDirectoryBlock(block_result.value(), entries);
        if (err != ErrorCode::OK) {
            continue;
        }
        
        for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
            if (entries[i].isValid()) {
                result.push_back(entries[i]);
            }
        }
    }
    
    return Result<std::vector<DirEntry>>::success(std::move(result));
}

Result<std::vector<DirEntry>> Directory::list(const std::string& path) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return Result<std::vector<DirEntry>>::failure(inode_result.error());
    }
    
    return listDirectory(inode_result.value());
}

bool Directory::isDirectoryEmpty(InodeId dir_inode) {
    auto entries_result = listDirectory(dir_inode);
    if (!entries_result.ok()) {
        return false;
    }
    
    for (const auto& entry : entries_result.value()) {
        if (entry.getName() != "." && entry.getName() != "..") {
            return false;
        }
    }
    
    return true;
}

//==============================================================================
// 文件/目录创建与删除
//==============================================================================

Result<InodeId> Directory::mkdir(const std::string& path) {
    auto parent_result = resolveParent(path);
    if (!parent_result.ok()) {
        return Result<InodeId>::failure(parent_result.error());
    }
    
    const ParentInfo& parent = parent_result.value();
    
    if (!isValidFilename(parent.filename)) {
        return Result<InodeId>::failure(ErrorCode::E_NAME_TOO_LONG);
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = lookupInternal(parent.parent_inode, parent.filename);
        if (existing.ok()) {
            return Result<InodeId>::failure(ErrorCode::E_ALREADY_EXISTS);
        }
    }
    
    auto inode_result = alloc_->allocInode();
    if (!inode_result.ok()) {
        return Result<InodeId>::failure(inode_result.error());
    }
    
    InodeId new_inode = inode_result.value();
    
    auto block_result = alloc_->allocBlock();
    if (!block_result.ok()) {
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(block_result.error());
    }
    
    BlockNo dir_block = block_result.value();
    
    Inode dir;
    dir.init(FileType::DIRECTORY);
    dir.size = 2 * sizeof(DirEntry);
    dir.link_count = 2;
    dir.block_count = 1;
    dir.direct_blocks[0] = dir_block;
    dir.create_time = currentTime();
    dir.modify_time = dir.create_time;
    dir.access_time = dir.create_time;
    
    DirEntry entries[DIRENTRIES_PER_BLOCK];
    std::memset(entries, 0, sizeof(entries));
    for (uint32_t i = 0; i < DIRENTRIES_PER_BLOCK; ++i) {
        entries[i].inode = INVALID_INODE;
    }
    
    entries[0].init(new_inode, ".", FileType::DIRECTORY);
    entries[1].init(parent.parent_inode, "..", FileType::DIRECTORY);
    
    ErrorCode err = writeDirectoryBlock(dir_block, entries);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(dir_block);
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(err);
    }
    
    err = alloc_->writeInode(new_inode, dir);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(dir_block);
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(err);
    }
    
    err = addEntry(parent.parent_inode, parent.filename, new_inode, FileType::DIRECTORY);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(dir_block);
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(err);
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto parent_inode_result = readInode(parent.parent_inode);
        if (parent_inode_result.ok()) {
            Inode parent_inode = parent_inode_result.value();
            parent_inode.link_count++;
            writeInode(parent.parent_inode, parent_inode);
        }
    }
    
    alloc_->sync();
    
    return Result<InodeId>::success(new_inode);
}

ErrorCode Directory::rmdir(const std::string& path) {
    if (normalizePath(path) == "/") {
        return ErrorCode::E_PERMISSION;
    }
    
    auto parent_result = resolveParent(path);
    if (!parent_result.ok()) {
        return parent_result.error();
    }
    
    const ParentInfo& parent = parent_result.value();
    
    Result<DirEntry> entry_result = lookup(parent.parent_inode, parent.filename);
    if (!entry_result.ok()) {
        return ErrorCode::E_NOT_FOUND;
    }
    
    InodeId dir_inode = entry_result.value().inode;
    
    auto inode_result = alloc_->readInode(dir_inode);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    Inode dir = inode_result.value();
    if (!dir.isDirectory()) {
        return ErrorCode::E_NOT_DIR;
    }
    
    if (!isDirectoryEmpty(dir_inode)) {
        return ErrorCode::E_NOT_EMPTY;
    }
    
    for (uint32_t i = 0; i < NUM_DIRECT_BLOCKS; ++i) {
        if (dir.direct_blocks[i] != INVALID_BLOCK) {
            alloc_->freeBlock(dir.direct_blocks[i]);
        }
    }
    
    ErrorCode err = removeEntry(parent.parent_inode, parent.filename);
    if (err != ErrorCode::OK) {
        return err;
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto parent_inode_result = readInode(parent.parent_inode);
        if (parent_inode_result.ok()) {
            Inode parent_inode = parent_inode_result.value();
            if (parent_inode.link_count > 0) {
                parent_inode.link_count--;
            }
            writeInode(parent.parent_inode, parent_inode);
        }
    }
    
    alloc_->freeInode(dir_inode);
    alloc_->sync();
    
    return ErrorCode::OK;
}

Result<InodeId> Directory::createFile(const std::string& path) {
    auto parent_result = resolveParent(path);
    if (!parent_result.ok()) {
        return Result<InodeId>::failure(parent_result.error());
    }
    
    const ParentInfo& parent = parent_result.value();
    
    if (!isValidFilename(parent.filename)) {
        return Result<InodeId>::failure(ErrorCode::E_NAME_TOO_LONG);
    }
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = lookupInternal(parent.parent_inode, parent.filename);
        if (existing.ok()) {
            return Result<InodeId>::failure(ErrorCode::E_ALREADY_EXISTS);
        }
    }
    
    auto inode_result = alloc_->allocInode();
    if (!inode_result.ok()) {
        return Result<InodeId>::failure(inode_result.error());
    }
    
    InodeId new_inode = inode_result.value();
    
    Inode file;
    file.init(FileType::REGULAR);
    file.size = 0;
    file.link_count = 1;
    file.block_count = 0;
    file.create_time = currentTime();
    file.modify_time = file.create_time;
    file.access_time = file.create_time;
    
    ErrorCode err = alloc_->writeInode(new_inode, file);
    if (err != ErrorCode::OK) {
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(err);
    }
    
    err = addEntry(parent.parent_inode, parent.filename, new_inode, FileType::REGULAR);
    if (err != ErrorCode::OK) {
        alloc_->freeInode(new_inode);
        return Result<InodeId>::failure(err);
    }
    
    alloc_->sync();
    
    return Result<InodeId>::success(new_inode);
}

ErrorCode Directory::removeFile(const std::string& path) {
    auto parent_result = resolveParent(path);
    if (!parent_result.ok()) {
        return parent_result.error();
    }
    
    const ParentInfo& parent = parent_result.value();
    
    auto entry_result = lookup(parent.parent_inode, parent.filename);
    if (!entry_result.ok()) {
        return ErrorCode::E_NOT_FOUND;
    }
    
    InodeId file_inode = entry_result.value().inode;
    
    auto inode_result = alloc_->readInode(file_inode);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    Inode file = inode_result.value();
    if (!file.isRegularFile()) {
        return ErrorCode::E_IS_DIR;
    }
    
    ErrorCode err = removeEntry(parent.parent_inode, parent.filename);
    if (err != ErrorCode::OK) {
        return err;
    }
    
    file.link_count--;
    
    if (file.link_count == 0) {
        freeFileBlocks(file, 0);
        alloc_->freeInode(file_inode);
    } else {
        alloc_->writeInode(file_inode, file);
    }
    
    alloc_->sync();
    
    return ErrorCode::OK;
}

ErrorCode Directory::remove(const std::string& path) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    auto stat_result = statInode(inode_result.value());
    if (!stat_result.ok()) {
        return stat_result.error();
    }
    
    if (stat_result.value().type == FileType::DIRECTORY) {
        return rmdir(path);
    } else {
        return removeFile(path);
    }
}

//==============================================================================
// 文件读写接口
//==============================================================================

Result<std::vector<uint8_t>> Directory::readFile(const std::string& path,
                                                  uint32_t offset,
                                                  uint32_t length) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return Result<std::vector<uint8_t>>::failure(inode_result.error());
    }
    
    return readFileByInode(inode_result.value(), offset, length);
}

Result<std::vector<uint8_t>> Directory::readFileByInode(InodeId inode_id,
                                                         uint32_t offset,
                                                         uint32_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto inode_result = readInode(inode_id);
    if (!inode_result.ok()) {
        return Result<std::vector<uint8_t>>::failure(inode_result.error());
    }
    
    Inode inode = inode_result.value();
    if (!inode.isRegularFile()) {
        return Result<std::vector<uint8_t>>::failure(ErrorCode::E_IS_DIR);
    }
    
    if (offset >= inode.size) {
        return Result<std::vector<uint8_t>>::success(std::vector<uint8_t>());
    }
    
    if (length == 0 || offset + length > inode.size) {
        length = inode.size - offset;
    }
    
    std::vector<uint8_t> data(length);
    uint32_t bytes_read = 0;
    uint8_t block_buffer[BLOCK_SIZE];
    
    while (bytes_read < length) {
        uint32_t current_offset = offset + bytes_read;
        uint32_t block_index = current_offset / BLOCK_SIZE;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t to_read = std::min(BLOCK_SIZE - block_offset, length - bytes_read);
        
        auto block_result = getFileBlock(inode, block_index);
        if (!block_result.ok()) {
            std::memset(data.data() + bytes_read, 0, to_read);
        } else {
            ErrorCode err = readBlockInternal(block_result.value(), block_buffer);
            if (err != ErrorCode::OK) {
                return Result<std::vector<uint8_t>>::failure(err);
            }
            std::memcpy(data.data() + bytes_read, block_buffer + block_offset, to_read);
        }
        
        bytes_read += to_read;
    }
    
    updateAccessTime(inode);
    writeInode(inode_id, inode);
    
    return Result<std::vector<uint8_t>>::success(std::move(data));
}

Result<uint32_t> Directory::writeFile(const std::string& path,
                                       const std::vector<uint8_t>& data,
                                       uint32_t offset) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return Result<uint32_t>::failure(inode_result.error());
    }
    
    return writeFileByInode(inode_result.value(), data, offset);
}

Result<uint32_t> Directory::writeFile(const std::string& path,
                                       const std::string& data,
                                       uint32_t offset) {
    std::vector<uint8_t> bytes(data.begin(), data.end());
    return writeFile(path, bytes, offset);
}

Result<uint32_t> Directory::writeFileByInode(InodeId inode_id,
                                              const std::vector<uint8_t>& data,
                                              uint32_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (data.empty()) {
        return Result<uint32_t>::success(0);
    }
    
    auto inode_result = readInode(inode_id);
    if (!inode_result.ok()) {
        return Result<uint32_t>::failure(inode_result.error());
    }
    
    Inode inode = inode_result.value();
    if (!inode.isRegularFile()) {
        return Result<uint32_t>::failure(ErrorCode::E_IS_DIR);
    }
    
    uint32_t write_end = offset + static_cast<uint32_t>(data.size());
    
    if (write_end > Inode::maxFileSize()) {
        return Result<uint32_t>::failure(ErrorCode::E_FILE_TOO_LARGE);
    }
    
    uint32_t bytes_written = 0;
    uint8_t block_buffer[BLOCK_SIZE];
    
    while (bytes_written < data.size()) {
        uint32_t current_offset = offset + bytes_written;
        uint32_t block_index = current_offset / BLOCK_SIZE;
        uint32_t block_offset = current_offset % BLOCK_SIZE;
        uint32_t to_write = std::min(static_cast<uint32_t>(BLOCK_SIZE - block_offset),
                                     static_cast<uint32_t>(data.size() - bytes_written));
        
        auto block_result = getOrAllocFileBlock(inode, inode_id, block_index);
        if (!block_result.ok()) {
            if (bytes_written > 0) {
                break;
            }
            return Result<uint32_t>::failure(block_result.error());
        }
        
        BlockNo block_no = block_result.value();
        auto cow_result = cowDataBlockIfNeeded(inode, inode_id, block_index, block_no);
        if (!cow_result.ok()) {
            if (bytes_written > 0) {
                break;
            }
            return Result<uint32_t>::failure(cow_result.error());
        }
        block_no = cow_result.value();
        
        if (block_offset != 0 || to_write != BLOCK_SIZE) {
            ErrorCode err = readBlockInternal(block_no, block_buffer);
            if (err != ErrorCode::OK) {
                std::memset(block_buffer, 0, BLOCK_SIZE);
            }
        }
        
        std::memcpy(block_buffer + block_offset, data.data() + bytes_written, to_write);
        
        ErrorCode err = writeBlockInternal(block_no, block_buffer);
        if (err != ErrorCode::OK) {
            if (bytes_written > 0) {
                break;
            }
            return Result<uint32_t>::failure(err);
        }
        
        bytes_written += to_write;
    }
    
    inode_result = readInode(inode_id);
    if (inode_result.ok()) {
        Inode updated = inode_result.value();
        if (write_end > updated.size) {
            updated.size = write_end;
        }
        updateModifyTime(updated);
        writeInode(inode_id, updated);
    }
    
    return Result<uint32_t>::success(bytes_written);
}

ErrorCode Directory::truncate(const std::string& path, uint32_t new_size) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return inode_result.error();
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto read_result = readInode(inode_result.value());
    if (!read_result.ok()) {
        return read_result.error();
    }
    
    Inode inode = read_result.value();
    if (!inode.isRegularFile()) {
        return ErrorCode::E_IS_DIR;
    }
    
    if (new_size >= inode.size) {
        inode.size = new_size;
    } else {
        uint32_t new_blocks = (new_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        uint32_t old_blocks = (inode.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
        
        if (new_blocks < old_blocks) {
            freeFileBlocks(inode, new_blocks);
        }
        
        inode.size = new_size;
    }
    
    updateModifyTime(inode);
    return writeInode(inode_result.value(), inode);
}

Result<uint32_t> Directory::appendFile(const std::string& path,
                                        const std::vector<uint8_t>& data) {
    auto inode_result = resolvePath(path);
    if (!inode_result.ok()) {
        return Result<uint32_t>::failure(inode_result.error());
    }
    
    auto stat_result = statInode(inode_result.value());
    if (!stat_result.ok()) {
        return Result<uint32_t>::failure(stat_result.error());
    }
    
    return writeFileByInode(inode_result.value(), data, stat_result.value().size);
}

//==============================================================================
// 辅助接口
//==============================================================================

bool Directory::exists(const std::string& path) {
    auto result = resolvePath(path);
    return result.ok();
}

bool Directory::isDirectory(const std::string& path) {
    auto result = stat(path);
    return result.ok() && result.value().type == FileType::DIRECTORY;
}

bool Directory::isFile(const std::string& path) {
    auto result = stat(path);
    return result.ok() && result.value().type == FileType::REGULAR;
}

ErrorCode Directory::sync() {
    ErrorCode err = flushCache();
    if (err != ErrorCode::OK) {
        return err;
    }
    return alloc_->sync();
}

//==============================================================================
// 内部辅助方法 - 块操作
//==============================================================================

ErrorCode Directory::readDirectoryBlock(BlockNo block_no, DirEntry* entries) {
    return readBlockInternal(block_no, entries);
}

ErrorCode Directory::writeDirectoryBlock(BlockNo block_no, const DirEntry* entries) {
    return writeBlockInternal(block_no, entries);
}

Result<BlockNo> Directory::cowDataBlockIfNeeded(Inode& inode, InodeId inode_id,
                                                uint32_t block_index, BlockNo block_no) {
    if (!snap_) {
        return Result<BlockNo>::success(block_no);
    }
    if (!snap_->needsCOW(block_no)) {
        return Result<BlockNo>::success(block_no);
    }

    auto cow_result = snap_->performCOW(block_no);
    if (!cow_result.ok()) {
        return Result<BlockNo>::failure(cow_result.error());
    }

    BlockNo new_block = cow_result.value();
    ErrorCode err = updateFileBlockPointer(inode, inode_id, block_index, new_block);
    if (err != ErrorCode::OK) {
        return Result<BlockNo>::failure(err);
    }
    return Result<BlockNo>::success(new_block);
}

ErrorCode Directory::updateFileBlockPointer(Inode& inode, InodeId inode_id,
                                            uint32_t block_index, BlockNo new_block) {
    if (block_index < NUM_DIRECT_BLOCKS) {
        inode.direct_blocks[block_index] = new_block;
        return writeInode(inode_id, inode);
    }

    block_index -= NUM_DIRECT_BLOCKS;

    if (block_index < PTRS_PER_BLOCK) {
        if (inode.single_indirect == INVALID_BLOCK) {
            return ErrorCode::E_INVALID_PARAM;
        }
        BlockNo indirect = inode.single_indirect;
        if (snap_ && snap_->needsCOW(indirect)) {
            auto cow = snap_->performCOW(indirect);
            if (!cow.ok()) {
                return cow.error();
            }
            if (cow.value() != indirect) {
                inode.single_indirect = cow.value();
                indirect = cow.value();
                ErrorCode err = writeInode(inode_id, inode);
                if (err != ErrorCode::OK) {
                    return err;
                }
            }
        }
        return setIndirectBlock(indirect, block_index, new_block);
    }

    block_index -= PTRS_PER_BLOCK;

    if (block_index < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode.double_indirect == INVALID_BLOCK) {
            return ErrorCode::E_INVALID_PARAM;
        }
        BlockNo l1_block = inode.double_indirect;
        if (snap_ && snap_->needsCOW(l1_block)) {
            auto cow = snap_->performCOW(l1_block);
            if (!cow.ok()) {
                return cow.error();
            }
            if (cow.value() != l1_block) {
                inode.double_indirect = cow.value();
                l1_block = cow.value();
                ErrorCode err = writeInode(inode_id, inode);
                if (err != ErrorCode::OK) {
                    return err;
                }
            }
        }

        uint32_t l1_index = block_index / PTRS_PER_BLOCK;
        uint32_t l2_index = block_index % PTRS_PER_BLOCK;

        uint8_t l1_data[BLOCK_SIZE];
        ErrorCode err = readBlockInternal(l1_block, l1_data);
        if (err != ErrorCode::OK) {
            return err;
        }
        BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
        if (l1_ptrs[l1_index] == INVALID_BLOCK) {
            return ErrorCode::E_INVALID_PARAM;
        }

        BlockNo l2_block = l1_ptrs[l1_index];
        if (snap_ && snap_->needsCOW(l2_block)) {
            auto cow = snap_->performCOW(l2_block);
            if (!cow.ok()) {
                return cow.error();
            }
            if (cow.value() != l2_block) {
                l2_block = cow.value();
                l1_ptrs[l1_index] = l2_block;
                err = writeBlockInternal(l1_block, l1_data);
                if (err != ErrorCode::OK) {
                    return err;
                }
            }
        }

        uint8_t l2_data[BLOCK_SIZE];
        err = readBlockInternal(l2_block, l2_data);
        if (err != ErrorCode::OK) {
            return err;
        }
        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
        l2_ptrs[l2_index] = new_block;
        return writeBlockInternal(l2_block, l2_data);
    }

    return ErrorCode::E_FILE_TOO_LARGE;
}

Result<BlockNo> Directory::getFileBlock(const Inode& inode, uint32_t block_index) {
    if (block_index < NUM_DIRECT_BLOCKS) {
        BlockNo block = inode.direct_blocks[block_index];
        if (block == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        return Result<BlockNo>::success(block);
    }
    
    block_index -= NUM_DIRECT_BLOCKS;
    
    if (block_index < PTRS_PER_BLOCK) {
        if (inode.single_indirect == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        return getIndirectBlock(inode.single_indirect, block_index);
    }
    
    block_index -= PTRS_PER_BLOCK;
    
    if (block_index < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode.double_indirect == INVALID_BLOCK) {
            return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
        }
        
        uint32_t l1_index = block_index / PTRS_PER_BLOCK;
        uint32_t l2_index = block_index % PTRS_PER_BLOCK;
        
        auto l1_result = getIndirectBlock(inode.double_indirect, l1_index);
        if (!l1_result.ok()) {
            return l1_result;
        }
        
        return getIndirectBlock(l1_result.value(), l2_index);
    }
    
    return Result<BlockNo>::failure(ErrorCode::E_FILE_TOO_LARGE);
}

Result<BlockNo> Directory::getOrAllocFileBlock(Inode& inode, InodeId inode_id, 
                                                uint32_t block_index) {
    if (block_index < NUM_DIRECT_BLOCKS) {
        if (inode.direct_blocks[block_index] == INVALID_BLOCK) {
            auto alloc_result = alloc_->allocBlock();
            if (!alloc_result.ok()) {
                return alloc_result;
            }
            inode.direct_blocks[block_index] = alloc_result.value();
            inode.block_count++;
            writeInode(inode_id, inode);
        }
        return Result<BlockNo>::success(inode.direct_blocks[block_index]);
    }
    
    block_index -= NUM_DIRECT_BLOCKS;
    
    if (block_index < PTRS_PER_BLOCK) {
        if (inode.single_indirect == INVALID_BLOCK) {
            auto alloc_result = allocIndirectBlock();
            if (!alloc_result.ok()) {
                return alloc_result;
            }
            inode.single_indirect = alloc_result.value();
            inode.block_count++;
            writeInode(inode_id, inode);
        }

        BlockNo indirect = inode.single_indirect;
        if (snap_ && snap_->needsCOW(indirect)) {
            auto cow = snap_->performCOW(indirect);
            if (!cow.ok()) {
                return Result<BlockNo>::failure(cow.error());
            }
            if (cow.value() != indirect) {
                inode.single_indirect = cow.value();
                indirect = cow.value();
                ErrorCode err = writeInode(inode_id, inode);
                if (err != ErrorCode::OK) {
                    return Result<BlockNo>::failure(err);
                }
            }
        }

        auto existing = getIndirectBlock(indirect, block_index);
        if (existing.ok()) {
            return existing;
        }
        
        auto alloc_result = alloc_->allocBlock();
        if (!alloc_result.ok()) {
            return alloc_result;
        }
        
        ErrorCode err = setIndirectBlock(indirect, block_index,
                                         alloc_result.value());
        if (err != ErrorCode::OK) {
            alloc_->freeBlock(alloc_result.value());
            return Result<BlockNo>::failure(err);
        }
        
        inode.block_count++;
        writeInode(inode_id, inode);
        
        return alloc_result;
    }
    
    block_index -= PTRS_PER_BLOCK;
    
    if (block_index < PTRS_PER_BLOCK * PTRS_PER_BLOCK) {
        if (inode.double_indirect == INVALID_BLOCK) {
            auto alloc_result = allocIndirectBlock();
            if (!alloc_result.ok()) {
                return alloc_result;
            }
            inode.double_indirect = alloc_result.value();
            inode.block_count++;
            writeInode(inode_id, inode);
        }

        BlockNo dbl = inode.double_indirect;
        if (snap_ && snap_->needsCOW(dbl)) {
            auto cow = snap_->performCOW(dbl);
            if (!cow.ok()) {
                return Result<BlockNo>::failure(cow.error());
            }
            if (cow.value() != dbl) {
                inode.double_indirect = cow.value();
                dbl = cow.value();
                ErrorCode err = writeInode(inode_id, inode);
                if (err != ErrorCode::OK) {
                    return Result<BlockNo>::failure(err);
                }
            }
        }

        uint32_t l1_index = block_index / PTRS_PER_BLOCK;
        uint32_t l2_index = block_index % PTRS_PER_BLOCK;
        
        auto l1_result = getIndirectBlock(dbl, l1_index);
        BlockNo l1_block;
        
        if (!l1_result.ok()) {
            auto alloc_result = allocIndirectBlock();
            if (!alloc_result.ok()) {
                return alloc_result;
            }
            l1_block = alloc_result.value();
            
            ErrorCode err = setIndirectBlock(dbl, l1_index, l1_block);
            if (err != ErrorCode::OK) {
                alloc_->freeBlock(l1_block);
                return Result<BlockNo>::failure(err);
            }
            
            inode.block_count++;
            writeInode(inode_id, inode);
        } else {
            l1_block = l1_result.value();
        }

        if (snap_ && snap_->needsCOW(l1_block)) {
            auto cow = snap_->performCOW(l1_block);
            if (!cow.ok()) {
                return Result<BlockNo>::failure(cow.error());
            }
            if (cow.value() != l1_block) {
                l1_block = cow.value();
                ErrorCode err = setIndirectBlock(dbl, l1_index, l1_block);
                if (err != ErrorCode::OK) {
                    return Result<BlockNo>::failure(err);
                }
            }
        }
        
        auto existing = getIndirectBlock(l1_block, l2_index);
        if (existing.ok()) {
            return existing;
        }
        
        auto alloc_result = alloc_->allocBlock();
        if (!alloc_result.ok()) {
            return alloc_result;
        }
        
        ErrorCode err = setIndirectBlock(l1_block, l2_index, alloc_result.value());
        if (err != ErrorCode::OK) {
            alloc_->freeBlock(alloc_result.value());
            return Result<BlockNo>::failure(err);
        }
        
        inode.block_count++;
        writeInode(inode_id, inode);
        
        return alloc_result;
    }
    
    return Result<BlockNo>::failure(ErrorCode::E_FILE_TOO_LARGE);
}

ErrorCode Directory::freeFileBlocks(Inode& inode, uint32_t from_block) {
    for (uint32_t i = from_block; i < NUM_DIRECT_BLOCKS; ++i) {
        if (inode.direct_blocks[i] != INVALID_BLOCK) {
            alloc_->freeBlock(inode.direct_blocks[i]);
            inode.direct_blocks[i] = INVALID_BLOCK;
            if (inode.block_count > 0) inode.block_count--;
        }
    }
    
    if (from_block <= NUM_DIRECT_BLOCKS && inode.single_indirect != INVALID_BLOCK) {
        uint8_t block_data[BLOCK_SIZE];
        if (readBlockInternal(inode.single_indirect, block_data) == ErrorCode::OK) {
            BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (ptrs[i] != INVALID_BLOCK) {
                    alloc_->freeBlock(ptrs[i]);
                    if (inode.block_count > 0) inode.block_count--;
                }
            }
        }
        alloc_->freeBlock(inode.single_indirect);
        inode.single_indirect = INVALID_BLOCK;
        if (inode.block_count > 0) inode.block_count--;
    }
    
    if (from_block <= NUM_DIRECT_BLOCKS + PTRS_PER_BLOCK && 
        inode.double_indirect != INVALID_BLOCK) {
        uint8_t l1_data[BLOCK_SIZE];
        if (readBlockInternal(inode.double_indirect, l1_data) == ErrorCode::OK) {
            BlockNo* l1_ptrs = reinterpret_cast<BlockNo*>(l1_data);
            
            for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
                if (l1_ptrs[i] != INVALID_BLOCK) {
                    uint8_t l2_data[BLOCK_SIZE];
                    if (readBlockInternal(l1_ptrs[i], l2_data) == ErrorCode::OK) {
                        BlockNo* l2_ptrs = reinterpret_cast<BlockNo*>(l2_data);
                        for (uint32_t j = 0; j < PTRS_PER_BLOCK; ++j) {
                            if (l2_ptrs[j] != INVALID_BLOCK) {
                                alloc_->freeBlock(l2_ptrs[j]);
                                if (inode.block_count > 0) inode.block_count--;
                            }
                        }
                    }
                    alloc_->freeBlock(l1_ptrs[i]);
                    if (inode.block_count > 0) inode.block_count--;
                }
            }
        }
        alloc_->freeBlock(inode.double_indirect);
        inode.double_indirect = INVALID_BLOCK;
        if (inode.block_count > 0) inode.block_count--;
    }
    
    return ErrorCode::OK;
}

Result<BlockNo> Directory::getIndirectBlock(BlockNo indirect_block, uint32_t index) {
    if (indirect_block == INVALID_BLOCK || index >= PTRS_PER_BLOCK) {
        return Result<BlockNo>::failure(ErrorCode::E_INVALID_PARAM);
    }
    
    uint8_t block_data[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(indirect_block, block_data);
    if (err != ErrorCode::OK) {
        return Result<BlockNo>::failure(err);
    }
    
    BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
    if (ptrs[index] == INVALID_BLOCK) {
        return Result<BlockNo>::failure(ErrorCode::E_NOT_FOUND);
    }
    
    return Result<BlockNo>::success(ptrs[index]);
}

ErrorCode Directory::setIndirectBlock(BlockNo indirect_block, uint32_t index, 
                                       BlockNo value) {
    if (indirect_block == INVALID_BLOCK || index >= PTRS_PER_BLOCK) {
        return ErrorCode::E_INVALID_PARAM;
    }
    
    uint8_t block_data[BLOCK_SIZE];
    ErrorCode err = readBlockInternal(indirect_block, block_data);
    if (err != ErrorCode::OK) {
        return err;
    }
    
    BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
    ptrs[index] = value;
    
    return writeBlockInternal(indirect_block, block_data);
}

Result<BlockNo> Directory::allocIndirectBlock() {
    auto result = alloc_->allocBlock();
    if (!result.ok()) {
        return result;
    }
    
    uint8_t block_data[BLOCK_SIZE];
    BlockNo* ptrs = reinterpret_cast<BlockNo*>(block_data);
    for (uint32_t i = 0; i < PTRS_PER_BLOCK; ++i) {
        ptrs[i] = INVALID_BLOCK;
    }
    
    ErrorCode err = writeBlockInternal(result.value(), block_data);
    if (err != ErrorCode::OK) {
        alloc_->freeBlock(result.value());
        return Result<BlockNo>::failure(err);
    }
    
    return result;
}

//==============================================================================
// inode 操作
//==============================================================================

Result<Inode> Directory::readInode(InodeId inode_id) {
    return alloc_->readInode(inode_id);
}

ErrorCode Directory::writeInode(InodeId inode_id, const Inode& inode) {
    return alloc_->writeInode(inode_id, inode);
}

//==============================================================================
// 时间操作
//==============================================================================

void Directory::updateAccessTime(Inode& inode) {
    inode.access_time = currentTime();
}

void Directory::updateModifyTime(Inode& inode) {
    int64_t now = currentTime();
    inode.modify_time = now;
    inode.access_time = now;
}

int64_t Directory::currentTime() {
    return static_cast<int64_t>(std::time(nullptr));
}

} // namespace fs
