# Stage 0：环境与工具链

> 🎯 **本阶段目标**：配好全部开发环境，用 CMake 编译并用 gdb 调试你的第一个 C++ 程序。
> 之后所有阶段的「地基」就在这里。

## 📚 理论铺垫

### 0.1 为什么必须在 Linux 下开发？

打开原始项目的 `webserver.h`，你会看到这些头文件：

```cpp
#include <sys/socket.h>   // socket 编程
#include <sys/epoll.h>    // epoll：Linux 独有的高性能 I/O 多路复用
#include <netinet/in.h>   // sockaddr_in
```

`epoll` 是 Linux 内核特有的机制，Windows 上没有（Windows 对应物是 IOCP）。
所以我们用 **WSL2（Windows Subsystem for Linux）**：在 Windows 里跑一个真正的 Linux 内核，
代码存在 Windows 磁盘上，编译运行都在 Linux 中进行。

### 0.2 工具链一览

| 工具 | 作用 | 类比 |
|------|------|------|
| g++ | 把 .cpp 编译成可执行文件 | 翻译官 |
| make | 按 Makefile 的规则自动编译 | 流水线工头 |
| CMake | 自动生成 Makefile（跨平台、可维护） | 画施工图的 |
| gdb | 断点调试、查崩溃 | 显微镜 |

一条命令走天下时你不会理解它们的分工，Stage 0 会逐个用一遍。

## 🔨 动手实现

### 0.3 进入 WSL 并验证工具

在 Windows 的 PowerShell 中输入 `wsl` 回车，进入 Ubuntu。之后教程所有命令都在这里执行。

```bash
# 逐个验证（你的环境应该都已经有了）
g++ --version     # 期望：g++ (Ubuntu 13.x ...)
make --version    # 期望：GNU Make 4.x
cmake --version   # 期望：cmake version 3.28.x
gdb --version     # 期望：GNU gdb (Ubuntu) 15.x
```

如果有缺失，一条命令装齐：

```bash
sudo apt update && sudo apt install -y build-essential cmake gdb
```

### 0.4 安装 MySQL（Stage 8 才用，但先装好）

```bash
sudo apt install -y mysql-server libmysqlclient-dev
sudo service mysql start

# 设置 root 密码为 root（与项目默认配置一致，仅限本地学习！）
sudo mysql -e "ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'root'; FLUSH PRIVILEGES;"

# 验证能登录
mysql -uroot -proot -e "SELECT VERSION();"
```

再建好项目需要的数据库和表（这就是 README「快速运行」里的内容）：

```sql
mysql -uroot -proot
```
```sql
CREATE DATABASE yourdb;
USE yourdb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;
INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```

> 💡 WSL2 没有 systemd 的传统自启，**每次新开 WSL 后**如果 MySQL 连不上，
> 先执行 `sudo service mysql start`。

### 0.5 第一个程序：三种方式编译同一个 hello world

在 WSL 中进入项目目录（Windows 的 C 盘挂载在 `/mnt/c`）：

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer/my_tiny_webserver
mkdir -p stage0_hello && cd stage0_hello
```

创建 `hello.cpp`：

```cpp
#include <iostream>

int add(int a, int b) { return a + b; }

int main() {
    int result = add(1, 2);
    std::cout << "1 + 2 = " << result << std::endl;
    return 0;
}
```

**方式一：g++ 直接编译**（理解编译器本身）

```bash
g++ -g -o hello hello.cpp    # -g 生成调试信息（gdb 需要），-o 指定输出文件名
./hello                      # 期望输出：1 + 2 = 3
```

**方式二：Makefile**（理解自动化构建）

创建 `Makefile`（注意：**命令行前必须是 Tab 键，不是空格**）：

```makefile
CXX = g++
CXXFLAGS = -g -Wall

hello: hello.cpp
	$(CXX) $(CXXFLAGS) -o hello hello.cpp

clean:
	rm -f hello
```

```bash
make          # 编译
./hello
make clean    # 清理
```

**方式三：CMake**（现代 C++ 工程的标准做法，也是本教程之后一直用的方式）

创建 `CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(hello CXX)

set(CMAKE_CXX_STANDARD 11)          # 本项目使用 C++11
set(CMAKE_BUILD_TYPE Debug)         # Debug 模式 = -g，方便 gdb 调试

add_executable(hello hello.cpp)
```

```bash
mkdir -p build && cd build
cmake ..       # 生成 Makefile（在 build 目录里，不污染源码目录）
make           # 编译
./hello
cd .. 
```

> 💡 观察：`cmake ..` 之后 build 目录里出现了 `Makefile`。CMake 不直接编译，
> 它是「生成构建文件的构建系统」。想深入理解看 [附录 A](appendix-cmake-make.md)。

### 0.6 用 gdb 调试这个程序

```bash
cd build
gdb ./hello
```

在 gdb 里依次输入：

```gdb
(gdb) break main        # 在 main 函数打断点
(gdb) run               # 运行，会停在断点处
(gdb) next              # 单步执行（不进入函数）
(gdb) step              # 单步执行（进入 add 函数）
(gdb) print a           # 查看变量 a 的值
(gdb) finish            # 跑完当前函数，回到 main
(gdb) continue          # 继续运行到结束
(gdb) quit              # 退出
```

> 你刚刚完成了 gdb 最核心的 6 个命令。更多技巧（多线程调试、core dump）
> 见 [附录 B](appendix-gdb.md)，之后的阶段会反复用到。

## ✅ 验证（本阶段验收清单）

逐项打勾，全部通过才能进入 Stage 1：

- [ ] `g++ --version`、`make --version`、`cmake --version`、`gdb --version` 都有输出
- [ ] 三种方式（g++ / make / CMake）都能编译出 `hello` 并输出 `1 + 2 = 3`
- [ ] 能用 gdb 打断点、单步、查看变量
- [ ] `mysql -uroot -proot -e "SELECT * FROM yourdb.user;"` 能查出 `name | passwd` 这一行

## 🐛 常见问题

**Q1: `make: *** No rule to make target...` 或 Makefile 报 `missing separator`？**
Makefile 的命令行前面必须是 **Tab**，很多编辑器默认把 Tab 转成空格。
用 `cat -A Makefile` 检查：命令行开头显示 `^I` 才是 Tab。

**Q2: CMake 报错找不到编译器？**
`sudo apt install build-essential` 没装全，重新执行 0.3 的安装命令。

**Q3: `mysql -uroot -proot` 提示 `Access denied`？**
WSL 里 MySQL 可能用了 auth_socket 插件。重新执行 0.4 中的 `ALTER USER` 语句。

**Q4: 在 /mnt/c 下编译很慢？**
Windows 磁盘跨系统访问确实慢，学习阶段可接受。如果介意，
可以把 my_tiny_webserver 放到 WSL 自己的文件系统（如 `~/my_tiny_webserver`），
用 VS Code 的 WSL 远程功能编辑。

## 🤔 思考与练习

1. 把 `add(1, 2)` 改成 `add(1)`，重新编译，观察 g++ 的报错信息 —— 学会读编译错误是基本功。
2. 在 gdb 里用 `break add` 打断点，run 之后 `backtrace`（简写 `bt`）查看函数调用栈。
3. 把 CMakeLists.txt 里的 `Debug` 改成 `Release`，重新 cmake + make，
   用 `ls -l hello` 对比两次可执行文件大小，想想为什么不同。

---

➡️ 下一阶段：[Stage 1：socket 与 TCP echo 服务器](stage-01-tcp-echo.md)
