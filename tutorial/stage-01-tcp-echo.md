# Stage 1 单线程 TCP echo 服务器

> 第一个网络程序!学完它,你就掌握了 socket 编程的完整流程——后面所有阶段都建立在这几个系统调用之上。

## 1. 本阶段目标

- [ ] 理解 `socket / bind / listen / accept / read / write / close` 全流程
- [ ] 写出能跑的单线程阻塞 TCP 服务器
- [ ] 用 `nc` 连接,输入什么就回显什么
- [ ] 会用 gdb 观察 `accept` 阻塞

**最终效果:** 一个终端启动服务器,另一个终端 `nc 127.0.0.1 9006`,你输入一行字,服务器原样回给你。

## 2. 前置知识

- C1:指针、`struct`
- 新增概念:文件描述符、网络字节序(下面边写边讲)
- 不需要 C5 的线程——本阶段是单线程、阻塞式

## 3. socket 编程全流程

一次网络通信,服务器要做七件事:

```text
socket() 创建套接字
   │
   ▼
bind()   绑定 IP 和端口   ← 告诉系统"我在 9006 端口等"
   │
   ▼
listen() 开始监听         ← 系统开始接受外面的连接请求
   │
   ▼
accept() 接受连接         ← 阻塞!等一个客户端连进来,返回"连接描述符"
   │
   ▼
read()   读客户端数据     ← 阻塞!等客户端发数据
   │
   ▼
write()  写回客户端
   │
   ▼
close()  关闭连接
```

**几个必须懂的概念:**

- **文件描述符(fd)**:Linux 里"一切皆文件",网络连接也是一个整数编号。`socket()` 返回 `listenfd`(监听用的),`accept()` 返回 `connfd`(和某个客户端通信用的)。我们之后所有读写都对着 `connfd`
- **阻塞**:`accept()`、`read()` 调用时,如果没有连接/没有数据,程序就**停在那里等**,直到有东西来才继续。这是本阶段的运行方式,Stage 4 的 epoll 会颠覆它
- **网络字节序**:整数在内存里,不同机器大端小端不同。网络传输统一用大端。`htons(PORT)` 把端口转成网络字节序(`h`=host主机,`to`, `n`=network网络,`s`=short短整数)
- **`sockaddr_in`**:存 IP + 端口的结构体,`sin_family` 填协议族,`sin_addr` 存 IP,`sin_port` 存端口。`memset` 清零是标准操作

## 4. 完整代码

在 `my_tiny_webserver/` 目录下新建 `main.cpp`(本阶段就这一个文件):

```cpp
// main.cpp —— 单线程阻塞 TCP echo 服务器
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const int PORT = 9006;

int main() {
    // 1. 创建 socket(文件描述符)
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("socket");
        return 1;
    }

    // 2. 绑定地址和端口
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;     // 监听所有网卡
    addr.sin_port = htons(PORT);           // 端口(网络字节序)
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    // 3. 开始监听
    if (listen(listenfd, 5) < 0) {
        perror("listen");
        return 1;
    }
    std::cout << "服务器已启动, 监听端口 " << PORT << std::endl;

    // 4. 循环接受连接
    while (true) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd, (struct sockaddr *)&client, &len);
        if (connfd < 0) {
            perror("accept");
            continue;
        }
        std::cout << "接受到连接: " << inet_ntoa(client.sin_addr)
                  << ":" << ntohs(client.sin_port) << std::endl;

        // 5. 回显:读到什么发回什么
        char buf[1024];
        while (true) {
            ssize_t n = read(connfd, buf, sizeof(buf));
            if (n <= 0) break;              // 连接关闭或出错
            write(connfd, buf, n);          // 原样发回
        }
        close(connfd);
        std::cout << "连接关闭" << std::endl;
    }

    close(listenfd);
    return 0;
}
```

**逐段讲解:**

| 代码 | 作用 | 细节 |
|---|---|---|
| `socket(AF_INET, SOCK_STREAM, 0)` | 创建 TCP 套接字 | `AF_INET` = IPv4,`SOCK_STREAM` = TCP |
| `bind(...)` | 把端口 9006 绑定到这个套接字 | 不 bind 系统会随机给端口 |
| `listen(listenfd, 5)` | 开始监听 | `5` 是等待队列长度 |
| `accept(...)` | **阻塞**等待客户端连入 | 返回 `connfd`,专用于和这个客户端通信 |
| `inet_ntoa(client.sin_addr)` | 把客户端 IP 转成可打印字符串 | `n`(网络)→`a`(ASCII) |
| `read(connfd, buf, 1024)` | 读客户端数据,返回读到的字节数 | 返回 0 = 客户端关闭,负数 = 出错 |
| `write(connfd, buf, n)` | 把读到的 n 个字节原样写回 | 这就是"回显" |
| `while(true)` 循环 | 服务器永不退出,一直接新连接 | 一个连接处理完,回到 accept 等下一个 |

## 5. 编译与运行

### 方式一:裸 g++(看背后发生了什么)

```bash
cd ~/TinyWebServer/my_tiny_webserver
g++ -Wall -o server main.cpp
./server
```

> 从 C5 起你知道了多线程要 `-pthread`。本阶段还没用线程,所以不需要;到 Stage 3 会加上。

### 方式二:CMake(本教程的正式方式)

新建 `my_tiny_webserver/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp)
```

然后构建运行:

```bash
cmake -S . -B build
cmake --build build
./build/server
```

**预期输出:**

```text
服务器已启动, 监听端口 9006
```

> 从现在起,每阶段都会更新这个 `CMakeLists.txt`(加源文件、加链接库),它跟着你的项目一起长大——这也正是 CMake 的教学目标之一。

### 测试回显

保持服务器运行,**另开一个 WSL 终端**:

```bash
nc 127.0.0.1 9006
```

输入任意一行字回车,比如 `hello`,会看到原样回显。按 `Ctrl+D` 结束连接。

**预期:服务器终端输出**

```text
服务器已启动, 监听端口 9006
接受到连接: 127.0.0.1:46190
连接关闭
```

> 没有 `nc`?`sudo apt install -y netcat-openbsd` 装一下。Windows 上也可以用 PowerShell 的 `Test-NetConnection` 或 Python 测试。

## 6. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `g++ -Wall -o server main.cpp && ./server` | 输出 `服务器已启动, 监听端口 9006` | ☐ |
| 2 | `cmake -S . -B build && cmake --build build` | 构建成功,`./build/server` 同样能启动 | ☐ |
| 3 | 另开终端 `nc 127.0.0.1 9006`,输入 `hello` | 回显 `hello` | ☐ |
| 4 | nc 里按 `Ctrl+D` | 服务器日志出现 `连接关闭` | ☐ |
| 5 | 服务器正在运行时,再开第二个 nc | 第二个连接也能正常回显(逐个处理) | ☐ |
| 6 | **杀掉服务器再启动**(Ctrl+C 后重启) | 能正常启动(无 bind 报错) | ☐ |

## 7. 调试技巧

### 用 gdb 观察 accept 阻塞

`accept()` 是"阻塞"的——它在那里等,这正是理解它的关键。用 gdb 看它等在哪:

```bash
gdb ./server
```

```text
(gdb) break accept          ← 在 accept 函数打断点
Breakpoint 1 at 0x1180
(gdb) run
...                         ← 程序一路跑到 accept 处停住,等你
(gdb) next                  ← 另开终端 nc 连进来后再按
```

连接进来后,`next` 会执行完 `accept`,返回一个非负的 `connfd`——你能亲眼看到"阻塞 → 被连接唤醒"的过程。

### 常见调试命令回顾

| 命令 | 用途 |
|---|---|
| `break accept` | 在函数打断点 |
| `run` / `continue`(`c`) | 启动 / 继续 |
| `print connfd` | 看 accept 返回值 |
| `backtrace`(`bt`) | 看调用栈 |

> 完整命令表见附录 [gdb 速查表](annex-gdb-cheatsheet.md)。

## 8. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| `bind: Address already in use` | 端口被上一个没退干净的 server 占着 | 找到并杀掉:`pkill -f "./server"`;或换端口 |
| 服务器启动后 nc 连接立刻失败 | 服务器没运行 / 端口不对 | 确认 `./server` 在运行、连的是 9006 |
| `socket: Operation not permitted` | 个别环境限制 | 确保在 WSL Ubuntu 里,不是 Windows 终端 |
| nc 显示 `Connection refused` | 服务器没监听 | 看服务器终端有没有报错输出 |
| 输入中文回显乱码 | 终端编码 | 本阶段先测英文,编码问题后面不涉及 |

## 9. 与原项目对照

本阶段是最简"单人版":原项目做了同样的事,但用 **epoll 事件循环 + 线程池** 同时处理成千上万个连接。对照关系:

| 本阶段 | 原项目对应 |
|---|---|
| `socket / bind / listen` | `webserver.cpp` 的 `eventListen()`(Stage 9 见) |
| `accept` | `webserver.cpp` 的 `dealclientdata()`(Stage 4 见) |
| `read → write` 回显 | `http_conn.cpp` 的 `read_once` → `write`(Stage 5 见) |

> 你现在写的 `socket/bind/listen/accept` 和原项目是**同一批系统调用**,只是原项目把它们组织得更复杂。先掌握本阶段的朴素版本,后面看原项目就不慌了。

## 10. 下一步

进入 **[Stage 2 单线程 HTTP 静态服务器](stage-02-http-static.md)**——把"回显数据"升级成"解析 HTTP 请求、返回网页",**在浏览器里看到自己的网页**。
