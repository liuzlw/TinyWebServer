# 附录 A：Makefile 与 CMake 详解

> 目标：看懂并能手写本项目的构建脚本，理解编译链接的底层过程。

## 1. 编译的本质：四个阶段

```bash
g++ -o server main.cpp http_conn.cpp ...
```

一条命令背后其实是四步（只对单个文件演示）：

```bash
g++ -E main.cpp -o main.i      # 1. 预处理：展开 #include/#define
g++ -S main.i   -o main.s      # 2. 编译：C++ → 汇编
g++ -c main.s   -o main.o      # 3. 汇编：汇编 → 机器码（目标文件）
g++ main.o http_conn.o ... -o server -lpthread -lmysqlclient   # 4. 链接
```

**为什么报错信息分两派？**
- 语法错误、找不到头文件 → 编译期错误（第 1-3 步）
- `undefined reference to XXX` → 链接期错误：函数声明了但**没人提供实现**
  （忘了把对应 .cpp 加进来，或忘了 `-l` 链接某个库）

常用编译选项：

| 选项 | 作用 | 本项目场景 |
|------|------|-----------|
| `-g` | 生成调试信息 | Debug 构建，gdb 需要 |
| `-O2` | 优化级别 2 | Release 构建（压测用） |
| `-Wall` | 打开常用警告 | 应该常开 |
| `-std=c++11` | 指定语言标准 | 本项目用 C++11 |
| `-I 目录` | 头文件搜索路径 | mysql.h |
| `-l库名` | 链接库 | `-lpthread -lmysqlclient` |
| `-fsanitize=address` | 内存错误检测 | 排查内存问题神器 |

## 2. Makefile：增量构建

### 2.1 本项目的 makefile 逐行解读

原始项目的 `makefile`：

```makefile
CXX ?= g++                          # ?=：如果没被环境变量定义过，就用 g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)                  # 条件判断：DEBUG=1 时加 -g
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

# 目标: 依赖文件列表
server: main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp \
        ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient
#                    ↑ $^ = 所有依赖文件

clean:
	rm -r server
```

要点：

- **目标: 依赖** + Tab 开头的命令 —— 依赖文件比目标新才重新执行命令（增量构建）
- 变量：`$(CXX)`、`$^`（全部依赖）、`$@`（目标名）、`$<`（第一个依赖）
- 带参构建：`make DEBUG=0` → Release 编译

### 2.2 手写一个更好的 Makefile（练习）

上面的写法每次全量重编。进阶版按 .o 分开编译：

```makefile
CXX = g++
CXXFLAGS = -g -Wall -std=c++11
LDFLAGS = -lpthread -lmysqlclient

SRCS = main.cpp config.cpp webserver.cpp \
       http/http_conn.cpp timer/lst_timer.cpp \
       log/log.cpp CGImysql/sql_connection_pool.cpp
OBJS = $(SRCS:.cpp=.o)              # 字符串替换：.cpp → .o

server: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.cpp                          # 模式规则：所有 .o 都由同名 .cpp 生成
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f server $(OBJS)
```

改一个文件只重编它自己 —— 这就是 Makefile 的核心价值。

## 3. CMake：生成构建系统的构建系统

### 3.1 为什么有了 make 还要 CMake？

- Makefile 手写繁琐，跨平台更痛苦（Windows 用 nmake、Mac/Linux 用 make）
- CMake 用一份 `CMakeLists.txt` 生成任意平台的构建文件，还能自动找库、管依赖

流程：`CMakeLists.txt → (cmake) → Makefile → (make) → 可执行文件`

### 3.2 本项目 CMakeLists.txt 逐行解读

```cmake
cmake_minimum_required(VERSION 3.16)     # 最低版本要求
project(my_tiny_webserver CXX)           # 项目名 + 语言

set(CMAKE_CXX_STANDARD 11)               # -std=c++11
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 构建类型：Debug(-g) / Release(-O2)。命令行可覆盖：cmake -DCMAKE_BUILD_TYPE=Release ..
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)     # 找头文件目录
find_library(MYSQL_LIB mysqlclient)            # 找库文件

add_executable(server                      # 目标名 + 源文件列表
    main.cpp config.cpp webserver.cpp
    http/http_conn.cpp timer/lst_timer.cpp
    log/log.cpp CGImysql/sql_connection_pool.cpp
)

target_include_directories(server PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(server pthread ${MYSQL_LIB})
```

### 3.3 标准工作流（背下来）

```bash
mkdir -p build && cd build     # out-of-source 构建：所有产物在 build/ 里
cmake ..                       # 生成 Makefile（只在 CMakeLists 变更后重跑）
make -j$(nproc)                # 并行编译，nproc 核数全开
./server
```

改代码后只需要 `make`；改 CMakeLists 后才需要重新 `cmake ..`。

### 3.4 常用 CMake 变量速查

| 需求 | 写法 |
|------|------|
| 加编译选项 | `add_compile_options(-Wall -Wextra)` |
| Debug 才加的选项 | `target_compile_options(server PRIVATE $<$<CONFIG:Debug>:-fsanitize=address>)` |
| 打印变量调试 | `message(STATUS "MYSQL_LIB=${MYSQL_LIB}")` |
| 子目录模块 | `add_subdirectory(http)` + 各目录自己的 CMakeLists |

### 3.5 Makefile vs CMake 怎么选？

- 几个文件的小玩具：g++ 或简单 Makefile
- 多人协作、跨平台、要长期维护的正式项目：**CMake**（事实标准）
- 读别人的老项目：两个都得看得懂

## 🤔 练习

1. 给本项目手写 2.2 那样的增量 Makefile，故意改一个 .cpp，`make` 观察只重编了一个 .o。
2. 在 CMake 中加 `-fsanitize=address`（编译和链接都要加），跑一遍服务器，
   让 ASan 帮你找内存问题。
3. 执行 `make VERBOSE=1`，观察 CMake 生成的实际编译命令。
