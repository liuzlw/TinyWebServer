cmake_minimum_required(VERSION 3.10)
project(Phase2_BlockQueue_Log VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

# 静态库：block_queue + log
add_library(log_lib STATIC
    log.cpp
)

# 测试程序
add_executable(test_log test_log.cpp)
target_link_libraries(test_log log_lib pthread)
