# Stage 1：socket 与 TCP echo 服务器

> 🎯 **本阶段目标**：写一个 TCP echo 服务器 —— 客户端发什么，服务器原样弹回什么。
> 代码只有几十行，但 socket 编程的核心套路（socket/bind/listen/accept/read/write）全在里面。
> 后面整个 TinyWebServer 都是在这几个系统调用之上做加法。

## 📚 理论铺垫

### 1.1 服务器和客户端在「系统调用层面」做什么

网络编程的本质是**进程间通信（跨机器）**。操作系统提供 socket 这套 API，
服务器端固定走这 5 步：

```
socket()  创建监听套接字（file descriptor，简称 fd）   ← 拿到一个"电话机"
bind()    绑定 IP 和端口                                 ← 插上电话线，占号
listen()  开始监听                                       ← 开机等来电
accept()  接受一个连接，返回新的 connfd                  ← 接起电话，得到一条专线
read/write 通过 connfd 收发数据                          ← 通话
close()   关闭                                           ← 挂断
```

客户端更简单：`socket() → connect() → read/write → close()`。

**关键概念：fd（文件描述符）**。Linux 中一切皆文件，socket 也是一个 fd，
对它 read/write 就像读写文件。TinyWebServer 里的 `m_listenfd`、`connfd`、`m_epollfd` 全是 fd。

### 1.2 sockaddr_in：地址的结构体

```cpp
struct sockaddr_in {
    sa_family_t    sin_family;  // 地址族，IPv4 填 AF_INET
    in_port_t      sin_port;    // 端口号（注意：网络字节序！）
    struct in_addr sin_addr;    // IP 地址（网络字节序）
};
```

**字节序坑点**：x86 CPU 是小端序，网络协议是大端序，所以端口和 IP 必须转换：

```cpp
addr.sin_port = htons(9006);           // host to network short
addr.sin_addr.s_addr = htonl(INADDR_ANY);  // INADDR_ANY = 0.0.0.0，监听所有网卡
```

### 1.3 阻塞式 I/O（本阶段用它，最简单）

默认情况下 `accept()`、`read()` 是**阻塞**的：
没有连接进来，`accept` 就一直等；没有数据到达，`read` 就一直等。
这意味着这种写法**同一时刻只能服务一个客户端** —— 先别管，Stage 3/4 会解决它。
现在的目标是：把 socket 套路跑通。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| 头文件包含 `#include` | `sys/socket.h` 等系统头文件 |
| `memset` / 结构体初始化 | 清零 sockaddr_in |
| 错误处理惯例：返回值 < 0 + `perror` | 每个系统调用之后 |
| `char buf[N]` 与 `std::string` 转换 | 收到数据后打印 |

## 🔨 动手实现

在 `my_tiny_webserver/` 下创建练习目录：

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer/my_tiny_webserver
mkdir -p stage1_tcp && cd stage1_tcp
```

### 1.4 echo 服务器 `echo_server.cpp`

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

const int PORT = 9006;        // 与 TinyWebServer 默认端口一致
const int BUF_SIZE = 1024;

int main() {
    // 1. 创建监听 socket。SOCK_STREAM = TCP
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    // 端口复用：服务器重启后不用等 TIME_WAIT 结束
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 2. bind：绑定端口
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }

    // 3. listen：5 是"等待 accept 的连接队列"长度
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }
    printf("echo server listening on port %d ...\n", PORT);

    while (true) {
        // 4. accept：来一个连接，返回新 fd
        sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int connfd = accept(listenfd, (sockaddr*)&client, &client_len);
        if (connfd < 0) { perror("accept"); continue; }

        printf("new client: %s:%d\n",
               inet_ntoa(client.sin_addr), ntohs(client.sin_port));

        // 5. echo 循环：读到什么写回什么
        char buf[BUF_SIZE];
        while (true) {
            memset(buf, 0, BUF_SIZE);
            int n = read(connfd, buf, BUF_SIZE - 1);
            if (n < 0)  { perror("read"); break; }
            if (n == 0) {                 // 对方关闭了连接
                printf("client disconnected\n"); break;
            }
            write(connfd, buf, n);
        }
        close(connfd);
    }
    close(listenfd);
    return 0;
}
```

### 1.5 编译

本阶段一个文件搞定，先不用 CMake，直接 g++：

```bash
g++ -g -Wall -o echo_server echo_server.cpp
```

> ⚠️ 注意：从 `socket()` 到 `read()` 每个调用我都检查了返回值。
> **网络编程里不检查返回值是万恶之源** —— 原始 TinyWebServer 大量使用 `assert`，
> 效果类似：出错立刻暴露，而不是带病运行。

## ✅ 验证

**验证 1：启动服务器**

```bash
./echo_server
# 期望输出：echo server listening on port 9006 ...
```

**验证 2：用 telnet/nc 当客户端**（新开一个 WSL 终端）

```bash
# 二选一，nc 没有的话 sudo apt install netcat
nc 127.0.0.1 9006
```

输入 `hello tinywebserver` 回车 —— 服务器应该把同一行弹回来。
多打几行都成立，Ctrl+C 退出后服务器打印 `client disconnected`。

**验证 3：观察连接的建立过程**

在第二个终端执行，可以看到 TCP 连接状态：

```bash
ss -tnp | grep 9006
# 连接建立时能看到 ESTABLISHED 状态的连接
```

**验证 4（体会阻塞模型的局限）：连两个客户端**

再开第三个终端也 `nc 127.0.0.1 9006`，输入内容 —— **没有回显**！
因为服务器阻塞在第一个连接的 `read` 上，根本没空 accept 第二个连接。

> 🔑 记住这个实验。Stage 3（线程池）和 Stage 4（epoll）就是为了消灭这个缺陷，
> 到那时你会明白它们各自是怎么解决的。

## 🐛 常见问题

**Q1: bind 报 `Address already in use`？**
上一次的服务器没退干净（或处于 TIME_WAIT）。`ss -tnlp | grep 9006` 找到进程 kill 掉。
代码里的 `SO_REUSEADDR` 就是为了缓解这个问题，没有它会经常遇到。

**Q2: 客户端连不上，提示 Connection refused？**
服务器没启动，或者端口不对。先确认 `ss -tlnp | grep 9006` 里能看到 LISTEN 状态。

**Q3: Windows 上的浏览器/telnet 能连 WSL 里的服务器吗？**
能。WSL2 默认会把 localhost 转发：Windows 上访问 `127.0.0.1:9006` 即可到达 WSL 里的服务器。
这对 Stage 2 用浏览器验证至关重要。

## 🤔 思考与练习

1. 把 `read` 的返回值 `n` 也打印出来（`printf("read %d bytes: %s", n, buf)`），
   观察 TCP 是**流协议**：一次发 2000 字节可能被拆成多次 read。这就是后面 HTTP 解析
   必须用状态机「攒数据」的根本原因。
2. 注释掉 `SO_REUSEADDR`，服务器 Ctrl+C 后立刻重启，观察 bind 报错。
3. 用 gdb 调试：在 `accept` 处打断点，用 nc 连上后 `bt` 看调用栈，`print client.sin_port`。
4. 思考：`listen(listenfd, 5)` 里的 5 是什么？试试改成 1，用多个 nc 同时连，观察现象
   （提示：`ss -tn state syn-recv`）。

---

➡️ 下一阶段：[Stage 2：单线程 HTTP 静态服务器](stage-02-simple-http.md)
