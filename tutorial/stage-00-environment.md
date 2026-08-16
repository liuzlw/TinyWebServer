# Stage 0：环境准备

本阶段的目标是：在 Windows 上搭好一个能跑 Linux C++ 代码的开发环境（WSL2 + Ubuntu 22.04），装齐本教程全程要用的工具链（g++/make/gdb/cmake/git/nc/curl）和 MySQL 8，并为项目准备好 `qgydb` 库与 `user` 表。学完本阶段，你要能写出第一个 hello world 并亲手走完「预处理 → 编译 → 汇编 → 链接」四步，还要会用 gdb 断点单步一次程序。这是后面所有阶段的地基，务必让每一条验收都通过。

## 前置要求

- 一台 Windows 10（版本 2004 及以后）或 Windows 11 电脑，且开启了虚拟化（BIOS 里的 VT-x/AMD-V）。
- 会用浏览器下载文件、会打开「终端 / PowerShell」窗口即可，不需要任何 C++ 或 Linux 基础。
- 本阶段是 Stage 1 的前置；Stage 1 见 [stage-01-cpp-basics.md](stage-01-cpp-basics.md)。

> Mac 用户：epoll/pthread 是 Linux 生态，macOS 上跑不了本项目。请在 Mac 上用 UTM/VMware 装一个 Ubuntu 22.04 虚拟机，或租一台云服务器（装 Ubuntu 22.04）后，直接从下面的「安装工具链」开始。

## 理论学习

### 1. 为什么是 WSL2

Windows 原生编译不出本项目（本项目大量使用 `epoll`、`pthread`、`<unistd.h>` 等 Linux 专有接口）。WSL2（Windows Subsystem for Linux 2）会在 Windows 里跑一个**真正的 Linux 内核**，Ubuntu 22.04 作为一个发行版运行在其上，因此 `epoll`、`pthread`、MySQL 等一切 Linux 能力都和真机一致。

```text
┌──────────────────────── Windows ────────────────────────┐
│                                                         │
│   你的浏览器 / VSCode / PowerShell                        │
│                                                         │
│   ┌─────────────── WSL2 虚拟机 ───────────────┐          │
│   │  Linux 内核 (提供 epoll/pthread/socket)     │          │
│   │   ┌──────── Ubuntu 22.04 ────────┐        │          │
│   │   │  g++/make/gdb/MySQL/你的代码   │        │          │
│   │   └──────────────────────────────┘        │          │
│   └───────────────────────────────────────────┘          │
└─────────────────────────────────────────────────────────┘
```

### 2. 一个 C++ 程序是怎么「跑」起来的

写好的 `.cpp` 是给人看的文本，机器只能执行二进制。从文本到可执行文件要经过四步，全部由 `g++` 驱动：

```text
hello.cpp ──预处理──▶ hello.i ──编译──▶ hello.s ──汇编──▶ hello.o ──链接──▶ hello(可执行)
 (源代码)  展开 #include   (纯C++)   生成汇编    (汇编)   生成目标文件   (链接)   补上库函数后运行
            和 #define
```

- **预处理**：把 `#include <iostream>` 里的内容「粘贴」进你的文件、展开宏，产出一个巨大的 `.i` 文件。
- **编译**：把 C++ 翻译成汇编语言 `.s`（仍然是人能读的文本，只是换了一种语言）。
- **汇编**：把汇编翻译成机器码，产出 `.o` 目标文件（二进制，但还不能运行，因为 `cout` 等函数还没着落）。
- **链接**：把 `.o` 和你用到的库（标准库、pthread 等）拼成一个可执行文件 `hello`。

后面我们会用 `g++ -E/-S/-c` 把每一步的中间产物都「停」下来看一遍。

### 3. MySQL 在本项目里的角色

最终完成的 Web 服务器要做「注册 / 登录」：浏览器提交用户名密码 → 服务器去数据库里查有没有这个用户 → 决定返回登录成功页还是失败页。所以本阶段要先把数据库 `qgydb`、表 `user` 建好，并插入一条测试数据，为 Stage 8 的注册登录、以及项目默认的 `root/root/qgydb` 连接参数做铺垫。

## 本阶段 C++ 知识点

本阶段是「环境」阶段，语法上只需要先记住两件事，足够看懂 hello world：

1. **`#include` 与头文件**：`#include <iostream>` 引入标准库的输入输出，`<iostream>` 提供 `cout`（标准输出流）。它发生在预处理阶段，本质是文本粘贴。
2. **`main` 函数是程序入口**：操作系统运行程序时，第一个执行的函数就是 `main`；`return 0;` 表示「程序正常结束」，这个 0 会返回给操作系统。`main` 函数签名常用 `int main()` 或 `int main(int argc, char *argv[])`（后者可以接收命令行参数，本项目 `main.cpp` 用的就是它）。

## 动手实现

下面每一步都在 Windows 的 PowerShell / 终端 或 Ubuntu 终端里进行，标注清楚了执行位置。

### 1. 安装 WSL2 与 Ubuntu 22.04

在 **Windows 的 PowerShell（以管理员身份运行）** 中执行：

```bash
wsl --install -d Ubuntu-22.04
```

- 若提示需要重启，按提示重启，重启后会自动继续。
- 安装过程会让你设置一个 Linux 用户名（比如 `qinyi`）和密码。**这个密码是以后 `sudo` 要用的，请记住**。
- 安装完成后，单独执行 `wsl`（或开始菜单打开「Ubuntu 22.04」）即可进入 Linux 终端。

验证 WSL 版本是 2：

```bash
wsl --list --verbose
```

预期输出类似：

```text
  NAME            STATE           VERSION
* Ubuntu-22.04    Running         2
```

如果 VERSION 不是 2，执行 `wsl --set-default-version 2`，再用 `wsl --set-version Ubuntu-22.04 2` 转换（转换需要一点时间）。

### 2. 初始化 Ubuntu（可选：换 apt 源）

进入 Ubuntu 终端，先更新软件源索引：

```bash
sudo apt update
```

> 如果 `apt update` 特别慢（国内网络常见），可以换成清华或阿里镜像源。以清华源为例（此步**可选**）：
>
> ```bash
> sudo sed -i 's@//.*archive.ubuntu.com@//mirrors.tuna.tsinghua.edu.cn@g' /etc/apt/sources.list
> sudo sed -i 's@//.*security.ubuntu.com@//mirrors.tuna.tsinghua.edu.cn@g' /etc/apt/sources.list
> sudo apt update
> ```

### 3. 安装工具链

在 Ubuntu 终端执行：

```bash
sudo apt install -y build-essential gdb cmake git netcat-openbsd curl
```

说明：

| 包 | 提供什么 | 本教程用途 |
|---|---|---|
| `build-essential` | `gcc`、`g++`、`make` 及标准库 | 编译 C/C++、make 构建 |
| `gdb` | 调试器 | 断点、单步、查变量 |
| `cmake` | 跨平台构建工具 | 附录 A 把项目 CMake 化 |
| `git` | 版本管理 | 下载仓库、管理自己的工作区 |
| `netcat-openbsd` | `nc` 命令 | Stage 2 起测试网络回显 |
| `curl` | 命令行 HTTP 客户端 | Stage 5 起验证 HTTP 响应 |

### 4. 安装并初始化 MySQL 8

Ubuntu 22.04 默认源里就是 MySQL 8.0。安装：

```bash
sudo apt install -y mysql-server
```

安装后确认服务在运行：

```bash
sudo systemctl status mysql
```

看到 `active (running)` 即正常；没启动则 `sudo systemctl start mysql`。

**关键：Ubuntu 的 MySQL 默认让 root 走 `auth_socket` 认证**——也就是说只有用 `sudo mysql` 时才能免密进入，普通 `mysql -uroot -p` 会因为 root「没有可用的密码」而无法登录。而本项目默认用 `root/root` 密码登录，所以要先给 root 设密码，并改成密码认证方式。

先用系统身份免密进入：

```bash
sudo mysql
```

在 `mysql>` 提示符下执行（注意每条 SQL 结尾的分号）：

```bash
ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'root';
FLUSH PRIVILEGES;
EXIT;
```

> `mysql_native_password` 是与本项目最兼容的认证插件（上游项目环境是 MySQL 5.7）。若你想用 MySQL 8 默认的 `caching_sha2_password`，只需把上面那句里的 `WITH mysql_native_password` 去掉即可，Ubuntu 22.04 自带的 libmysqlclient 也支持它。

现在验证能用密码登录：

```bash
mysql -uroot -proot
```

看到 `mysql>` 提示符即成功。接着建库、建表、插入一条测试数据（与仓库 `README.md` 完全一致）：

```bash
CREATE DATABASE qgydb;
USE qgydb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;
INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```

验证数据：

```bash
SELECT * FROM qgydb.user;
```

预期输出：

```text
+----------+--------+
| username | passwd |
+----------+--------+
| name     | passwd |
+----------+--------+
1 row in set (0.00 sec)
```

退出 MySQL：

```bash
EXIT;
```

#### 项目的默认连接参数在哪改

仓库的 `main.cpp` 第 5～8 行硬编码了数据库登录信息：

```cpp
//需要修改的数据库信息,登录名,密码,库名
string user = "root";
string passwd = "root";
string databasename = "qgydb";
```

- 登录名 → `user`（第 6 行），密码 → `passwd`（第 7 行），库名 → `databasename`（第 8 行）。
- 这三个值最终在 `webserver.cpp` 里传给连接池，连接池用 `mysql_real_connect(con, "localhost", user, passwd, databasename, 3306, NULL, 0)` 建立连接。
- 所以：**只要你按本节把 root 密码设成 `root`、库名建为 `qgydb`，就不用改任何代码**；如果你给 MySQL 设了别的密码，就把 `main.cpp` 第 7 行改成你的密码（本仓库是参考答案，改动只在 `my_tiny_webserver/` 里做，见 Stage 9）。

### 5. 准备编辑器

#### 5.1 vim 最小命令（够用即可）

Linux 下最基础的编辑器，几乎每个环境都有。打开/创建文件：

```bash
vim hello.cpp
```

vim 有「命令模式」和「插入模式」两种状态，初学只需记住这套流程：

| 操作 | 按键 |
|---|---|
| 进入编辑（插入）模式 | `i` |
| 保存并退出 | `Esc` 然后 `:wq` 回车 |
| 不保存强制退出 | `Esc` 然后 `:q!` 回车 |
| 删除一行 | 命令模式下 `dd` |
| 撤销 | 命令模式下 `u` |

#### 5.2 VSCode + Remote-WSL（可选，强烈推荐）

在 Windows 装好 [VSCode](https://code.visualstudio.com/)，安装扩展 **WSL**（微软官方，图标为一只小企鹅）。之后：

1. 在 Ubuntu 终端里 `cd` 到你的代码目录；
2. 执行 `code .`，VSCode 会自动通过 WSL 连接打开该目录；
3. 之后你在 VSCode 里编辑的文件，实际就落在 Ubuntu 的文件系统里，直接 `g++` 编译即可。

这套方案能让你用上语法高亮、自动补全和图形化调试，比 vim 舒服得多。

### 6. 第一个 hello world：编辑 → 四步编译 → 运行

先按仓库约定建立工作区（`my_tiny_webserver/` 与仓库 `TinyWebServer/` 同级）：

```bash
mkdir -p ~/projects/my_tiny_webserver/stage00
cd ~/projects/my_tiny_webserver/stage00
```

> 约定回顾：`~/projects/TinyWebServer/` 是本仓库（参考答案，只读）；`~/projects/my_tiny_webserver/` 是你的代码区。请把仓库 clone 到 `~/projects/TinyWebServer`（`git clone https://github.com/qinguoyi/TinyWebServer.git`），本教程后续所有「参考答案对照」都相对该目录。

用 vim 或 VSCode 创建 `hello.cpp`：

```cpp
#include <iostream>
using namespace std;

int main()
{
    cout << "hello world" << endl;
    return 0;
}
```

逐行看：

- `#include <iostream>`：引入输入输出库，`cout`、`endl` 都来自它。
- `using namespace std;`：让 `cout` 不用写成 `std::cout`，省事（项目里也普遍这么写）。
- `int main()`：程序入口。
- `cout << "hello world" << endl;`：把字符串输出到屏幕，`endl` 表示换行。
- `return 0;`：正常结束。

**四步编译，逐步观察中间产物**：

```bash
# 1) 预处理：展开 #include 和宏
g++ -E hello.cpp -o hello.i
# 2) 编译：C++ 翻译成汇编
g++ -S hello.i -o hello.s
# 3) 汇编：汇编翻译成机器码目标文件
g++ -c hello.s -o hello.o
# 4) 链接：目标文件 + 库 → 可执行文件
g++ hello.o -o hello
```

用 `ls -l hello.i hello.s hello.o hello` 能看到四个产物依次出现，其中 `hello.i` 通常有几万行（因为 `<iostream>` 展开了大量标准库代码），这就是「预处理是文本粘贴」的直观证据。日常开发不必分四步，一条命令搞定：

```bash
g++ hello.cpp -o hello
```

运行：

```bash
./hello
```

预期输出：

```text
hello world
```

### 7. gdb 初体验：断点、单步、看变量

先把上面 `hello.cpp` 稍微丰富一点，方便观察变量和函数调用。新建 `debug_demo.cpp`：

```cpp
#include <iostream>
using namespace std;

int add(int a, int b)
{
    int sum = a + b;
    return sum;
}

int main()
{
    int x = 1;
    int y = 2;
    int z = add(x, y);
    cout << "z = " << z << endl;
    return 0;
}
```

**必须加 `-g`** 编译，它会把「第几行源码对应哪条机器指令」的信息写进可执行文件，否则 gdb 看不到源码行号：

```bash
g++ -g debug_demo.cpp -o debug_demo
```

启动 gdb：

```bash
gdb ./debug_demo
```

进入 gdb 后按顺序输入命令，真实会话如下（`(gdb)` 是 gdb 自己的提示符，后面的命令才是你敲的）：

```text
(gdb) break main
Breakpoint 1 at 0x11e9: file debug_demo.cpp, line 12.
(gdb) run
Starting program: /home/qinyi/projects/my_tiny_webserver/stage00/debug_demo

Breakpoint 1, main () at debug_demo.cpp:12
12	    int x = 1;
(gdb) next
13	    int y = 2;
(gdb) next
14	    int z = add(x, y);
(gdb) print x
$1 = 1
(gdb) step
add (a=1, b=2) at debug_demo.cpp:6
6	    int sum = a + b;
(gdb) next
7	    return sum;
(gdb) print sum
$2 = 3
(gdb) quit
```

各命令含义：

| 命令 | 含义 |
|---|---|
| `break main` | 在 `main` 函数入口下断点，程序一进 main 就停 |
| `run` | 开始运行程序，直到命中断点 |
| `next` | 单步执行**一行**（不进入函数内部） |
| `step` | 单步执行，**进入**被调用的函数内部 |
| `print x` | 打印变量 `x` 当前的值 |
| `quit` | 退出 gdb |

观察上面 `next` 与 `step` 在第 14 行的区别：`next` 会直接执行完 `add` 并回到下一行，而 `step` 会「钻」进 `add` 函数体里。这是调试最重要的两个动作。更系统的 gdb 用法在 [附录 B](appendix-b-gdb.md)。

## 编译与运行

本阶段没有需要自己「实现」的项目代码，核心命令汇总如下（均已在上面拆解过）：

```bash
# 编译并运行 hello world
g++ hello.cpp -o hello && ./hello

# 带调试信息编译并用 gdb 单步
g++ -g debug_demo.cpp -o debug_demo
gdb ./debug_demo
```

如果这两组命令的输出分别出现 `hello world` 和 gdb 的 `(gdb)` 提示符，本阶段就完成了。

## 验收清单

逐条执行，每一条的「预期输出」都出现才算过关：

- [ ] `wsl --list --verbose`（在 Windows PowerShell）输出中包含 `Ubuntu-22.04` 且 VERSION 为 `2`
- [ ] `g++ --version` 输出形如 `g++ (Ubuntu 11.x.x-... ) 11.x.x`（g++ 11 系列）
- [ ] `gcc --version` 输出 `gcc (Ubuntu 11.x.x-... ) 11.x.x`
- [ ] `make --version` 输出 `GNU Make 4.3` 左右
- [ ] `gdb --version` 输出 `GNU gdb (Ubuntu 12.1-...) 12.1`
- [ ] `cmake --version` 输出 `cmake version 3.22.x`
- [ ] `git --version` 输出 `git version 2.34.x`
- [ ] `nc -h 2>&1 | head -n 1` 输出 `OpenBSD netcat` 或 usage 信息（说明 `nc` 可用）
- [ ] `curl --version | head -n 1` 输出 `curl 7.81.x`
- [ ] `sudo systemctl status mysql` 输出包含 `active (running)`
- [ ] `mysql -uroot -proot` 能进入 `mysql>` 提示符（无需 `sudo`）
- [ ] 在 mysql 中执行 `SELECT * FROM qgydb.user;` 输出一行 `name | passwd`
- [ ] `g++ -E hello.cpp -o hello.i && g++ -S hello.i -o hello.s && g++ -c hello.s -o hello.o && g++ hello.o -o hello` 四步全部无报错，且 `ls -l hello.i hello.s hello.o hello` 能看到四个文件
- [ ] `./hello` 输出 `hello world`
- [ ] 在 `gdb ./debug_demo` 中依次执行 `break main`、`run`、`next`、`print x`，`print x` 输出 `$1 = 1`
- [ ] 在 gdb 中执行 `step` 能进入 `add` 函数，`print sum` 输出 `$2 = 3`

## 参考答案对照

本阶段是环境搭建，不涉及仓库源码，无对照文件。仅一处需要你「知道在哪改」：

| 内容 | 仓库位置 | 说明 |
|---|---|---|
| 数据库连接参数（root/root/qgydb） | `main.cpp` 第 5～8 行 | 登录名/密码/库名，改动规则见上文 |
| 建库建表 SQL | `README.md` 第 154～167 行 | 与本阶段建的库表一致 |

## 常见问题

**1. `wsl --install` 报错或提示需要启用「虚拟机平台」**
多半是 BIOS 里没开虚拟化，或 Windows 功能「虚拟机平台」「适用于 Linux 的 Windows 子系统」没勾选。重启进 BIOS 开启 VT-x/AMD-V，然后在「控制面板 → 程序 → 启用或关闭 Windows 功能」里勾选「虚拟机平台」和「适用于 Linux 的 Windows 子系统」，重启后再执行。

**2. `mysql -uroot -p` 提示 `Access denied for user 'root'@'localhost'`，但 `sudo mysql` 能进**
这就是 `auth_socket` 插件在作怪：root 被绑定成「只能用系统身份登录」，所以 `-p` 输密码反而失败。用 `sudo mysql` 进去执行 `ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'root'; FLUSH PRIVILEGES;` 即可（本阶段第 4 步已包含）。

**3. `apt update` / `apt install` 特别慢或超时**
网络问题，换国内镜像源即可（本阶段第 2 步给了清华源一行命令）。换完源记得再 `sudo apt update` 一次。如果 WSL2 的 DNS 解析有问题导致 `apt` 连不上，可以在 `/etc/wsl.conf` 加 `[network] generateResolvConf = false` 后重启 WSL，并手动指定 DNS（如 `8.8.8.8`）。

**4. gdb 里执行 `list` 或单步时报 `No symbol table is loaded` / 看不到源码行**
编译时忘了加 `-g`，可执行文件里没有调试符号。用 `g++ -g xxx.cpp -o xxx` 重新编译再进 gdb 即可；「no debug symbols」本质是同一类问题（用 `file` 命令看可执行文件，有 `with debug_info` 才说明带符号）。

**5. `./hello` 报 `Permission denied` 或 `command not found`**
`Permission denied` 是文件没有可执行权限，`chmod +x hello` 后再 `./hello`；`command not found` 通常是你敲成了 `hello` 而没加 `./`——Linux 不会自动在当前目录找可执行文件，必须写 `./hello`。

**6. Windows 浏览器访问不到 WSL2 里的服务（为后续阶段预习）**
WSL2 走的是 NAT 虚拟网络，它的 IP 与 Windows 不同网段。想从 Windows 浏览器访问 WSL2 里跑的服务，用 `ip addr show eth0` 查到 WSL2 的 IP（形如 `172.x.x.x`），在 Windows 里访问 `http://172.x.x.x:9006`。反过来，WSL2 里访问 Windows 上的服务，Windows 防火墙需放行对应端口。真机/云服务器则直接访问它们的公网/内网 IP 即可。

**7. VSCode 的 `code .` 提示找不到命令**
在 Ubuntu 里装 WSL 扩展后，VSCode 会自动往 PATH 注入 `code` 命令；若没有，先在 Windows 的 VSCode 里执行一次「Remote-WSL: Open Folder」，之后通常就能用 `code .` 了。

## 思考题

1. 为什么 WSL2 能编译运行本项目，而 Windows 原生（MinGW/MSVC）不行？试着说出 `epoll` 和 `pthread` 分别是什么层面的东西。
2. 「预处理」到底把什么「粘贴」进了 `hello.i`？用 `wc -l hello.i` 数一下行数，再解释为什么它比 `hello.cpp` 大那么多。
3. `.o` 目标文件是二进制，为什么它还不能直接运行？「链接」这一步补上了什么？
4. gdb 中 `next` 和 `step` 的区别是什么？什么场景下你会选择 `step` 而不是 `next`？
5. 为什么 Ubuntu 的 MySQL 默认让 root 用 `auth_socket` 而不是密码登录？这种设计对系统安全有什么好处？
6. 项目默认用 `root/root` 连接数据库，从安全角度看有什么隐患？正式部署时你会怎么改？

## 下一步

环境就绪后，进入 [Stage 1：C++ 快速上手](stage-01-cpp-basics.md)，用一批贴近本项目的小练习补齐 C++ 语法，为读懂 `webserver.cpp`、`http_conn.cpp` 这些源码打基础。
