# Phase 0 —— 环境搭建与工具入门

## 目标

在本阶段结束时，你将：

1. 拥有一套可用的 Linux C++ 开发环境
2. 理解 g++ → make → cmake 的演进关系
3. 能独立编写 CMakeLists.txt 并完成编译
4. 能用 gdb 设置断点、单步执行、查看变量

**可见结果：** 终端运行 `./build/hello`，输出 `Hello, TinyWebServer!`。

---

## 前置知识

- 会用终端敲命令（cd、ls、mkdir）
- 知道 C++ 的 `main` 函数长什么样
- **不需要** cmake 或 gdb 经验

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **g++** | 手动编译单文件：`g++ main.cpp -o hello` |
| **make** | 手动写 Makefile：目标、依赖、命令三要素 |
| **cmake** | `CMakeLists.txt`：`project`、`add_executable`、`cmake -B build` |
| **gdb** | `break`、`run`、`next`、`step`、`print`、`backtrace`、`quit` |

---

## 分步实现

### Step 1：确认环境

```bash
# 确认 Ubuntu / WSL 版本
lsb_release -a

# 安装编译工具链
sudo apt update
sudo apt install -y build-essential g++ gdb cmake
```

验证安装：

```bash
g++ --version      # 应显示 g++ 11.x 或更高
cmake --version    # 应显示 cmake 3.16 或更高
gdb --version      # 应显示 gdb 10.x 或更高
```

### Step 2：直接用 g++ 编译

创建 `main.cpp`：

```cpp
#include <iostream>

int main() {
    std::cout << "Hello, TinyWebServer!" << std::endl;
    return 0;
}
```

手动编译运行：

```bash
g++ main.cpp -o hello
./hello
# 输出：Hello, TinyWebServer!
```

**g++ 做了什么？** 预处理 → 编译 → 汇编 → 链接，四步合成一条命令。

### Step 3：手写 Makefile

随着文件变多，手动敲 `g++ a.cpp b.cpp c.cpp -o server` 很累。Makefile 解决这个问题：

```makefile
# Makefile
hello: main.cpp
	g++ main.cpp -o hello

clean:
	rm -f hello
```

> **注意：** 命令前面的缩进必须是 **Tab**，不能是空格。

```bash
make           # 等价于 g++ main.cpp -o hello
make clean     # 删除 hello
```

### Step 4：引入 cmake

Makefile 在跨平台时很麻烦（Linux 和 macOS 和 Windows 写法不同）。cmake 是"生成构建系统的系统"：

**CMakeLists.txt：**

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hello main.cpp)
```

**构建：**

```bash
# 方式一：传统两步
mkdir build && cd build
cmake ..
make

# 方式二：cmake 3.13+ 一行搞定
cmake -B build
cmake --build build

# 运行
./build/hello
```

**关键概念：**

- `cmake_minimum_required`：声明 cmake 最低版本
- `project`：项目名、版本、语言
- `set(CMAKE_CXX_STANDARD 11)`：使用 C++11
- `add_executable`：定义可执行目标及其源文件

> **💡 快捷方式：** 本项目根目录提供了 `build.sh` 一键构建脚本，内部等价于执行 `cmake -B build && cmake --build build`。后续阶段可以直接 `sh build.sh` 完成编译。

### Step 5：gdb 调试入门

编译时加 `-g` 保留调试信息：

```cmake
set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_BUILD_TYPE Debug)
```

或直接在 cmake 命令中指定：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

**gdb 基本操作练习：**

```cpp
// debug_demo.cpp
#include <iostream>

int add(int a, int b) {
    int result = a + b;
    return result;
}

int main() {
    int x = 10;
    int y = 20;
    int z = add(x, y);
    std::cout << x << " + " << y << " = " << z << std::endl;
    return 0;
}
```

```bash
# 编译
g++ -g debug_demo.cpp -o debug_demo

# 启动 gdb
gdb ./debug_demo

# 在 gdb 内：
(gdb) break main          # 在 main 函数入口设断点
(gdb) break add           # 在 add 函数入口设断点
(gdb) run                 # 运行程序
(gdb) next                # 单步执行（不进入函数）
(gdb) step                # 单步执行（进入函数）
(gdb) print x             # 查看变量 x 的值
(gdb) print y             # 查看变量 y 的值
(gdb) backtrace           # 查看调用栈
(gdb) continue            # 继续执行到下一个断点
(gdb) quit                # 退出
```

**常用 gdb 命令速记：**

| 缩写 | 全称 | 作用 |
|------|------|------|
| `b` | `break` | 设置断点 |
| `r` | `run` | 开始运行 |
| `n` | `next` | 下一行（不进入函数） |
| `s` | `step` | 下一行（进入函数） |
| `p` | `print` | 打印变量值 |
| `bt` | `backtrace` | 显示调用栈 |
| `c` | `continue` | 继续执行 |
| `q` | `quit` | 退出 |

---

## 验证方法

完成以下全部才算通过 Phase 0：

- [ ] `cmake -B build && cmake --build build` 无报错
- [ ] `./build/hello` 输出 `Hello, TinyWebServer!`
- [ ] 在 gdb 中给 `main` 设断点、单步走完全程
- [ ] 在 gdb 中用 `print` 查看变量值
- [ ] 在 gdb 中用 `backtrace` 查看调用栈

---

## 踩坑记录

1. **cmake 找不到编译器？** 确认 `g++ --version` 正常。如果刚装完，试试 `hash -r` 刷新命令缓存。

2. **Makefile 报 "missing separator"？** 命令前的缩进必须是 Tab，不是四个空格。在 vim 里 `:set list` 可以显示不可见字符。

3. **gdb 里 `print` 显示 `<optimized out>`？** 编译时没加 `-g`，或者开了 `-O2` 优化导致变量被优化掉了。确保用 `-g -O0`。

4. **cmake vs cmake-gui？** 本教程全部用命令行 cmake。图形界面可以以后再说，命令行才是服务器开发的日常。

---

## 阶段小结

至此，你掌握了：
- g++ 编译单文件
- Makefile 管理多文件编译
- cmake 跨平台构建系统
- gdb 基础调试操作

下一阶段，我们将开始写第一个有实际功能的模块：**线程同步原语**（mutex、semaphore、condition_variable）。
