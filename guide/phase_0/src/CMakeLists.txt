# Phase 0 — CMakeLists.txt
# 这是最简单的 CMake 构建文件
# 学习要点见 phase_0/README.md 的"Step 4: CMake"部分

cmake_minimum_required(VERSION 3.10)

# [语法] project() — 定义项目名称、版本、语言
project(hello_phase0 VERSION 0.1.0 LANGUAGES CXX)

# [语法] set() — 设置变量
# CMAKE_CXX_STANDARD — 指定 C++ 标准版本
# 11 = C++11, 14 = C++14, 17 = C++17, 20 = C++20
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# [语法] add_executable() — 定义要生成的可执行文件
# 第一个参数是输出文件名，后面是源文件列表
add_executable(hello hello.cpp)

# [语法] target_compile_definitions() — 给特定目标添加宏定义
# 这相当于在代码里写 #define DEBUG
# 编译 Release 版本时可以注释掉这行
target_compile_definitions(hello PRIVATE DEBUG)
