#include "filesystem/include/FS.h"
#include <iostream>

int main() {
    fs::FileSystem fs;
    
    // 格式化（16MB，1024 个 inode）
    fs.format("review_system.img", 16384, 1024);
    
    // 挂载
    fs::FSConfig config;
    config.cache_capacity = 128;
    fs.mount("review_system.img", config);
    
    // 创建审稿系统目录结构
    fs.mkdirp("/papers");
    fs.mkdirp("/users");
    fs.mkdirp("/reviews");
    
    // 创建论文
    fs.mkdirp("/papers/paper001/versions");
    fs.create("/papers/paper001/metadata.json");
    fs.writeFile("/papers/paper001/metadata.json", 
                 R"({"title": "My Paper", "author": "John"})");
    
    // 创建快照
    fs.createSnapshot("before_review");
    
    // 添加评审
    fs.create("/papers/paper001/review.txt");
    fs.writeFile("/papers/paper001/review.txt", "Accept with minor revisions");
    
    // 查看目录树
    fs.printTree("/");
    
    // 获取统计
    auto info = fs.getInfo();
    std::cout << "Used: " << info.used_blocks << " blocks" << std::endl;
    
    // 卸载
    fs.unmount();
    
    return 0;
}