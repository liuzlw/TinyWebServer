# Phase 0 —— 环境搭建与工具入门

## 本阶段目标

在本阶段结束时，你将：

1. 拥有一套可用的 Linux C++ 开发环境（WSL / 虚拟机 / 云服务器）
2. 理解 **g++ → make → cmake** 的演进关系，知道"为什么需要构建工具"
3. 能独立编写 CMakeLists.txt 和 Makefile 并完成编译
4. 能用 gdb 设置断点、单步执行、查看变量、查看调用栈

**可见结果：**

```bash
# 编译
$ mkdir -p build && cd build && cmake .. && make
# 运行
$ ./hello Hello from CMake!
Hello, TinyWebServer!
[DEBUG] Program compiled in debug mode.
Arguments: [0] ./hello [1] Hello [2] from [3] CMake!
```

**验收标准（全部满足才能进入 Phase 1）：**

- [ ] `g++ --version` 显示 7.x 或更高
- [ ] `cmake --version` 显示 3.10 或更高  
- [ ] `gdb --version` 显示 8.x 或更高
- [ ] 能编译运行 `src/hello.cpp`，输出正确
- [ ] 在 gdb 中能设置断点、单步、查看变量

---

## 理论与机制

### 1. 从源码到可执行文件：编译器做了什么？

```
hello.cpp  ──预处理──▶  hello.i  ──编译──▶  hello.s  ──汇编──▶  hello.o  ──链接──▶  hello
(源码)     (宏展开)     (汇编代码)  (目标文件)  (可执行文件)
```

| 阶段 | 命令 | 做什么 | 产物 |
|------|------|--------|------|
| **预处理** | `g++ -E hello.cpp -o hello.i` | 展开 `#include`、`#define` 宏、删除注释 | 纯 C++ 文本（很大） |
| **编译** | `g++ -S hello.i -o hello.s` | 把 C++ 转成汇编语言 | 汇编代码（人类可读） |
| **汇编** | `g++ -c hello.s -o hello.o` | 把汇编转成机器码 | 目标文件（二进制） |
| **链接** | `g++ hello.o -o hello` | 把多个 .o 文件和库合并 | 可执行文件 |

**为什么分这么多步？** 
- 大型项目可能有几千个 .cpp 文件。如果只改一个文件，只需要**重新编译那一个 .o**，然后**重新链接**。不用全部重来。
- 这是增量编译的基础 —— make 和 cmake 本质上就是在管理"哪些 .o 需要重新编译"。

### 2. g++ → make → cmake：为什么构建工具在进化？

假设项目有 3 个文件：`main.cpp`、`foo.cpp`、`foo.h`。

**只用 g++（手敲命令）：**

```bash
g++ -c main.cpp -o main.o
g++ -c foo.cpp -o foo.o
g++ main.o foo.o -o myapp
```

每次改一个文件都要**想清楚哪些文件受影响**，全部手敲。

**用 Make（自动化依赖跟踪）：**

```makefile
myapp: main.o foo.o
	g++ main.o foo.o -o myapp
main.o: main.cpp foo.h
	g++ -c main.cpp -o main.o
foo.o: foo.cpp foo.h
	g++ -c foo.cpp -o foo.o
```

`make` 自动比较文件修改时间，只重编译被影响的文件。但 Makefile 要手写，跨平台会出问题。

**用 CMake（跨平台构建描述）：**

```cmake
add_executable(myapp main.cpp foo.cpp)
```

`cmake` 自动分析依赖，生成平台相关的构建文件（Linux 上生成 Makefile，Windows 上生成 Visual Studio 项目）。**写一次 CMakeLists.txt，到处都能构建。**

```
你写的           CMake 生成的         make 读的
CMakeLists.txt →  Makefile          →  执行编译
       ↑              ↑                  ↑
   跨平台描述      平台适配            实际构建
```

### 3. gdb 调试的核心概念

程序出 bug 时，`printf` 调试法的最大问题是：每次加一行 `printf` 就要重新编译。gdb 让你在**不修改源码**的情况下：

- **断点（breakpoint）**：让程序在指定行暂停
- **单步执行（step/next）**：一行一行执行，观察控制流
- **查看变量（print）**：在暂停时看任何变量的值
- **调用栈（backtrace）**：看"我是怎么被调用到这里的"

---

## 实现指南

### Step 1：环境安装（Linux）

**选项 A：WSL（Windows Subsystem for Linux）— 推荐 Windows 用户**

```powershell
# 在 PowerShell（管理员）中执行
wsl --install -d Ubuntu-22.04
```

**选项 B：虚拟机**

安装 VirtualBox + Ubuntu 22.04 Server/Desktop 镜像。

**选项 C：云服务器**

阿里云 / 腾讯云买一台最低配的 ECS，选 Ubuntu 22.04。

**安装开发工具（所有选项通用）：**

```bash
sudo apt update
sudo apt install -y build-essential g++ gdb cmake make git
sudo apt install -y libmysqlclient-dev  # Phase 3 用，先装上
```

**验证安装：**

```bash
g++ --version    # 应显示 11.x 或类似
gdb --version    # 应显示 12.x 或类似
cmake --version  # 应显示 3.22.x 或类似
```

### Step 2：第一个程序（只用 g++）

```cpp
// hello.cpp
#include <iostream>

int main() {
    std::cout << "Hello, TinyWebServer!" << std::endl;
    return 0;
}
```

```bash
# 直接编译 + 链接一步完成
g++ -g -Wall -std=c++11 -o hello hello.cpp
./hello
# 输出: Hello, TinyWebServer!

# 分步编译（理解原理）
g++ -E hello.cpp -o hello.i    # 预处理
g++ -S hello.i -o hello.s      # 编译
g++ -c hello.s -o hello.o      # 汇编
g++ hello.o -o hello           # 链接
```

**⚠️ 常见错误：**

| 错误信息 | 原因 | 解决 |
|---------|------|------|
| `g++: command not found` | 没装编译器 | `sudo apt install g++` |
| `undefined reference to ...` | 链接时找不到函数定义 | 检查是否漏了 .cpp 文件或 -l 库 |
| `No such file or directory` | 路径错误 | `ls` 确认文件存在 |
| `error: expected ';' before ...` | 语法错误（C++ 语句必须以 `;` 结尾！） | 检查对应行 |

### Step 3：用 Make 管理编译

见 `src/Makefile`。核心语法：

```makefile
# 变量
CXX = g++
CXXFLAGS = -g -Wall -std=c++11

# 规则
目标: 依赖
	命令（必须以 TAB 开头，不能是空格！）

# 默认目标（第一个）
hello: hello.cpp
	$(CXX) $(CXXFLAGS) -o hello hello.cpp

# 伪目标（不生成文件）
.PHONY: clean
clean:
	rm -f hello
```

**⚠️ Makefile 最常见的错误：** 命令前面的缩进必须是 **TAB 字符**，不是空格！如果从网页复制 Makefile，经常会把 TAB 变成空格，导致 `make: *** missing separator. Stop.`

### Step 4：用 CMake 管理构建

见 `src/CMakeLists.txt`。CMake 的核心命令：

| 命令 | 用途 |
|------|------|
| `cmake_minimum_required(VERSION x.y)` | 指定 CMake 最低版本 |
| `project(Name LANGUAGES CXX)` | 定义项目 |
| `set(VAR value)` | 设置变量 |
| `add_executable(name src1.cpp src2.cpp)` | 生成可执行文件 |
| `target_link_libraries(name lib1 lib2)` | 链接库 |
| `include_directories(path)` | 添加头文件搜索路径 |

**cmake 的标准工作流：**

```bash
# 1. 创建构建目录（out-of-source build）
mkdir -p build && cd build

# 2. 生成构建文件（CMakeLists.txt 所在的目录）
cmake ..

# 3. 编译
make

# 4. 运行
./hello

# 一键重新编译（改了代码后）
make -C build
```

**为什么用 `build/` 目录？** 这叫 "out-of-source build"，把编译产物和源码分离。`.gitignore` 只需要忽略 `build/` 即可，不用逐个排除 `.o` 文件。

### Step 5：gdb 调试入门

```bash
# 编译时必须加 -g（生成调试符号）
g++ -g -Wall -std=c++11 -o hello hello.cpp

# 启动 gdb
gdb ./hello

# === gdb 常用命令速查 ===
(gdb) break main           # 在 main 函数入口设断点
(gdb) break hello.cpp:10   # 在 hello.cpp 第 10 行设断点
(gdb) run                  # 运行程序（到第一个断点停下）
(gdb) run arg1 arg2        # 带参数运行
(gdb) next                 # 单步执行（不进入函数内部）
(gdb) step                 # 单步执行（进入函数内部）
(gdb) continue             # 继续运行到下一个断点
(gdb) print var            # 打印变量值
(gdb) print &var           # 打印变量地址
(gdb) info locals          # 显示所有局部变量
(gdb) backtrace            # 显示调用栈（bt 是简写）
(gdb) list                 # 显示当前行周围的源码
(gdb) quit                 # 退出
```

**实操练习：**

```bash
$ gdb ./hello

(gdb) break main
Breakpoint 1 at 0x11a0: file hello.cpp, line 14.

(gdb) run
Starting program: ./hello
Breakpoint 1, main (argc=1, argv=0x7fffffffe2f8) at hello.cpp:14

(gdb) print argc
$1 = 1

(gdb) next
Hello, TinyWebServer!
...

(gdb) continue
Continuing.
[Inferior 1 (process 12345) exited normally]
```

---

## 验证用例与预期结果

### 测试 1：验证 g++ 编译

```bash
cd guide/phase_0/src
g++ -g -Wall -std=c++11 -o hello hello.cpp
./hello
```

**预期输出：**
```
Hello, TinyWebServer!
[DEBUG] Program compiled in debug mode.
Arguments: [0] ./hello
```

### 测试 2：验证 Make

```bash
cd guide/phase_0/src
make clean
make
./hello
make run
```

**预期：** `make` 输出编译命令，`./hello` 输出正确。`make run` 输出 "Hello from Makefile!" 作为参数。

### 测试 3：验证 CMake

```bash
cd guide/phase_0/src
mkdir -p build && cd build
cmake ..
make
./hello CMake test
```

**预期输出包含：**
```
Hello, TinyWebServer!
[DEBUG] Program compiled in debug mode.
Arguments: [0] ./hello [1] CMake [2] test
```

### 测试 4：验证 gdb

```bash
cd guide/phase_0/src
g++ -g -Wall -std=c++11 -DDEBUG -o hello hello.cpp
gdb ./hello
```

在 gdb 中执行以下操作并确认结果：

| gdb 命令 | 预期结果 |
|----------|---------|
| `break main` | 显示 `Breakpoint 1 at ...` |
| `run` | 停在 main 第一行 |
| `print argc` | 显示 `$1 = 1` |
| `next` | 执行到下一行 |
| `info locals` | 显示 `name = ...` |
| `backtrace` | 显示 `#0 main (...) at hello.cpp:...` |

### 失败排查

| 问题 | 可能原因 |
|------|---------|
| cmake 报错 `No CMAKE_CXX_COMPILER could be found` | 没装 g++：`sudo apt install g++` |
| make 报错 `missing separator` | Makefile 命令前用了空格而不是 TAB |
| gdb 看不到变量名 | 编译时没加 `-g` 选项 |
| gdb `run` 报 `No executable file specified` | 启动 gdb 时没指定程序：`gdb ./hello` |

---

## C++ 语法速查（本阶段涉及）

| 语法 | 示例 | 说明 |
|------|------|------|
| `#include <...>` | `#include <iostream>` | 包含系统头文件 |
| `int main()` | `int main(int argc, char* argv[])` | 程序入口 |
| `std::cout` | `std::cout << "hello"` | 标准输出流 |
| `std::endl` | `std::cout << std::endl` | 换行 + 刷新 |
| `std::string` | `std::string s = "hi"` | C++ 字符串类 |
| `#ifdef / #endif` | `#ifdef DEBUG` | 条件编译 |

---

## 阶段小结

你完成了：

- ✅ Linux C++ 开发环境搭建
- ✅ 理解了 g++ → make → cmake 的演进逻辑
- ✅ 能独立写 CMakeLists.txt 和 Makefile
- ✅ 掌握了 gdb 的基本调试操作（断点、单步、查看变量、调用栈）

这些工具将在后续每个阶段反复使用。**不要跳过 gdb 练习**——后续碰到段错误（Segmentation Fault）和逻辑 bug 时，gdb 是你最强大的武器。

下一阶段：**Phase 1 — 线程同步原语**——用 C++ 封装 pthread 的互斥锁、信号量、条件变量，并实现生产者-消费者模型。
