cmake_minimum_required(VERSION 3.10)
project(Phase1_SyncPrimitives VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(test_sync test_producer_consumer.cpp)
target_link_libraries(test_sync pthread)
