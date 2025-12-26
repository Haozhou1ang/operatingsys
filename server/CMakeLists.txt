cmake_minimum_required(VERSION 3.16)
project(paper_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)
include_directories(${CMAKE_CURRENT_LIST_DIR}/../filesystem/include)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../filesystem
                 ${CMAKE_CURRENT_BINARY_DIR}/filesystem_build)

add_executable(server
  main.cpp
  src/auth/auth.cpp
  src/services/paper.cpp
  src/storage/admin_fs.cpp
  src/protocol/protocol.cpp
  src/net/net.cpp
)

target_link_libraries(server pthread reviewfs)
