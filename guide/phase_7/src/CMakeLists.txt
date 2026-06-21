cmake_minimum_required(VERSION 3.10)
project(Phase7_EchoServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(echo_server echo_server.cpp)
target_link_libraries(echo_server pthread)
