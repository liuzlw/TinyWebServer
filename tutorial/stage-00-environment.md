# Stage 0 环境准备

> 本教程的起点。做完这一步,你的开发环境能编译、能调试、能连数据库——后面所有阶段都基于此。

## 1. 本阶段目标

- [ ] WSL2 环境检查通过(g++ / cmake / gdb / git)
- [ ] 用 CMake 构建并运行第一个 C++ 程序
- [ ] 会用 gdb 打断点、单步、看变量
- [ ] 装好 MySQL 并建好项目要用的库和表
- [ ] 跑通"验收清单"里全部 5 条

## 2. 前置知识

无。这是起点,你只需要装好 Windows 11 + WSL2(不会装?见下文第 3 节)。

---

## 3. WSL2 与工具链检查

### 3.1 打开 WSL2

在 Windows 里打开 PowerShell 或 CMD,输入:

```bash
wsl --status
```

如果还没装 WSL,先执行一次:

```bash
wsl --install
```

装完重启,再打开 Ubuntu 终端。本教程假设你使用的是 **Ubuntu 24.04**。

### 3.2 检查工具链

在你的 **WSL Ubuntu 终端**里逐条运行(不是 Windows 终端!):

```bash
g++ --version
cmake --version
gdb --version
git --version
mysql --version
```

**预期输出**(版本号 >= 下面这些即可):

```text
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
cmake version 3.28.3
GNU gdb (Ubuntu 14.2) ...
git version 2.43.0
mysql  Ver 8.0.39 ...   ← 如果这行报"command not found",说明 MySQL 没装,见第 6 节
```

**如果你的 g++/cmake/gdb 没装或版本太低**,执行:

```bash
sudo apt update
sudo apt install -y build-essential cmake gdb git
```

> ⚠️ **重要**:`g++ --version` 至少要 11.0。本教程的代码需要 C++11 及以上支持,老版本编译会报错。
>
> ⚠️ 如果 `g++` 报 `command not found`,通常是缺 `build-essential`,上面一条命令就能装齐。

### 3.3 确认项目位置 + 建个短路径软链接

原仓库代码在 Windows 的 `C:\Users\liuzl\Documents\projects\TinyWebServer`,在 WSL 里对应路径是:

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer
ls
```

你会看到 `main.cpp`、`webserver.cpp`、`http/`、`threadpool/` 等——这就是我们要复现的目标(也是你的"参考答案")。

**建议现在建一个软链接 `~/TinyWebServer` 指向仓库**,后面所有阶段都用 `~/TinyWebServer/...` 这个短路径(不用每次敲一长串 `/mnt/c/...`):

```bash
ln -s /mnt/c/Users/liuzl/Documents/projects/TinyWebServer ~/TinyWebServer
cd ~/TinyWebServer
ls my_tiny_webserver        # 应该能看到 lock/ threadpool/ http/ timer/ log/ CGImysql/ root/ 空骨架目录
```

> 如果提示 `my_tiny_webserver` 不存在(比如克隆的仓库没带骨架),先手动创建,效果一样:
> ```bash
> mkdir -p my_tiny_webserver/{lock,threadpool,http,timer,log,CGImysql,root}
> ```

> 软链接只是仓库的"快捷方式",读写的是同一个物理目录——你的复现代码写进 `my_tiny_webserver/` 后,Windows 侧的 Git 也能看到。后续所有阶段的 `cd ~/TinyWebServer/my_tiny_webserver` 都建立在这一步之上。

> ⚠️ **关于在 `/mnt/c` 上构建**:`/mnt/c` 是 Windows 的 drvfs 挂载,在上面跑 CMake 会明显偏慢,偶尔还有文件锁/权限报错。本教程默认就在这里工作(方便 Windows 侧 Git 直接看到),大多数情况下没问题。如果构建频繁报错或慢得受不了,可以改用"工作副本"方案:把仓库复制到 Linux 家目录里构建,做完再把成果拷回仓库(见第 8 节常见坑的说明)。两种方案二选一,别混用。

---

## 4. 第一个 CMake 工程

我们不用 `g++ 文件名.cpp` 的裸编译起步,而是直接学 **CMake**——这是现代 C++ 项目的标准构建方式,后面每个阶段都会用它。

### 4.1 创建工程目录

```bash
cd ~
mkdir -p hello_demo
cd hello_demo
```

### 4.2 写源代码

新建 `main.cpp`:

```cpp
// main.cpp —— 第一个程序
#include <iostream>

int main() {
    int x = 42;
    std::cout << "Hello TinyWebServer! x=" << x << std::endl;
    return 0;
}
```

逐行讲解:
- `#include <iostream>`:引入输入输出流库,`std::cout` 用它
- `int main() { ... }`:程序入口,操作系统从这里开始执行
- `int x = 42;`:声明一个整数变量并初始化
- `std::cout << "..." << x << std::endl;`:打印字符串和变量,然后换行
- `return 0;`:告诉系统"正常结束"

### 4.3 写 CMakeLists.txt

新建 `CMakeLists.txt`(注意大小写,必须一字不差):

```cmake
# CMakeLists.txt —— 告诉 CMake 怎么构建这个工程
cmake_minimum_required(VERSION 3.20)   # 最低 CMake 版本
project(hello)                          # 工程名叫 hello
add_executable(hello main.cpp)          # 用 main.cpp 生成可执行文件 hello
```

三行各干一件事:声明版本、声明工程名、声明"用哪些源文件生成哪个可执行文件"。

### 4.4 构建并运行

```bash
cmake -S . -B build
cmake --build build
./build/hello
```

**逐条解释:**
- `cmake -S . -B build`:`-S` 指定源码目录(`.` 当前目录),`-B` 指定构建目录(`build`)。它会生成一堆 Makefile 和配置
- `cmake --build build`:真正调用编译器,把 `main.cpp` 编成可执行文件
- `./build/hello`:运行它

**预期输出:**

```text
$ cmake -S . -B build
-- The C compiler identification is GNU 13.3.0
...
-- Configuring done (1.3s)
-- Generating done (0.0s)
-- Build files have been written to: build

$ cmake --build build
[ 50%] Building CXX object CMakeFiles/hello.dir/main.cpp.o
[100%] Linking CXX executable hello
[100%] Built target hello

$ ./build/hello
Hello TinyWebServer! x=42
```

看到 `Hello TinyWebServer! x=42` 就成功了。如果哪一步报错,对照第 8 节的"常见坑"。

> 💡 **CMake 到底干了什么?** `cmake -S . -B build` 生成构建脚本,`cmake --build build` 执行它们。你之后会经常看到这两条命令,记成"配置 + 构建"即可。想偷懒可以自己把它们写进一个 `build.sh`(里面一行 `cmake -S . -B build && cmake --build build`,`chmod +x build.sh`,之后 `./build.sh` 一键构建)。

---

## 5. gdb 入门

gdb 是 Linux 下最常用的 C 家族调试器。它的核心三板斧:**打断点 → 单步 → 看变量**。

> 首次运行 gdb 可能会问你 `Enable debuginfod for this session?`,直接按 `n` 回车(它联网下载调试符号,我们不需要)。嫌烦可以执行一次 `echo "set debuginfod enabled off" >> ~/.gdbinit` 永久关掉。

### 5.1 编译带调试信息的版本

要调试,编译时必须加 `-g` 参数(保留变量名、行号等调试信息):

```bash
cd ~/hello_demo
g++ -g -o hello_dbg main.cpp
```

> 注意:这里用裸 `g++` 是为了让你看清 CMake 背后的真实编译命令。后面正式阶段我们统一用 CMake。

### 5.2 开始调试

```bash
gdb ./hello_dbg
```

然后依次输入(逐条,`(gdb)` 是 gdb 的提示符,不用敲):

```text
(gdb) break main
(gdb) run
(gdb) next
(gdb) print x
(gdb) quit
```

**逐条解释与预期输出:**

| 命令 | 含义 | 预期输出 |
|---|---|---|
| `break main` | 在 `main` 函数第一行打断点 | `Breakpoint 1 at 0x1195: file main.cpp, line 3.` |
| `run` | 开始运行,遇到断点停下 | `Breakpoint 1, main () at main.cpp:3` 并显示第 3 行代码 |
| `next` | 执行当前这一行(`int x = 42;`),停在下一行 | 显示第 4 行 `std::cout << ...` |
| `print x` | 打印变量 x 的值 | `$1 = 42` |
| `quit` | 退出 gdb | 若提示 `A debugging session is active`,输入 `y` |

**完整演示:**

```text
(gdb) break main
Breakpoint 1 at 0x1195: file main.cpp, line 3.

(gdb) run
Breakpoint 1, main () at main.cpp:3
3	    int x = 42;

(gdb) next
4	    std::cout << "Hello TinyWebServer! x=" << x << std::endl;

(gdb) print x
$1 = 42

(gdb) quit
```

**为什么要 `next` 一下再 `print`?** 断点停在 `int x = 42;` 这一行时,`x` 还没被赋值(是垃圾值)。`next` 执行完这一行后,`x` 才等于 42。这正是"单步执行"的意义——你能看到程序**一行一行**的变化。

> 完整命令表见附录 [gdb 速查表](annex-gdb-cheatsheet.md),先掌握这三个就够本阶段用了。

---

## 6. MySQL 安装与建库建表

这个项目的完整功能需要数据库存储用户名密码(Stage 8 才用,但**现在**就把环境配好,免得以后打断学习节奏)。

### 6.1 安装 MySQL 服务器与客户端开发库

```bash
sudo apt update
sudo apt install -y mysql-server libmysqlclient-dev
```

- `mysql-server`:数据库服务器本体
- `libmysqlclient-dev`:MySQL 的 C 接口头文件和库——**编译项目代码时必须,别漏**

### 6.2 启动 MySQL 服务

WSL2 默认没有 systemd,用老式的 service 方式启动:

```bash
sudo service mysql start
sudo service mysql status
```

**预期输出:**

```text
 * Starting MySQL database server mysqld     [ OK ]
 * MySQL Community Server 8.0.39 is started  [ OK ]
```

### 6.3 设置 root 密码并建库建表

Ubuntu 24.04 的 MySQL root 用户默认用 `auth_socket` 免密登录,我们把它改成密码 `root`(与项目默认配置一致),并创建 `qgydb` 库和 `user` 表。

```bash
sudo mysql
```

进入 mysql 提示符(`mysql>`),依次执行:

```sql
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'root';
FLUSH PRIVILEGES;
CREATE DATABASE qgydb;
USE qgydb;
CREATE TABLE user (
    username VARCHAR(50) NOT NULL,
    passwd   VARCHAR(50) NOT NULL,
    PRIMARY KEY (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
EXIT;
```

**说明:**
- `ALTER USER ... BY 'root'`:把 root 密码设为 `root`。`mysql_native_password` 是老式认证方式,和老项目兼容性最好
- `CREATE DATABASE qgydb`:建库,名字对应 `main.cpp` 里的 `databasename = "qgydb"`
- `CREATE TABLE user`:建用户表,列名 `username`/`passwd` **必须和 Stage 8 的 SQL 完全一致**(原仓库用 `SELECT username,passwd FROM user`)

> ⚠️ 如果 `ALTER USER ... mysql_native_password` 报 `Unknown plugin 'mysql_native_password'`,说明你装的是 MySQL 8.4+ / 9.x(该插件已移除)。改用默认插件即可,连接方式不变:
> ```sql
> ALTER USER 'root'@'localhost' IDENTIFIED BY 'root';
> ```

> ⚠️ 这段 SQL 敲错会报语法错误,注意分号结尾、列名拼写。出错就 `DROP TABLE user;` 重建,或 `DROP DATABASE qgydb;` 重来。

### 6.4 验证能连上

```bash
mysql -u root -proot -e "SHOW DATABASES;"
```

**预期输出**(至少包含):

```text
+--------------------+
| Database           |
+--------------------+
| information_schema |
| qgydb              |
| mysql              |
| performance_schema |
| sys                |
+--------------------+
```

看到 `qgydb` 就成功了。注意 `-proot` 是 `-p` + 密码 `root` 连写,**没有空格**。

---

## 7. 验收清单

对照检查,全部通过才算本阶段完成:

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `cmake --build build && ./build/hello`(在 `~/hello_demo`) | 输出 `Hello TinyWebServer! x=42` | ☐ |
| 2 | `gdb ./hello_dbg` 里执行 `break main` `run` `next` `print x` | 打印 `$1 = 42` | ☐ |
| 3 | `mysql -u root -proot -e "SHOW DATABASES;"` | 能连上,列出数据库 | ☐ |
| 4 | `mysql -u root -proot -e "SHOW DATABASES LIKE 'qgydb';"` | 能看到 `qgydb` | ☐ |
| 5 | `mysql -u root -proot qgydb -e "DESCRIBE user;"` | 显示 username / passwd 两列 | ☐ |

> 第 5 条的完整预期:
> ```text
> +----------+-------------+------+-----+---------+-------+
> | Field    | Type        | Null | Key | Default | Extra |
> +----------+-------------+------+-----+---------+-------+
> | username | varchar(50) | NO   | PRI | NULL    |       |
> | passwd   | varchar(50) | NO   |     | NULL    |       |
> +----------+-------------+------+-----+---------+-------+
> ```

全部打勾 → 环境就绪,**恭喜你完成 Stage 0!** 接下来进入 C++ 基础部分。

---

## 8. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| `g++: command not found` | 缺编译器 | `sudo apt install -y build-essential` |
| `cmake: command not found` | 缺 CMake | `sudo apt install -y cmake` |
| `gdb` 提示 `No symbol table is loaded` | 编译时忘了 `-g` | `g++ -g ...` 重新编译 |
| `mysql: command not found` | MySQL 没装 | 执行 6.1 的安装命令 |
| `Can't connect to local MySQL server through socket` | 服务没启动 | `sudo service mysql start` |
| `Access denied for user 'root'@'localhost'` | root 密码不对 | 重新 `sudo mysql` 里执行 `ALTER USER` 那段 |
| gdb 首次提示 `debuginfod` | 联网下载调试符号 | 按 `n` 回车,或用 `.gdbinit` 永久关闭 |
| WSL 里 `/mnt/c/...` 的文件没权限写,或构建报奇怪的锁/权限错误 | drvfs 挂载的权限/锁问题 | 改用工作副本:把仓库复制到 `~` 下构建(如 `cp -r /mnt/c/Users/liuzl/Documents/projects/TinyWebServer ~/ws_local`,之后 `cd ~/ws_local` 继续),完成后把 `my_tiny_webserver/` 拷回仓库 |

## 9. 下一步

环境全部就绪。进入 **[C1 语法速览](cpp-01-syntax.md)**——6 章 C++ 基础,每章只讲项目真正会用到的部分。
