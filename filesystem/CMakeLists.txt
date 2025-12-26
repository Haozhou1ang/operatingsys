# filesystem/CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(ReviewSystemFS)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

set(FS_SOURCES
    src/DiskImage.cpp
    src/Allocator.cpp
    src/Directory.cpp
    src/Cache.cpp
    src/Snapshot.cpp
    src/FS.cpp
)

add_library(reviewfs STATIC ${FS_SOURCES})

# 工具
add_executable(mkfs_tool src/mkfs_main.cpp)
target_link_libraries(mkfs_tool reviewfs)

# 测试程序
add_executable(test_diskimage src/test_diskimage.cpp)
target_link_libraries(test_diskimage reviewfs)

add_executable(test_allocator src/test_allocator.cpp)
target_link_libraries(test_allocator reviewfs)

add_executable(test_directory src/test_directory.cpp)
target_link_libraries(test_directory reviewfs)

add_executable(test_cache src/test_cache.cpp)
target_link_libraries(test_cache reviewfs)

add_executable(test_cache_integration src/test_cache_integration.cpp)
target_link_libraries(test_cache_integration reviewfs)

add_executable(test_snapshot src/test_snapshot.cpp)
target_link_libraries(test_snapshot reviewfs)

add_executable(test_fs src/test_fs.cpp)
target_link_libraries(test_fs reviewfs)

# 编译选项
if(MSVC)
    target_compile_options(reviewfs PRIVATE /W4 /Zi)
else()
    set(COMMON_FLAGS -Wall -Wextra -g -O0)
    target_compile_options(reviewfs PRIVATE ${COMMON_FLAGS})
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")