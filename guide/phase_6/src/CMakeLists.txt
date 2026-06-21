cmake_minimum_required(VERSION 3.10)
project(Phase6_HttpParser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

add_executable(test_http test_http_parser.cpp)
target_link_libraries(test_http pthread)
