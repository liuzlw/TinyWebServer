# 附录 A：make 与 CMake —— 吃透构建工具链

> **什么时候读**：完成 [Stage 2](stage-02-socket-echo.md) 之后随时可读；[Stage 9](stage-09-integration.md) 结束前**必读**。
> **解决什么问题**：从"只会照着敲 g++"到"看得懂 makefile、能自己写 CMakeLists.txt"。本附录会逐行拆解本项目的 `makefile`，教你用它改进出增量编译，再用 CMake 构建出**同一份** `server`。
> 全程命令在 Ubuntu 22.04（g++ 11、GNU make 4.x、CMake 3.22+）下验证。学习时请把命令敲到自己的 `my_tiny_webserver/` 工作区里，仓库只作参考答案。

## 前置要求

- 完成 [Stage 2](stage-02-socket-echo.md)（会用 g++ 编译单个/多个文件、会运行 echo 服务器）。
- 已安装 `g++`、`make`、`cmake`（Stage 0 已装）。
- 已安装 `libmysqlclient-dev` 与 `pkg-config`（[Stage 8](stage-08-mysql.md) 会装；若还没做到 Stage 8，现在补装：`sudo apt install libmysqlclient-dev pkg-config`）。
- 你的工作区里已经有本项目的全部源文件（至少 7 个 `.cpp` 与对应头文件），目录结构如下：

```text
my_tiny_webserver/
├── main.cpp
├── config.cpp  config.h
├── webserver.cpp  webserver.h
├── http/http_conn.cpp  http/http_conn.h
├── timer/lst_timer.cpp  lst_timer.h
├── log/log.cpp  log.h  block_queue.h
├── lock/locker.h
├── CGImysql/sql_connection_pool.cpp  sql_connection_pool.h
└── makefile
```

> 说明：本项目参与编译的源文件一共 **7 个** `.cpp`（`main.cpp`、`config.cpp`、`webserver.cpp`、`http/http_conn.cpp`、`timer/lst_timer.cpp`、`log/log.cpp`、`CGImysql/sql_connection_pool.cpp`）。下文所有命令都基于这 7 个文件。

## 一、为什么需要构建工具

先直观感受一下"没有构建工具"是什么体验。本项目要生成 `server`，手动敲的 g++ 命令长这样（在项目根目录执行）：

```bash
g++ -o server \
    main.cpp config.cpp webserver.cpp \
    http/http_conn.cpp timer/lst_timer.cpp \
    log/log.cpp CGImysql/sql_connection_pool.cpp \
    -lpthread -lmysqlclient
```

这条命令有三大痛点：

1. **太长、太容易错**：7 个源文件路径 + 2 个库，少写一个 `-lpthread` 或拼错一个路径，就是一大串报错。
2. **每次全量重编译**：哪怕只改了 `main.cpp` 一个字母，上面这条命令也会把 7 个文件**全部重新编译一遍**。7 个文件要几秒钟；当工程变成几十上百个文件时，每次改一行代码都要等几十秒。
3. **不可复用、难交接**：别人拿到你的项目，不知道编译命令是什么、参数是什么、依赖什么库。

`make` 和 CMake 就是为了消灭这三个痛点：**把"怎么编译"写进一个文件，一条命令完成构建，只重编译真正改过的文件**。本项目里，这一步被简化成了：

```bash
make        # 等价于 sh ./build.sh
```

## 二、g++ 编译选项速览

先弄清构建工具背后到底在调什么。g++ 一次编译的完整流程是：预处理 → 编译 → 汇编 → 链接。下表是本项目用到的选项：

| 选项 | 含义 | 本项目是否用到 |
|---|---|---|
| `-c` | 只编译/汇编到目标文件 `.o`，**不链接** | 进阶 makefile 会用到 |
| `-o file` | 指定输出文件名（否则默认 `a.out`） | 用到（`-o server`） |
| `-g` | 生成调试信息（给 gdb 用，见 [附录 B](appendix-b-gdb.md)） | makefile 的 DEBUG 分支 |
| `-O0` | 不优化，调试最友好（g++ 默认就是 `-O0`） | 默认 |
| `-O2` | 二级优化，生成更快/更小的代码，但变量可能被优化掉 | makefile 的 release 分支 |
| `-Wall` | 打开常见警告（"warning" 全家桶） | 仓库没开，进阶版补上 |
| `-I 目录` | 额外头文件搜索目录（`#include <...>` 找不到时用） | 本项目用相对路径 `#include "..."`，所以**不需要** |
| `-L 目录` | 额外库文件搜索目录 | 不需要（库在系统默认路径） |
| `-l名字` | 链接库 `lib名字.so`（如 `-lmysqlclient` → `libmysqlclient.so`） | 用到 |
| `-lpthread` | 链接 POSIX 线程库 `libpthread` | 用到（线程池） |
| `-lmysqlclient` | 链接 MySQL 客户端库 `libmysqlclient` | 用到（数据库连接池） |
| `-std=gnu++11` | 指定 C++ 标准（`gnu++11` = C++11 + GNU/POSIX 扩展；本项目用到 `bzero`/`strcasecmp` 等 POSIX 函数，必须用 `gnu` 变体，严格的 `-std=c++11` 会把这些函数藏起来导致编译报错） | 进阶版补上 |

几个容易混淆的点，单独说清楚：

- **`-I`/`-L`/`-l` 的区别**：`-I` 管**头文件**（编译阶段找 `.h`），`-L` 管**库搜索路径**，`-l` 管**具体链接哪个库**。本项目头文件用相对路径 include（如 `#include "./threadpool/threadpool.h"`），MySQL 头文件 `<mysql/mysql.h>` 又在系统默认路径，所以一个 `-I` 都不用加。
- **静态库 vs 动态库**：`libmysqlclient.so` 是**动态库**（`.so`，运行期加载），静态库是 `.a`（链接期直接打进可执行文件）。本项目链接的是动态库，所以运行时机器上也要有它。用 `ldd` 能直接看到依赖了哪些动态库：

```bash
ldd ./server
```

```text
    linux-vdso.so.1 (0x00007ffd8a5fb000)
    libmysqlclient.so.21 => /lib/x86_64-linux-gnu/libmysqlclient.so.21 (0x00007f...)
    libstdc++.so.6 => /lib/x86_64-linux-gnu/libstdc++.so.6 (0x00007f...)
    libgcc_s.so.1 => /lib/x86_64-linux-gnu/libgcc_s.so.1 (0x00007f...)
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f...)
    /lib64/ld-linux-x86-64.so.2 (0x00007f...)
```

> **注意**：Ubuntu 22.04 的 glibc（≥ 2.34）已经把 `libpthread` 合并进了 `libc`，所以你看不到单独的 `libpthread.so.0` 一行——这是**正常现象**，不代表 `-lpthread` 漏了。重点是确认 `libmysqlclient.so.21` 出现了。

## 三、make 原理

`make` 读当前目录下的 `makefile`（或 `Makefile`），根据里面的**规则**决定编译哪些文件。核心只有三个概念。

### 3.1 规则：目标、依赖、命令

一条规则的格式：

```makefile
目标: 依赖1 依赖2
	命令
```

- **目标（target）**：要生成的文件名（如 `server`、`main.o`）。
- **依赖（prerequisites）**：生成目标需要哪些文件。
- **命令（recipe）**：怎么从依赖生成目标。**这一行必须以一个 Tab 开头**（不是空格！这是新手第一大坑）。

例如：

```makefile
main.o: main.cpp config.h
	g++ -c main.cpp -o main.o
```

含义：`main.o` 依赖 `main.cpp` 和 `config.h`，用 `g++ -c` 生成它。

### 3.2 时间戳判断：为什么只重编改过的文件

make 决定是否执行命令，只看一件事——**时间戳**：

1. 如果 `main.o` 不存在 → 执行命令生成它；
2. 如果 `main.o` 存在，但比它的**任一依赖**（`main.cpp` 或 `config.h`）更旧 → 执行命令重新生成；
3. 如果 `main.o` 比所有依赖都新 → 跳过，打印一句 `make: 'main.o' is up to date.`。

这就是"增量编译"的本质。用 `touch` 亲手验证一下（`touch` 只把文件的修改时间改成"现在"，不改变内容）：

```bash
touch main.cpp     # 假装"改了" main.cpp
make               # 因为 main.cpp 比 main.o 新，会重新编译 main.o
make               # 再执行一次：这次什么都不做
```

```text
g++ -c main.cpp -o main.o
make: 'server' is up to date.
```

### 3.3 变量：`=` `:=` `?=` `+=`

| 写法 | 含义 | 展开时机 |
|---|---|---|
| `A = 1` | 递归展开：用到 `A` 时才展开，后定义的会覆盖 | 惰性（可能被后续覆盖） |
| `A := 1` | 立即展开：赋值那一刻就定死 | 立即 |
| `A ?= 1` | 仅当 `A` 尚未定义时才赋值（"缺省值"） | 立即 |
| `A += x` | 追加（等价于在原来的值后面接上 `x`） | 立即 |

看两个关键区别：

```makefile
A = 1
B = $(A)     # B 此刻只是"记录"了 $(A)，尚未展开
A = 2
# 此时 $(B) 会展开成 2，因为 $(A) 是惰性展开的

C := 1
D := $(C)    # D 立刻被展开成 1
C := 2
# 此时 $(D) 仍是 1
```

`?=` 的典型用途就是本仓库 makefile 的 `CXX ?= g++`：**如果你不指定就用 g++，指定了就用你的**。比如 `make CXX=clang++` 就改用 clang++ 编译。

> 补充：`+=` 在本项目里配合 `?=` 使用——`CXXFLAGS += -g` 是"在 CXXFLAGS 后面追加 `-g`"。注意 `CXXFLAGS` 是 make 的**内置变量**（g++ 编译 C++ 时的选项），make 知道用它拼出默认的编译命令，这也是下一节要讲的"内置规则"。

### 3.4 自动变量：`$@` `$<` `$^`

写规则时，目标和依赖的名字很长、还容易重复。自动变量就是"代称"：

| 变量 | 含义 |
|---|---|
| `$@` | 当前**目标**名 |
| `$<` | 第一个**依赖**名 |
| `$^` | **所有**依赖名（去重后，空格分隔） |

比如仓库 makefile 里的这一行：

```makefile
server: main.cpp ./timer/lst_timer.cpp ... config.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient
```

`$^` 会被展开成 `main.cpp ./timer/lst_timer.cpp ... config.cpp` 这一整串依赖列表，于是命令等价于：`g++ -o server main.cpp ./timer/lst_timer.cpp ... config.cpp -lpthread -lmysqlclient`。目标名 `server` 因为固定，这里直接写死了（当然也能写成 `-o $@`）。

### 3.5 内置规则与隐式编译

make 内置了一大批"默认规则"，其中最有名的一条是：

```makefile
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@
```

含义：**任何** `xxx.o` 都能由同名的 `xxx.cpp` 编译而来。所以只要你设好了 `CXX`、`CXXFLAGS`，下面这条：

```makefile
main.o: main.cpp config.h
```

make 会自动用内置规则补出命令 `g++ $(CXXFLAGS) -c main.cpp -o main.o`，你甚至不用自己写命令。这就是为什么 `CXXFLAGS` 有用——它被内置规则引用。仓库的 makefile **没用**内置规则（它绕过了 `.o`，直接一次性编译所有 `.cpp`），下一节细说。

### 3.6 伪目标 `.PHONY` 与 `clean`

`clean` 不是要生成的文件，而是一个"动作名"。如果目录里恰好有个文件叫 `clean`，`make clean` 会误判"已是最新"而跳过。用 `.PHONY` 声明它是伪目标，就能强制每次执行：

```makefile
.PHONY: clean
clean:
	rm -f server $(OBJS)
```

另外，`make` 不带参数时默认执行**第一个目标**。所以把最常用的目标（如 `server`）写在最前面。

## 四、逐行解析仓库 makefile

这是仓库根目录 `makefile` 的完整内容（共 15 行）：

```makefile
CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp  ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp  webserver.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server
```

逐行拆解：

- **第 1 行 `CXX ?= g++`**：编译器变量，缺省 `g++`，可用命令行覆盖（`make CXX=clang++`）。
- **第 3 行 `DEBUG ?= 1`**：调试开关，缺省 1（调试模式）。
- **第 4 行 `ifeq ($(DEBUG), 1)`**：条件判断。`$(DEBUG)` 等于 `1` 时走 `-g` 分支，否则走 `-O2` 分支。`ifeq (参数1, 参数2)` 比较两个参数是否相等。
- **第 5 行 `CXXFLAGS += -g`**：调试模式 → 加 `-g` 生成调试信息。
- **第 7 行 `CXXFLAGS += -O2`**：发布模式 → 加 `-O2` 优化。所以想发正式版，就 `make DEBUG=0`。
- **第 9 行 `endif`**：结束条件块（第 8 行的空行是 `else` 分支的结尾留白，无实际作用）。
- **第 11 行 `server: ... 7 个 .cpp`**：目标 `server` 的依赖**直接是 7 个源文件**，没有 `.o` 中间产物。
- **第 12 行**：命令，`$^` 展开成全部 7 个源文件，一次 g++ 完成"编译 + 链接"（等价于第一节那条手动命令）。
- **第 14~15 行 `clean: rm -r server`**：删除可执行文件 `server`。

### 它最大的局限：全量重编译

看第 11~12 行：目标 `server` 依赖的是 **7 个 `.cpp` 源文件**，而不是 `.o`。这带来两个后果：

1. **没有增量编译**：只要 7 个 `.cpp` 里**任意一个**比 `server` 新（甚至 `touch` 一下），`make` 就重跑第 12 行，把 7 个文件**全部重编一遍**。
2. **没有利用内置规则**：因为根本没生成 `.o`，make 的 `.o` 增量机制完全用不上。

自己验证一下"全量"有多明显：

```bash
make
make          # 再执行一次
```

```text
g++ -o server main.cpp ./timer/lst_timer.cpp ... -lpthread -lmysqlclient
g++ -o server main.cpp ./timer/lst_timer.cpp ... -lpthread -lmysqlclient   # 第二次仍然全量重编！
```

第二条命令本应打印 `make: 'server' is up to date.`（因为 7 个源文件都没变、`server` 比它们都新）。但只要**改任何一个源文件**，就会重编全部 7 个。下一节我们把 `server` 的依赖改成 `.o`，就能真正做到"改谁编谁"。

## 五、改进：带 `.o` 与模式规则的进阶 makefile

把目标 `server` 的依赖从 `.cpp` 换成 `.o`，再补一条模式规则，就得到了增量编译版本：

```makefile
CXX      ?= g++
DEBUG    ?= 1

# 补上仓库漏掉的通用选项
# 注意用 gnu++11 而不是严格的 c++11：本项目用了 bzero/strcasecmp 等 POSIX 函数
CXXFLAGS += -Wall -std=gnu++11

ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

# 源文件清单（换成你自己的路径即可）
SRCS = main.cpp config.cpp webserver.cpp \
       http/http_conn.cpp timer/lst_timer.cpp \
       log/log.cpp CGImysql/sql_connection_pool.cpp

# 把每个 xxx.cpp 换成 xxx.o（替换引用 $(SRCS:.cpp=.o)）
OBJS = $(SRCS:.cpp=.o)

# 最终目标：依赖变成一堆 .o
server: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(CXXFLAGS) -lpthread -lmysqlclient

# 模式规则：任意 xxx.o 由 xxx.cpp 编译而来（覆盖内置规则，显式写清楚）
%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $< -o $@

clean:
	rm -f server $(OBJS)

.PHONY: clean
```

对比仓库版本，改动只有三处，但意义重大：

1. `SRCS` / `OBJS` 两个变量集中管理文件清单，增删文件只改一处；
2. `server` 依赖 `.o`，`%.o: %.cpp` 负责逐个编译——**改一个源文件，只重编它对应的 `.o`，再链接一次**；
3. 补了 `-Wall -std=gnu++11`，并给 `clean` 加了 `-f` 和 `.PHONY`（更健壮）。

亲手验证增量编译（这是本附录最该亲自做一遍的实验）：

```bash
make                 # 第一次：编译 7 个 .o + 链接
touch webserver.cpp  # 假装只改了 webserver.cpp
make                 # 第二次：应该只重编 webserver.o，再链接
```

第二次的输出**应该只有**：

```text
g++ -c -Wall -std=gnu++11 -g webserver.cpp -o webserver.o
g++ -o server main.o config.o webserver.o http/http_conn.o timer/lst_timer.o log/log.o CGImysql/sql_connection_pool.o -Wall -std=gnu++11 -g -lpthread -lmysqlclient
```

看到没：`main.o`、`config.o` 等其余 6 个 `.o` **没有被重编**，只有 `webserver.o` 重编了，最后链接一次成 `server`。这就是增量编译。

> 小提示：进阶版把 `.o` 直接生成在源文件旁边（如 `http/http_conn.o`）。更工程化的做法是用 `build/` 目录隔离中间产物，但那样模式规则要写得更复杂，这里先用简单版把原理吃透。另一个已知局限：本进阶版只把 `.cpp` 列为依赖，**改头文件（如 `http_conn.h`）不会触发重编译**。想补全可以给每条规则加头文件依赖（如 `http/http_conn.o: http/http_conn.cpp http/http_conn.h`），或用 g++ 的 `-MMD -MP` 自动生成依赖文件——后者是大型工程的通用做法，可作为练习 4 的延伸。

## 六、build.sh 一行解析

仓库的 `build.sh` 全文只有三行：

```bash
#!/bin/bash

make server
```

- `#!/bin/bash`：声明用 bash 解释本脚本（shebang）。
- `make server`：执行 makefile 里的 `server` 目标。

所以 `sh ./build.sh` 和 `make server`、`make`（server 是第一个目标，默认执行）完全等价。写这个脚本只是为了"一条命令、一看就懂"。

## 七、CMake 入门

make 已经够用了，为什么还要 CMake？因为 makefile 是**面向单个平台**的（本项目只有 Linux 能跑，所以 make 够用）；一旦要跨平台、要生成 IDE 工程、要管理复杂依赖，makefile 会越来越难维护。CMake 的思路是**"写一份 CMakeLists.txt，生成各平台的构建脚本"**——在 Linux 上它默认生成 makefile，再交给 make 去执行。

### 7.1 基本语法

一个最小 CMakeLists.txt：

```cmake
# 最低 CMake 版本要求（本项目用 3.10 已足够；Ubuntu 22.04 自带 3.22）
cmake_minimum_required(VERSION 3.10)

# 工程名与语言
project(TinyWebServer CXX)

# 用 C++11（CMAKE_CXX_EXTENSIONS 默认 ON，等价 gnu++11，POSIX 函数可用）
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 可执行文件 server，由这些源文件生成
add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

# 链接库（pthread 和 mysqlclient）
target_link_libraries(server PRIVATE pthread mysqlclient)
```

逐个语法点：

> 关于标准的小坑：`set(CMAKE_CXX_STANDARD 11)` 配合默认的 `CMAKE_CXX_EXTENSIONS ON`，实际传给 g++ 的是 `-std=gnu++11`（带 GNU 扩展），因此 `strcasecmp`、`bzero` 等 POSIX 函数可用。若你显式 `set(CMAKE_CXX_EXTENSIONS OFF)`，会变成严格的 `-std=c++11`，本项目会因找不到这些函数而编译报错——与第二节表格里的说明是同一回事。

| 指令 | 作用 |
|---|---|
| `cmake_minimum_required(VERSION x.y)` | 声明最低 CMake 版本（写 3.10 就能覆盖绝大多数机器） |
| `project(名字 语言)` | 工程名与语言（`CXX` 表示 C++） |
| `add_executable(名字 源文件...)` | 生成一个可执行文件 |
| `target_link_libraries(名字 可见性 库...)` | 给目标链接库；`PRIVATE` 表示"只给这个目标用" |
| `option(名字 描述 默认值)` | 定义一个可开关的布尔选项 |
| `find_package(包 必需?)` | 查找外部库（见 8.3） |
| `set(变量 值)` | 设置变量 |

`option` 的典型用法（本项目也能用上）：

```cmake
option(ENABLE_DEBUG "Build with debug info" ON)
if(ENABLE_DEBUG)
    target_compile_options(server PRIVATE -g)
else()
    target_compile_options(server PRIVATE -O2)
endif()
```

### 7.2 out-of-source 构建惯例

CMake 强烈建议"源码与构建产物分离"——不要在源码目录里直接跑 cmake，而是建一个 `build` 目录：

```bash
cmake -S . -B build     # -S 指定源码目录，-B 指定构建目录
cmake --build build     # 等价于进 build 目录执行 make
```

```text
-- The CXX compiler identification is GNU 11.4.0
-- Detecting CXX compiler ABI info - done
-- Configuring done
-- Generating done
-- Build files have been written to: .../build
```

这样所有中间文件、`server` 都落在 `build/` 里，`rm -rf build` 一键清干净，源码目录干干净净。

### 7.3 cmake 与 make 的关系

一句话：**cmake 是"生成器"，make 是"执行器"**。

```text
CMakeLists.txt  --(cmake)-->  build/Makefile  --(make)-->  server
```

`cmake -S . -B build` 这一步在 `build/` 里生成了一份 makefile；`cmake --build build` 只是帮你调用了那个目录里的 `make`。你完全可以 `cd build && make`，效果一样。

## 八、给本项目写完整 CMakeLists.txt

下面给两个版本，都能构建出和 make 一模一样的 `server`。**版本一**最简单直接；**版本二**更规范（用 `find_package` 查找 MySQL），并处理 MySQL 8 下 `find_package` 可能失败的问题。

### 8.1 版本一：手动链接 `pthread mysqlclient`

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

# 直接写库名：pthread -> -lpthread，mysqlclient -> -lmysqlclient
target_link_libraries(server PRIVATE pthread mysqlclient)
```

构建并验证：

```bash
cmake -S . -B build_v1
cmake --build build_v1
./build_v1/server -p 9007 -c 1 &
curl -s http://127.0.0.1:9007/ | head -n 3
kill %1
```

说明：`target_link_libraries` 里的 `pthread`、`mysqlclient` 是不带 `-l` 的裸名字，CMake 会自动拼成 `-lpthread -lmysqlclient`。`PRIVATE` 表示这两个库只被 `server` 使用，不向下游传播。

> 更规范的线程写法是用 `find_package(Threads REQUIRED)` + `Threads::Threads`（跨平台、语义清晰），版本二会用它。

### 8.2 版本二：`find_package(MySQL)` 规范查找

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 线程库：跨平台标准写法
find_package(Threads REQUIRED)

# 查找 MySQL 客户端库（头文件 + libmysqlclient）
find_package(MySQL REQUIRED)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

# MySQL 的头文件路径 + 库，都由 find_package 找好
target_include_directories(server PRIVATE ${MySQL_INCLUDE_DIRS})
target_link_libraries(server PRIVATE Threads::Threads ${MySQL_LIBRARIES})
```

构建：

```bash
cmake -S . -B build_v2
cmake --build build_v2
```

`find_package(MySQL)` 成功后会设置 `MySQL_INCLUDE_DIRS`（`mysql.h` 所在目录）和 `MySQL_LIBRARIES`（要链接的库）。之所以要显式 `target_include_directories`，是因为 `#include <mysql/mysql.h>` 需要找到 MySQL 头文件。

### 8.3 MySQL 8 下 `find_package` 失败的备选方案

Ubuntu 22.04 默认是 MySQL 8.0。`find_package(MySQL)` 是老模块，某些系统上会**找不到库**或误找到 MariaDB，报错类似：

```text
CMake Error at /usr/share/cmake-3.22/Modules/FindMySQL.cmake:... (message):
  Could not find MySQL
```

原因通常是：只装了客户端库 `libmysqlclient-dev`，或模块在找已删除的 `libmysqlclient_r`（MySQL 8 移除了线程安全变体）。遇到这种情况，按优先级用下面任一备选（都能得到同一份 `server`）：

**备选 A（推荐）：pkg-config 方式**，最稳：

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# 用 pkg-config 查找 mysqlclient（libmysqlclient-dev 提供 mysqlclient.pc）
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED IMPORTED_TARGET mysqlclient)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

target_link_libraries(server PRIVATE Threads::Threads PkgConfig::MYSQL)
```

`PkgConfig::MYSQL` 这个导入目标同时带上了头文件路径和 `-lmysqlclient`，一行搞定。前提是装了 `pkg-config` 和 `libmysqlclient-dev`（`mysqlclient.pc` 由后者提供）。

**备选 B：直接链接 `-lmysqlclient`**（最简单粗暴，退回到版本一的手动写法）：

```cmake
target_link_libraries(server PRIVATE Threads::Threads mysqlclient)
```

因为头文件 `/usr/include/mysql` 在系统默认搜索路径里，多数情况连 `target_include_directories` 都不用加；加一句更保险：

```cmake
target_include_directories(server PRIVATE /usr/include/mysql)
```

> 小结：**能跑通 `find_package(MySQL)` 就用它；跑不通就换 pkg-config；再不行就 `mysqlclient` 裸名直接链接。**三种写法最终生成的 `server` 功能完全一致，差异只在"查找库的方式"。

### 8.4 构建出同一份 server 并运行验证

无论哪个版本，构建产物都是 `server`。验证它和 make 产物等价：

```bash
# 对比两个可执行文件（大小/依赖应一致或接近）
ls -l server build_v2/server
ldd build_v2/server | grep mysql
```

```text
-rwxrwxr-x 1 you you 274152 ... server
-rwxrwxr-x 1 you you 265384 ... build_v2/server
    libmysqlclient.so.21 => /lib/x86_64-linux-gnu/libmysqlclient.so.21 (...)
```

> 两个可执行文件大小略有差异是**正常**的：make 默认带 `-g`（调试信息），而上面的 CMakeLists 没加 `-g`，文件自然更小。功能、动态库依赖完全一致；若想严格对齐，在 CMake 里同样开 `-g` 即可。

> 运行验证需要 MySQL 已启动并建好库表（见 [Stage 8](stage-08-mysql.md)）。若无 MySQL，`server` 会在初始化连接池时 `exit(1)`——这不影响本附录"构建"部分的验收。

## 九、make vs CMake 对比

| 维度 | make | CMake |
|---|---|---|
| 写什么 | 手写 makefile（规则、依赖、命令） | 写声明式的 CMakeLists.txt |
| 编译产物 | 直接调 g++ | 先生成 makefile/工程，再交给 make 编译 |
| 跨平台 | 弱（本项目本身只跑 Linux） | 强（一份配置生成 Linux/Windows/macOS 工程） |
| 依赖查找 | 手动写 `-lxxx`、`-I` | `find_package` / `pkg_check_modules` 自动找 |
| 上手难度 | 低（规则、变量、自动变量就够） | 中（语法更抽象，但更省心） |
| 适合场景 | 单平台小项目、教学 | 多平台、多依赖、多人协作 |

**本项目结论**：仓库用 make 完全够用（单平台、文件不多、依赖只有 pthread + mysqlclient）；但你若要把项目改造成 CMake 版（更工程化、便于扩展），用第八节任一个版本即可，产物等价。

---

## 验收清单

把下面每一行都亲手跑通再打勾：

- [ ] **make 构建成功**：在项目根目录执行 `make`（或 `sh ./build.sh`），输出里出现 `g++ -o server ... -lpthread -lmysqlclient` 且无 error；`ls -l server` 能看到可执行文件，`file server` 显示 `ELF 64-bit ... executable`。
- [ ] **make clean 行为正确**：执行 `make clean` 后 `ls server` 报 `ls: cannot access 'server': No such file or directory`；再 `make` 能重新生成 `server`。
- [ ] **DEBUG 开关生效**：先 `make clean && make DEBUG=0`，观察命令里出现 `-O2` 而非 `-g`；再 `make clean && make DEBUG=1`，命令里出现 `-g`。
- [ ] **进阶 makefile 增量编译**：用第五节的进阶 makefile 替换后，`make` 首次编译出 7 个 `.o` + 链接；然后 `touch webserver.cpp && make`，输出**只有** `g++ -c ... webserver.cpp -o webserver.o` 一条编译命令 + 一条链接命令，其余 6 个 `.o` 不重编。
- [ ] **CMake 版本一构建成功**：`cmake -S . -B build_v1 && cmake --build build_v1` 无 error，生成 `build_v1/server`。
- [ ] **CMake 版本二构建成功**：`cmake -S . -B build_v2 && cmake --build build_v2` 无 error，生成 `build_v2/server`（若 `find_package(MySQL)` 失败，改用 8.3 的 pkg-config 或裸名方案后重试，仍然要成功）。
- [ ] **ldd 确认动态依赖**：`ldd ./server` 输出里能看到 `libmysqlclient.so.21`（以及 `libstdc++.so.6`、`libc.so.6`；看不到 `libpthread.so.0` 是正常的，见第二节说明）。
- [ ] **运行验证（需 MySQL）**：`./server -p 9007 -c 1 &` 启动后，`curl -s http://127.0.0.1:9007/ | head -n 5` 能返回 `judge.html` 的 HTML（含 `<html>` 等标签）；结束后 `kill %1` 停掉后台进程。

## 常见问题

1. **`make: *** missing separator. Stop.`（或 `Makefile:N: *** missing separator`）**：命令行的缩进用了**空格**而不是 **Tab**。make 只认 Tab 开头的命令行。把 `g++ ...` 那行的空格删掉、敲一个 Tab；编辑器里别把 Tab 转成空格。

2. **改了 makefile 里的变量却不生效**：注意 `=`/`:=`/`?=`/`+=` 的区别，以及 `?=` 只在"未定义"时生效。命令行传入的值优先级最高：`make CXXFLAGS=-O2` 会覆盖 makefile 里的 `CXXFLAGS += -g`（因为命令行变量优先于 makefile 里的普通赋值）。

3. **`make: 'server' is up to date.` 但我明明改了文件**：多半是时间戳问题——比如从别处拷贝文件导致源文件时间比 `server` 旧。`touch 源文件` 或 `make clean` 后重编即可。

4. **CMake 报 `Could not find MySQL` / `Could NOT find MySQL`**：先确认 `sudo apt install libmysqlclient-dev pkg-config` 已装；再按 8.3 节改用 pkg-config（`pkg_check_modules(... mysqlclient)`）或直接 `target_link_libraries(... mysqlclient)`。注意包名是 **libmysqlclient-dev**，不是 `libmysql`。

5. **链接报一堆 `undefined reference to 'pthread_create'` / `'mysql_real_connect'`**：漏了 `-lpthread` 或 `-lmysqlclient`。链接选项要放在**源文件之后**（gcc 按出现顺序解析符号）。makefile 里确认 `-lpthread -lmysqlclient` 在命令末尾；CMake 里确认 `target_link_libraries` 里写了 `pthread`/`mysqlclient`。

6. **`-g` 和 `-O2` 混用导致调试对不上**：`-g` 用于调试、`-O2` 用于优化，两者可以并存但**不要同时开**——优化会重排代码、把变量优化掉，gdb 里出现 `<optimized out>`、断点行号乱跳。调试时用 `-g`（或加 `-O0`），发布时用 `-O2`。本项目 makefile 的 if/else 正是为了二选一。

7. **`sh ./build.sh` 报 `make: command not found`**：`build-essential` 没装全。`sudo apt install build-essential`（它包含 g++ 和 make）。

## 练习

1. **改编译器**：用 `make CXX=clang++` 构建（先 `sudo apt install clang`），对比 g++ 和 clang++ 产出的 `server` 大小与 `ldd` 依赖是否一致。
2. **加一个编译选项**：在进阶 makefile 里追加 `-DDEBUG_LOG`，再写一段 `#ifdef DEBUG_LOG ... #endif` 的代码，验证 `make` 后宏是否生效、`make DEBUG=0` 后是否失效。
3. **加一个源文件**：给项目新增 `util/helper.cpp`（随便写个函数并让 `webserver.cpp` 调用），只改进阶 makefile 的 `SRCS` 一行，验证 `make` 能自动编译新文件、`make clean` 能清掉它。
4. **把 `.o` 挪进 `build/` 目录**：改写进阶 makefile，让所有 `.o` 生成到 `build/obj/` 下（提示：把 `OBJS` 里每个路径前加 `build/obj/`，并调整模式规则的目标路径），验证增量编译仍然只重编改动的那一个。
5. **CMake 加开关**：在版本一 CMakeLists.txt 里加一个 `option(ENABLE_DEBUG ...)`，把 `-g`/`-O2` 的选择交给 cmake，然后用 `cmake -S . -B build -DENABLE_DEBUG=OFF` 验证优化分支生效。
6. **读依赖**：用 `ldd -v build_v2/server` 观察完整的依赖链（含 libmysqlclient 又依赖了哪些库），找一找 `libssl`、`libcrypto` 是从哪儿被间接拉进来的。

---

> 下一步：学会构建后，调试就靠 [附录 B：gdb](appendix-b-gdb.md)。建议两篇连着读。
