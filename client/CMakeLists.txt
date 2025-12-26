cmake_minimum_required(VERSION 3.16)
project(client_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include headers
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Build client library (so later modules link cleanly)
add_library(client_lib
  src/cli/CliApp.cpp
  src/net/TcpTransport.cpp
  src/protocol/ClientProtocol.cpp
  src/session/SessionStore.cpp
)

# Main executable
add_executable(client_cli
  src/main.cpp
)

target_link_libraries(client_cli PRIVATE client_lib)

# Smoke test executable
add_executable(test_client_smoke
  test/test_client_smoke.cpp
)
target_link_libraries(test_client_smoke PRIVATE client_lib)
