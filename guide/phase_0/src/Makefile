# Phase 0 — Makefile
# 这是最简明的 Makefile，用于理解 make 的工作原理
# 学习要点见 phase_0/README.md 的"Step 3: Make"部分
#
# Makefile 语法：
# target: dependencies
# <TAB>   command
#
# make 的工作方式：
# 1. 检查 target 是否存在
# 2. 检查 dependencies 是否比 target 新
# 3. 如果需要更新，执行 command

# [语法] 变量定义：CXX = 编译器
CXX = g++

# [语法] 变量定义：CXXFLAGS = 编译选项
# -g        : 生成调试信息（gdb 需要）
# -Wall     : 开启几乎所有警告（推荐！）
# -Wextra   : 额外警告（比 -Wall 更多）
# -std=c++11: 使用 C++11 标准
# -DDEBUG   : 定义 DEBUG 宏（等于代码里写 #define DEBUG）
CXXFLAGS = -g -Wall -Wextra -std=c++11 -DDEBUG

# [语法] 第一个 target 是默认 target（只敲 make 时执行）
hello: hello.cpp
	$(CXX) $(CXXFLAGS) -o hello hello.cpp

# [语法] .PHONY — 声明 clean 不是文件名，而是一个"伪目标"
.PHONY: clean

clean:
	rm -f hello

# [语法] run 目标 — 编译并运行
.PHONY: run
run: hello
	./hello Hello from Makefile!
