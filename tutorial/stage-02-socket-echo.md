# Stage 2：第一个服务器——阻塞式 echo 服务器

本阶段你将写出人生中第一个"服务器"程序：一个 **echo（回声）服务器**。它监听一个端口，等客户端连上来，把客户端发来的每一句话原样发回去。它功能简单得"可怜"，而且一次只能服务一个客户端，但它是后面所有阶段的地基：**socket 的创建流程**是 Stage 4 epoll、Stage 5 HTTP 服务器的共同底座；而本阶段暴露出来的"第二个客户端要排队"问题，正是 Stage 3 引入线程池的直接动机。

完成本阶段后，你将：

- 掌握 TCP 服务器最核心的五步流程：`socket → bind → listen → accept → recv/send → close`；
- 学会用 `nc` 连接服务器、用 `ss` / `netstat` 查看端口监听状态；
- 学会手写第一份 makefile，告别每次敲一长串 g++ 命令。

## 前置要求

- 已完成 [Stage 0：环境准备](stage-00-environment.md)：装好 Ubuntu 22.04（WSL2 或虚拟机均可）、g++ 11、gdb；
- 已完成 [Stage 1：C++ 快速上手](stage-01-cpp-basics.md)：会用 g++ 编译单文件程序，知道 `#include`、`main` 函数、基本类型；
- 工作区 `my_tiny_webserver/` 里目前只有 [Stage 0](stage-00-environment.md) 留下的 `stage00/` 练习目录（可保留或删除）；本阶段在 `my_tiny_webserver/` 根目录新建 `echo_server.cpp` 与 `makefile`。

> 工作区约定：所有代码写在本仓库**同级**的 `my_tiny_webserver/` 目录下，本仓库 `TinyWebServer/` 只当参考答案，不要改它。目录结构见 [主索引](README.md)。

## 理论学习

### 1. 网络通信的直觉：socket 就是"电话"

想象两个人打电话。要通话需要三样东西：一部电话机（socket）、一个电话号码（IP + 端口）、一段通话过程（连接 + 收发数据）。Linux 把网络连接抽象成一个**文件描述符（file descriptor，简称 fd）**——一个整数，读它就像读文件、写它就像写文件。这就是 `socket` 名字的由来："插在墙上的插座"。

服务器和客户端的角色是不对称的：

- **服务器**：先"开机并公布电话号码"（绑定端口 + 监听），然后"守在电话旁等铃响"（accept）；
- **客户端**：主动"拨号"（connect），拨通后双方开始收发数据。

### 2. IP 与端口：门牌号与房号

- **IP 地址**：标识网络上的哪一台机器，好比小区的门牌号。例如 `192.168.1.10`。
- **端口**：标识机器上的哪一个程序，好比房间号。取值范围 0～65535，其中 0～1023 是系统保留端口（如 80 是 HTTP），自己练习时请用 1024 以上的端口。
- **回环地址 `127.0.0.1`**：一个特殊 IP，永远指向"本机自己"。客户端和服务器跑在同一台电脑上时，就用它连接。它的主机名通常叫 `localhost`。
- **`0.0.0.0`（INADDR_ANY）**：监听时表示"监听本机所有网卡"，这样无论从哪个 IP 连进来都能收到。

```text
  本机（127.0.0.1）
  ┌─────────────────────────────────┐
  │  echo_server  监听 0.0.0.0:9006 │  <- 服务器
  │        ▲                        │
  │        │ 通过 127.0.0.1:9006 连接│
  │        │                        │
  │   nc  127.0.0.1 9006            │  <- 客户端
  └─────────────────────────────────┘
```

### 3. TCP：可靠的双向字节流

TCP（Transmission Control Protocol，传输控制协议）和 UDP 是两种最常见的传输层协议：

| 对比项 | TCP | UDP |
|---|---|---|
| 是否面向连接 | 需要先"握手"建立连接 | 无连接，直接发 |
| 可靠性 | 保证不丢、不乱序、不重复 | 不保证，可能丢 |
| 数据形态 | 字节流（没有消息边界） | 数据报（有边界） |
| 典型用途 | HTTP、文件传输、本项目 | 视频直播、DNS |

本项目用 TCP，因为它可靠——Web 服务器绝不允许网页"缺半截"。TCP 建立连接需要**三次握手**，断开需要**四次挥手**：

```text
  三次握手（建立连接）            四次挥手（断开连接）
  客户端          服务器           客户端          服务器
    |--- SYN ------>|               |--- FIN ------>|  1. 客户端：我说完了
    |<-- SYN+ACK ---|               |<--- ACK ------|  2. 服务器：知道了
    |--- ACK ------>|               |<--- FIN ------|  3. 服务器：我也说完了
    |               |               |--- ACK ------>|  4. 客户端：知道了
    |<== 数据通信 ==>|               |               |  连接关闭
```

- **三次握手**：双方确认"我发得出去、你也收得到、你也发得出来"，缺一不可，所以是三次而不是两次。
- **四次挥手**：断开时 TCP 是"半关闭"的——一方说完了对方可能还没说完，所以 FIN 和 ACK 要分开各走一次。

> 本阶段你不需要自己写握手代码，内核帮你完成了。你只要知道：`listen` 之后客户端 connect 的握手由内核处理，`accept` 拿到的已经是"握完手"的连接。

### 4. 服务器 socket 五步流程

任何 TCP 服务器的主干都是这五步：

```text
 socket()      bind()       listen()      accept()     recv()/send()    close()
   │            │             │             │              │             │
   ▼            ▼             ▼             ▼              ▼             ▼
 创建套接字   绑定地址      开始监听      接受连接        收发数据       关闭连接
 (拿电话机)  (公布号码)   (守在电话旁)  (接起电话)     (通话内容)      (挂电话)
```

1. `socket()`：创建一个套接字，返回一个 fd（还没绑定任何地址）；
2. `bind()`：把 fd 绑定到一个 `IP:端口`，即"公布自己的电话号码"；
3. `listen()`：把套接字设为监听状态，并指定**连接等待队列**的长度（backlog）；
4. `accept()`：从等待队列里取出一个已完成的连接，返回**新的 fd** 用于和这个客户端通信。注意：监听用的 fd 和通信用的 fd 是两个不同的 fd；
5. `recv()` / `send()`：用新 fd 收发数据；通信结束 `close()` 关掉它。

### 5. 用 ss / netstat 查看端口监听

服务器跑起来后，如何确认它真的在监听？用 `ss`（推荐）或 `netstat`：

```bash
ss -tlnp | grep 9006
# 或
netstat -tlnp | grep 9006
```

- `-t` 只看 TCP，`-l` 只看处于 LISTEN 状态的，`-n` 不把端口翻译成服务名，`-p` 显示是哪个进程在监听；
- `-p` 显示进程名需要你有该进程的权限（自己启动的进程无需 sudo）。

预期输出会看到 `LISTEN` 字样和 `echo_server` 进程名（见验收清单）。

## 本阶段 C++ 知识点

本阶段代码会用到下面这些语法点，全部围绕真实代码展开。

### 1. 指针与数组

数组名在大多数表达式中会"退化"成指向首元素的指针：

```cpp
char buf[BUF_SIZE];              // 在栈上分配 1024 个 char 的数组
recv(clnt_sock, buf, BUF_SIZE - 1, 0);  // buf 在这里等价于 &buf[0]，类型是 char*
```

`recv` 需要知道"往哪儿写"，我们传的是首地址 `buf` 和最大长度。它不需要知道"数组"这个整体概念，只要一个起始指针即可——这正是"数组退化为指针"的直观体现。

### 2. struct：打包多个字段

`sockaddr_in` 是内核定义的一个结构体，用来描述一个 socket 地址：

```cpp
struct sockaddr_in serv_addr;          // 定义变量
memset(&serv_addr, 0, sizeof(serv_addr)); // 先整体清零，避免脏数据
serv_addr.sin_family = AF_INET;        // 地址族：IPv4
serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // IP：监听所有网卡
serv_addr.sin_port = htons(atoi(argv[1]));     // 端口：来自命令行参数
```

用 `.` 访问结构体成员。注意 `sin_addr` 本身又是一个结构体，所以 IP 要写成 `serv_addr.sin_addr.s_addr` 两层。

### 3. C 字符串：以 `'\0'` 结尾的字符数组

C 语言没有专门的字符串类型，字符串就是"以 `'\0'` 结尾的 char 数组"。收数据时我们要手动补上这个结尾：

```cpp
char buf[BUF_SIZE];
int str_len = recv(clnt_sock, buf, BUF_SIZE - 1, 0); // recv 返回实际收到的字节数
buf[str_len] = '\0';   // 手动封口，buf 才是一个合法的 C 字符串
```

`recv` 返回的是"这次收到了多少字节"，它**不会**帮你补 `'\0'`，所以这一步不能省（尤其当你后面要用 `printf("%s", buf)` 或 `strlen` 时）。

### 4. sizeof 与 strlen

- `sizeof(buf)` 是**编译期**就知道的、变量占用的字节数，这里恒为 1024；
- `strlen(buf)` 是**运行期**从 `buf` 开始数到第一个 `'\0'` 为止的字符个数。

```cpp
char buf[BUF_SIZE];        // sizeof(buf) == 1024
strcpy(buf, "hi");         // strlen(buf) == 2，但 sizeof(buf) 仍是 1024
```

所以"缓冲区有多大"用 `sizeof`，"字符串有多长"用 `strlen`，两者语义完全不同。

### 5. 函数参数传递

C++ 默认是**值传递**（把值复制一份），但数组作为参数时会退化为指针：

```cpp
void error_handling(const char *msg)  // 传指针：只复制地址，不复制字符串本身
{
    perror(msg);
    exit(1);
}
```

传指针而不是传整个数组，是为了高效——否则每次调用都要复制 1024 个字节。这正是 C/C++ 里"传指针"如此常见的原因。

### 6. errno 与错误处理

Linux 系统调用失败时，通常返回 `-1`，并把**失败原因**写进一个全局整数 `errno`。`perror(msg)` 会自动读取 `errno`，打印"你的提示文字 + 系统对 errno 的解释"：

```cpp
if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
    error_handling("bind() error");   // 输出类似：bind() error: Address already in use
```

需要 `#include <errno.h>`（`perror` 在 `<stdio.h>`）。养成习惯：**每个系统调用都检查返回值**，这是 C 程序员的职业素养。

### 7. const：只读承诺

```cpp
void error_handling(const char *msg)
```

`const char *msg` 承诺"这个函数不会修改 msg 指向的内容"。编译器会替你盯着：一旦函数里试图 `msg[0] = 'x'`，直接编译报错。写 `const` 能让意图清晰、提前暴露 bug。

## 动手实现

### 第 1 步：单客户端版（一次 echo 后退出）

先写一个"最小可跑"版本：接收一个客户端、收一次数据、原样发回，然后退出。为了看清主干，这一步**省略了错误检查**（第 2 步补全）。

在 `my_tiny_webserver/` 下创建 `echo_server.cpp`：

```cpp
// echo_server.cpp —— 第 1 步：单客户端版（一次 echo 后退出，省略错误检查）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

int main(int argc, char *argv[])
{
    // 1. 创建套接字
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    // 2. 绑定地址
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;              // IPv4
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    serv_addr.sin_port = htons(atoi(argv[1]));     // 端口取自命令行参数
    bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

    // 3. 监听（等待队列长度 5）
    listen(serv_sock, 5);

    // 4. 接受一个客户端连接，拿到通信用的 fd
    struct sockaddr_in clnt_addr;
    socklen_t clnt_addr_size = sizeof(clnt_addr);
    int clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);

    // 5. 收一次、回一次
    char buf[BUF_SIZE];
    int str_len = recv(clnt_sock, buf, BUF_SIZE - 1, 0);
    buf[str_len] = '\0';
    send(clnt_sock, buf, str_len, 0);

    // 6. 关闭
    close(clnt_sock);
    close(serv_sock);
    return 0;
}
```

编译运行（暂时手工编译，下一节再用 make）：

```bash
cd ~/projects/my_tiny_webserver
g++ -Wall -g -o echo_server echo_server.cpp
./echo_server 9006
```

另开一个终端：

```bash
nc 127.0.0.1 9006
hello
# 服务器把 hello 原样发回来，然后两边都退出
```

这段代码就是 TCP 服务器的"骨架"，请逐行对照上一节的五步流程读懂它。它的缺点很明显：只服务一个客户端就退出了。

### 第 2 步：循环版（while 循环 accept，一次服务一个客户端）

真实服务器不能接完一个就退出。我们把"接受 + 服务"放进 `while(1)` 循环，并为每个系统调用补上错误检查。

**完整替换** `echo_server.cpp` 为下面的最终版本（本阶段交付物之一）：

```cpp
// echo_server.cpp —— 第 2 步：循环版阻塞 echo 服务器（本阶段最终版本）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#define BUF_SIZE 1024

// 统一错误处理：打印 errno 的含义并退出
void error_handling(const char *msg)
{
    perror(msg);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("用法: %s <端口号>\n", argv[0]);
        exit(1);
    }

    // 1. 创建套接字：IPv4、字节流(TCP)、协议自动选择(0)
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1)
        error_handling("socket() error");

    // 2. 绑定地址
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));      // 先清零
    serv_addr.sin_family = AF_INET;                // IPv4
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡
    serv_addr.sin_port = htons(atoi(argv[1]));     // 端口

    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("bind() error");

    // 3. 监听：等待队列长度为 5
    if (listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    printf("echo 服务器已启动，监听端口 %s\n", argv[1]);

    while (1)
    {
        // 4. 接受一个客户端连接
        struct sockaddr_in clnt_addr;
        socklen_t clnt_addr_size = sizeof(clnt_addr);
        int clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1)
        {
            perror("accept() error");
            continue;   // 单次失败不致命，继续等待下一个连接
        }

        // inet_ntoa 把二进制 IP 转成 "x.x.x.x"，ntohs 把端口转回主机字节序
        printf("客户端连接: %s:%d\n",
               inet_ntoa(clnt_addr.sin_addr), ntohs(clnt_addr.sin_port));

        // 5. 为这个客户端服务，直到它断开
        char buf[BUF_SIZE];
        int str_len;
        while ((str_len = recv(clnt_sock, buf, BUF_SIZE - 1, 0)) > 0)
        {
            buf[str_len] = '\0';          // 补 C 字符串结尾
            send(clnt_sock, buf, str_len, 0); // 原样发回 = echo
        }

        // 客户端断开（recv 返回 0）或出错（返回 -1）都会走到这里
        close(clnt_sock);
        printf("客户端断开\n");
    }

    close(serv_sock);
    return 0;
}
```

关键行讲解：

- `while ((str_len = recv(...)) > 0)`：`recv` 的返回值分三种情况——**>0** 收到数据继续循环；**==0** 客户端正常断开（对端 close），退出循环；**-1** 出错退出循环。这是 TCP 收数据的标准写法；
- `buf[str_len] = '\0';` 后再 `send(..., str_len, 0)`：send 的第三参数是"发送多少字节"，这里发 `str_len` 个字节，**不包含** `'\0'`，所以客户端收到的就是原文；
- `continue`：`accept` 偶尔失败（比如被信号打断）不应让整个服务器退出，所以打印后继续循环。

### 第 3 步：理解它的局限（为线程池埋下动机）

这个版本**一次只能服务一个客户端**。原因：主线程在 `while ((str_len = recv(...)) > 0)` 里**阻塞**——没数据时就一直等，等不到就不往下走，也就轮不到下一次 `accept`。

我们做一个实验验证（这正是验收清单里的第 5、6 条）：

1. 终端 A：`nc 127.0.0.1 9006`，输入 `hello`，立刻回显 `hello`，连接保持；
2. 终端 B：`nc 127.0.0.1 9006`，输入 `world`，**没有任何回显**；
3. 现在在终端 A 按 `Ctrl+C` 断开。观察终端 B：它刚才输入的 `world` **此刻才被回显出来**。

原因拆解：

```text
 主线程时间线
 ──────────────────────────────────────────────────>
 [accept 客户端A] → [阻塞在 recv，服务 A] → A 断开 → [accept 客户端B] → [读到 B 早就发来的 "world"] → 回显
                                        ↑ 在这之前，B 一直排队，B 的数据只是存在内核缓冲区里
```

- 客户端 B 的 TCP 握手早就完成（握手由内核完成，不需要我们 accept），连接只是躺在 `listen` 的等待队列里；
- B 发来的 `world` 也早就到了，存在内核的接收缓冲区里；
- 但服务器**没空**去 `accept` 它、去读它——主线程被客户端 A 占着。

这就是"阻塞式单线程服务器"的根本缺陷：**一个慢客户端会拖住所有其他客户端**。怎么解决？"来一个客户端就开一个新线程去服务"，或者"预先准备一批线程排队领任务"——后者就是下一阶段要讲的**线程池**。这也是本项目并发模型的第一环：Stage 2 阻塞 → Stage 3 线程池 → Stage 4 epoll。

## 编译与运行

### make 入门：手写第一份 makefile

现在项目还只有一个文件，手工 `g++ -Wall -g -o echo_server echo_server.cpp` 也能编译。但等到 Stage 3 起文件变多，每次都手敲整串命令又慢又容易漏。make 解决的问题就是：**只编译改过的文件，并自动拼出编译命令**。

一个 makefile 由若干条**规则（rule）**组成，每条规则形如：

```makefile
目标: 依赖1 依赖2 ...
	命令
	命令
```

- **目标（target）**：要生成的东西，通常是一个文件名（如 `echo_server`）；
- **依赖（prerequisites）**：生成目标所依赖的文件；
- **命令（recipe）**：如何用依赖生成目标。**命令行的缩进必须是 Tab 键，不能是空格**，否则 make 报 `missing separator`；
- make 的直觉：如果目标不存在，或**依赖比目标更新**（依赖被改过），就执行命令重新生成目标。依赖没变时直接跳过，这就是"增量编译"。

本阶段的 makefile（创建 `my_tiny_webserver/makefile`，**代码块里的缩进是 Tab，复制时请保留；若你的编辑器把 Tab 转成了空格，make 会报错**）：

```makefile
# makefile —— 命令行缩进必须是 Tab 键！
# 变量：编译器
CXX = g++
# 变量：编译选项，-Wall 打开常用警告，-g 生成 gdb 调试信息
CXXFLAGS = -Wall -g

# 默认目标（make 不带参数时执行的第一条规则）
echo_server: echo_server.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

# clean 目标：make clean 时执行，用于清理产物
clean:
	rm -f echo_server
```

逐项解释：

- **变量**：`CXX`、`CXXFLAGS` 是约定俗成的名字（CXX 指 C++ 编译器，FLAGS 指选项）。引用变量用 `$(CXX)`。改一处即可全局生效；
- **`$@`**：代表"目标"本身，展开后就是 `echo_server`；
- **`$^`**：代表"所有依赖"，展开后是 `echo_server.cpp`。于是命令实际执行的是 `g++ -Wall -g -o echo_server echo_server.cpp`；
- **`clean`**：一个"伪目标"——它不生成叫 `clean` 的文件，只是提供一个清理命令的入口。`make clean` 时执行 `rm -f echo_server`；
- 为什么第一条规则是默认目标：`make` 不带参数时，执行 makefile 中**第一条规则**。所以通常把主目标放在最前面。

`make` 会打印它实际执行的命令，你可以据此验证 `$@`/`$^` 的展开结果：

```bash
make
# g++ -Wall -g -o echo_server echo_server.cpp
```

### 完整操作流程

```bash
cd ~/projects/my_tiny_webserver
make          # 编译，得到 echo_server
./echo_server 9006
```

在另一个终端连接：

```bash
nc 127.0.0.1 9006
```

## 验收清单

> 每一条都要亲自动手跑一遍、看到预期输出才算过关。`[ ]` 勾掉换成 `[x]`。

- [ ] 创建并进入工作区：`mkdir -p ~/projects/my_tiny_webserver && cd ~/projects/my_tiny_webserver`，`pwd` 显示 `/home/<你>/projects/my_tiny_webserver`；
- [ ] 写好 `echo_server.cpp` 与 `makefile` 两个文件，`ls` 能看到这两个文件名；
- [ ] 手工编译：`g++ -Wall -g -o echo_server echo_server.cpp`，无任何报错和警告；
- [ ] `make clean && make`：make 打印出 `g++ -Wall -g -o echo_server echo_server.cpp` 且无报错，`ls` 看到 `echo_server`；
- [ ] 运行：`./echo_server 9006`，终端打印 `echo 服务器已启动，监听端口 9006`；
- [ ] 新开终端查端口：`ss -tlnp | grep 9006`，看到一行包含 `LISTEN` 和 `:9006`，且进程名为 `echo_server`；
- [ ] 单客户端回显：`nc 127.0.0.1 9006` 后输入 `hello` 回车，立即回显 `hello`；再输入 `tiny webserver` 回车，回显 `tiny webserver`；
- [ ] 观察串行处理：保持上一个 `nc` 不断开，再开第二个终端 `nc 127.0.0.1 9006`，输入 `world` 回车，**没有任何回显**；
- [ ] 验证排队恢复：在第一个 `nc` 里按 `Ctrl+C` 断开，观察第二个 `nc` 此时才回显 `world`，且服务器终端打印了 `客户端断开`；
- [ ] 退出服务器：在服务器终端按 `Ctrl+C`，进程退出；再执行 `ss -tlnp | grep 9006`，**无输出**（端口已释放）；
- [ ] 清理产物：`make clean`，`ls` 中不再有 `echo_server`。

## 参考答案对照

本阶段对应仓库 `webserver.cpp` 的 `eventListen()` 中 socket 部分，见 [webserver.cpp](../webserver.cpp) 第 103～133 行。对照如下：

| 仓库写法 | 本阶段写法 | 说明 |
|---|---|---|
| `socket(PF_INET, SOCK_STREAM, 0)` | 相同 | 一致 |
| `assert(m_listenfd >= 0)` | `if (...) error_handling(...)` | 仓库用 `assert`（调试期快速暴露错误），本阶段用 `if` + `perror`，对新手报错信息更友好 |
| `bzero(&address, sizeof(address))` | `memset(&serv_addr, 0, sizeof(serv_addr))` | 效果相同。`bzero` 是 POSIX 老接口，`memset` 是 ISO 标准，更推荐 |
| `address.sin_family = AF_INET` 等三条赋值 | 相同 | 一致 |
| `setsockopt(... SO_LINGER ...)`（109～119 行） | 未写 | **优雅关闭连接**，本阶段省略，[Stage 9](stage-09-integration.md) 讲 |
| `setsockopt(... SO_REUSEADDR ...)`（129 行） | 未写 | 允许端口在 TIME_WAIT 后立即复用，能解决"Address already in use"，[Stage 9](stage-09-integration.md) 讲 |
| `bind` / `listen(m_listenfd, 5)` | 相同 | 一致 |
| 端口来自成员 `m_port`（由 config 传入） | 端口来自 `argv[1]` | 本阶段还没 config，先用命令行参数，[Stage 9](stage-09-integration.md) 恢复 |
| 封装在 `WebServer` 类里 | 裸 `main` + 全局函数 | 本阶段刻意扁平化，降低认知负担 |

本阶段代码是仓库 socket 流程的"浓缩 + 注释版"，读懂它，再看仓库第 103～133 行会非常顺畅。

## 常见问题

1. **`bind() error: Address already in use`（端口被占用）**
   上一次运行还没退出，或端口处于 TIME_WAIT 状态。先 `ss -tlnp | grep 9006` 找到占用进程 `kill` 掉，或换一个端口（如 9007）。治本方案 `SO_REUSEADDR` 到 [Stage 9](stage-09-integration.md) 再讲。

2. **`nc: command not found`（nc 不存在）**
   Ubuntu 22.04 默认可能没装。执行 `sudo apt update && sudo apt install netcat-openbsd`。不想装也可以用 `telnet 127.0.0.1 9006`（同样要先 `sudo apt install telnet`）。

3. **WSL2 下从 Windows 侧连不上 127.0.0.1**
   WSL2 是一台独立的轻量虚拟机，`127.0.0.1` 在"Windows 侧"和"WSL 侧"不是同一张网卡。请**在 WSL 终端里**同时开服务器和 `nc`（本阶段全部在 WSL 内操作即可）；若一定要从 Windows 连，优先用 `localhost`（WSL2 有 localhost 转发），或用 `ip addr` 查到 WSL 的 IP 再连。

4. **`nc` 连上但输入没回显 / 提示连接被拒绝（Connection refused）**
   先确认服务器在跑：`ss -tlnp | grep 9006` 有没有 LISTEN；再确认端口号一致（服务器 9006，客户端也 9006）；最后确认没被防火墙挡：`sudo ufw status`（若 `active`，可 `sudo ufw allow 9006/tcp` 放行）。

5. **`accept() error: ...` 返回 -1**
   `accept` 返回 -1 属偶发（例如被信号打断，errno 为 `EINTR`；或系统 fd 用尽，errno 为 `EMFILE`）。本阶段代码已用 `continue` 容错。若持续报错，用 `perror` 打印出的 errno 到 `man accept` 里查 `ERRORS` 一节定位。

6. **`make: *** missing separator. Stop.`（make 报错）**
   命令行前用的是空格而不是 Tab。把 `$(CXX) ...` 和 `rm -f ...` 两行的缩进改成 Tab 键即可。这是 make 新手第一坑。

7. **客户端异常断开时服务器被 `SIGPIPE` 信号杀死**
   若客户端在服务器 `send` 时突然断开，`send` 可能触发 `SIGPIPE` 信号，默认行为是**终止进程**。本阶段回显循环里 `recv` 返回 0 就会退出，通常不会触发；但正式项目里要 `signal(SIGPIPE, SIG_IGN)` 忽略它。仓库在 `webserver.cpp` 第 150 行正是这么做的（`utils.addsig(SIGPIPE, SIG_IGN)`），到 [Stage 9](stage-09-integration.md) 讲。

## 思考题

1. `recv` 返回 `0` 和返回 `-1` 分别代表什么？为什么 `0` 是"正常断开"而 `-1` 是"出错"？
2. 一个客户端 A 连接后什么都不发，另一个客户端 B 连上后立刻发数据。B 的数据能立刻被回显吗？为什么？
3. `sizeof(buf)` 与 `strlen(buf)` 什么时候相等、什么时候不等？在 echo 循环里为什么用 `str_len`（recv 的返回值）而不是 `strlen(buf)` 来决定 send 多少字节？
4. `listen(serv_sock, 5)` 里的 `5` 是什么含义？如果忙着一个客户端时同时来了 10 个连接，会发生什么？
5. `127.0.0.1` 和 `0.0.0.0` 在本阶段分别扮演什么角色？把服务器的 `INADDR_ANY` 改成 `htonl(INADDR_LOOPBACK)` 后，还能用 `127.0.0.1` 连吗？
6. 为什么 `send` 时传 `str_len` 而不是 `str_len + 1`？如果把 `'\0'` 也发过去，客户端会看到什么？

## 下一步

你已经跑通了 TCP 服务器的完整链路，也亲身体验了"单线程串行"的痛点。下一步就去解决它：引入多线程与线程池，让多个客户端**并发**回显、互不阻塞。

继续阅读 [Stage 3：多线程与线程池](stage-03-threadpool.md)。
