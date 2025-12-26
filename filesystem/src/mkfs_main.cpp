// filesystem/src/mkfs_main.cpp
#include "DiskImage.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

void printUsage(const char* prog) {
    std::cout << "Usage: " << prog << " [options] <disk_image>" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -b <blocks>   Total number of blocks (default: 16384)" << std::endl;
    std::cout << "  -i <inodes>   Total number of inodes (default: 1024)" << std::endl;
    std::cout << "  -f            Force overwrite existing file" << std::endl;
    std::cout << "  -v            Verbose output" << std::endl;
    std::cout << "  -h            Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Example:" << std::endl;
    std::cout << "  " << prog << " -b 8192 -i 512 -v disk.img" << std::endl;
}

int main(int argc, char* argv[]) {
    fs::MkfsOptions opts;
    std::string path;

    // 解析命令行参数
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (std::strcmp(argv[i], "-b") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: -b requires an argument" << std::endl;
                return 1;
            }
            opts.total_blocks = std::atoi(argv[i]);
        } else if (std::strcmp(argv[i], "-i") == 0) {
            if (++i >= argc) {
                std::cerr << "Error: -i requires an argument" << std::endl;
                return 1;
            }
            opts.total_inodes = std::atoi(argv[i]);
        } else if (std::strcmp(argv[i], "-f") == 0) {
            opts.force = true;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            opts.verbose = true;
        } else if (argv[i][0] == '-') {
            std::cerr << "Unknown option: " << argv[i] << std::endl;
            printUsage(argv[0]);
            return 1;
        } else {
            path = argv[i];
        }
    }

    if (path.empty()) {
        std::cerr << "Error: No disk image path specified" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // 执行格式化
    fs::MkfsResult result = fs::mkfs(path, opts);

    if (result.error != fs::ErrorCode::OK) {
        std::cerr << "Error: " << result.message << std::endl;
        std::cerr << "Error code: " << fs::errorCodeToString(result.error) << std::endl;
        return 1;
    }

    if (!opts.verbose) {
        std::cout << "Filesystem created: " << path << std::endl;
        std::cout << "  Total size: " << (result.total_blocks * fs::BLOCK_SIZE / 1024) << " KB" << std::endl;
        std::cout << "  Available: " << (result.data_blocks * fs::BLOCK_SIZE / 1024) << " KB" << std::endl;
        std::cout << "  Inodes: " << result.total_inodes << std::endl;
    }

    return 0;
}