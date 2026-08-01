# 附录 A1:原项目 makefile 解读

> 你的复现代码用 CMake 构建,而原项目用 makefile。本附录把原项目的 makefile 逐行讲透,并说清它和 CMake 的对应关系——这样你两种构建方式都懂(目标 3 覆盖 make)。

## 1. 原项目 makefile 全文

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

## 2. 逐行解读

### 变量定义

```makefile
CXX ?= g++
```

- `?=` 是"如果没定义才赋值"。支持在命令行覆盖:`make CXX=clang++` 就能用 clang 编译
- 整份 makefile 用 `$(CXX)` 引用这个变量

```makefile
DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif
```

- `DEBUG` 默认 1
- `ifeq/else/endif` 是 make 的条件语法:`$(DEBUG)` 等于 `1` 就追加 `-g`(带调试信息,能 gdb),否则追加 `-O2`(优化,跑得快)
- `+=` 是追加到已有变量
- **对应 CMake**:`-g` ≈ CMake 的 Debug 构建,`-O2` ≈ Release 构建。CMake 里用 `cmake -DCMAKE_BUILD_TYPE=Debug/Release` 切换

### 目标规则

```makefile
server: main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient
```

**make 规则的核心语法:`目标: 依赖` 下面缩进一行是"命令"。**

- **目标** `server`:要生成的东西(可执行文件)
- **依赖**:生成它需要的源文件(7 个 .cpp)
- **命令**:`$(CXX) -o server $^ ...` —— 把依赖编译链接成 server

`$^` 是一个**自动变量**,代表"所有依赖文件"。所以这条命令等价于:

```bash
g++ -o server main.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp config.cpp -g -lpthread -lmysqlclient
```

**增量编译原理(这是 make 存在的意义):**

```text
make 比较"目标"和"依赖"的时间戳:
  依赖比目标新(某个 .cpp 刚改过)  → 重新执行命令,重新编译
  依赖都比目标旧(什么都没改)      → "server is up to date",什么都不做
```

**对应 CMake**:`add_executable(server main.cpp ... )` + `target_link_libraries(server pthread mysqlclient)` 干的是完全一样的事——列出源文件、链接库。区别是 CMake 多了一层:生成 makefile(或别的构建脚本),再调用 make 真正编译。所以 CMake 是"生成构建规则的工具",make 是"执行构建规则的工具"。

### clean 目标

```makefile
clean:
	rm  -r server
```

- `clean` 是**伪目标**(不是文件,只是命令的名字),删掉编译产物
- 注意这里用了 `rm -r`(按目录删)而不是 `rm -f`(强制删文件)——如果 `server` 不存在,`make clean` 会报错。这是原项目的一个小瑕疵,不影响使用
- **对应 CMake**:`cmake --build build --target clean`,或直接删 `build/` 目录

## 3. make 基础概念速览

| 概念 | 说明 | 例子 |
|---|---|---|
| 目标 | 要生成的东西 | `server:`、`clean:` |
| 依赖 | 目标依赖的文件 | `main.cpp`、`webserver.cpp` |
| 命令 | 怎么生成,**必须用 Tab 缩进** | `$(CXX) -o server $^ ...` |
| 变量 | `VAR = 值`,用 `$(VAR)` 引用 | `CXX ?= g++` |
| 自动变量 | 构建时自动填充 | `$^` 所有依赖、`$@` 目标 |
| 伪目标 | 不是文件的目标 | `clean` |
| `?=` / `+=` | 赋值运算符 | `CXX ?= g++`、`CXXFLAGS += -g` |

**运行方式:**

```bash
make server     # 只构建 server(或直接 make,用第一个目标)
make clean      # 清理
make CXX=clang++   # 覆盖变量
```

## 4. 本教程为什么用 CMake

| | makefile(原项目) | CMake(本教程) |
|---|---|---|
| 你要写的 | 把编译命令写成规则 | 声明"源文件有哪些、要什么库" |
| 跨平台 | 只针对 Linux | 能生成 VS/忍者/Unix Makefiles 等 |
| 学习门槛 | 低,规则直白 | 略高,多一层抽象 |
| 本教程用途 | 附录看懂即可 | 主构建方式(S1~S9) |

> **一句话**:makefile 是"怎么做"的清单,CMake 是"声明要什么"的描述。CMake 最终也会生成 makefile 再去执行。两个都认识,面试聊构建工具就不虚。

## 5. 验证

在你的 `my_tiny_webserver/` 里,如果想把原项目那套 makefile 也用起来(需要 MySQL 就绪):

```bash
cp ../makefile .        # 复制原项目 makefile
make server             # 应该编译出 server
./server -p 9006
make clean              # 清理
```

> 注意:makefile 编译需要 `-lmysqlclient`(MySQL 已装才有)。没装 MySQL 时,用 CMake + 教程的主流程即可。
