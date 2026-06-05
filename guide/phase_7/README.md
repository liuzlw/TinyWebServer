# Phase 7 —— 非阻塞 Socket + epoll 事件驱动

## 目标

理解 Linux TCP 网络编程的核心模式：socket → bind → listen → accept → epoll 事件循环。同时掌握 LT（水平触发）与 ET（边缘触发）的本质区别。

**可见结果：** 一个单线程 echo 服务器。telnet 连接后，输入任意文本，服务器原样返回。多个 telnet 同时连接，服务器正常处理所有客户端。

---

## 前置知识

- Phase 1 的 `locker.h`（理解非阻塞概念即可）
- 知道 IP 地址 + 端口 = 网络服务的"门牌号"

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **strace** | strace -e trace=epoll_wait 追踪系统调用，观察 epoll 唤醒频率 |

---

## 分步实现

### Step 1：TCP 三次握手与 socket API

一个 TCP 服务器的标准启动流程：

```
socket()          创建 socket（分配 fd）
   ↓
bind()            绑定 IP:Port（告诉 OS："我占这个端口"）
   ↓
listen()          监听（告诉 OS："我可以接受连接了"）
   ↓
accept()          接受连接（阻塞等待客户端 SYN）
   ↓
recv()/send()     收发数据
   ↓
close()           关闭连接
```

对应代码：

```cpp
// 1. 创建 socket
int listenfd = socket(PF_INET, SOCK_STREAM, 0);

// 2. 设置端口复用（避免重启时 "Address already in use"）
int flag = 1;
setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

// 3. 绑定
struct sockaddr_in addr;
bzero(&addr, sizeof(addr));
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 监听所有网卡
addr.sin_port = htons(9006);               // 端口 9006
bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));

// 4. 监听（backlog = 5：已完成三次握手的队列长度）
listen(listenfd, 5);
```

### Step 2：阻塞 vs 非阻塞

默认 socket 是**阻塞**的：
- `accept` 在没有新连接时阻塞
- `recv` 在没有数据时阻塞
- `send` 在发送缓冲区满时阻塞

如果只有一个客户端，阻塞没问题。但服务器需要同时处理**成千上万个**连接——一个线程阻塞在 A 的 `recv` 上时，B 的数据到了也没人收。

**解决方案：非阻塞 + IO 多路复用。**

```cpp
// 设置非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
```

非阻塞 socket 上：
- `accept` 无连接时返回 -1，`errno = EAGAIN`
- `recv` 无数据时返回 -1，`errno = EAGAIN`
- `send` 缓冲区满时返回 -1，`errno = EAGAIN`

### Step 3：epoll —— IO 多路复用

epoll 是 Linux 上最高效的 IO 多路复用机制。核心是三个系统调用：

```cpp
// 1. 创建 epoll 实例
int epollfd = epoll_create(5);  // 参数在 Linux 2.6.8 后被忽略

// 2. 注册要监视的 fd
struct epoll_event ev;
ev.data.fd = listenfd;
ev.events = EPOLLIN;            // 监视可读事件
epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev);

// 3. 等待事件
struct epoll_event events[MAX_EVENTS];
int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
// 返回：就绪的事件数量。events[] 填充了就绪的事件

// 4. 处理就绪事件
for (int i = 0; i < nfds; ++i) {
    if (events[i].data.fd == listenfd) {
        // 新连接到达
        int connfd = accept(listenfd, ...);
        setnonblocking(connfd);
        // 把 connfd 也注册到 epoll
        epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev);
    } else {
        // 已有连接的数据到达
        recv(events[i].data.fd, ...);
        // 处理数据...
    }
}
```

`epoll_wait` 只返回"就绪"的 fd，而不是遍历所有 fd 检查（那是 `select` 的方式）。这就是 epoll 比 select 快的原因。

### Step 4：LT vs ET —— 这是面试高频考点

| | LT（水平触发） | ET（边缘触发） |
|---|---|---|
| **触发条件** | fd 上**仍然**有数据可读，就一直通知 | fd 从"不可读"变成"可读"的**那一刻**通知一次 |
| **读策略** | 读一次也行，分多次也行 | **必须循环读，直到返回 EAGAIN** |
| **代码复杂度** | 简单 | 需要循环 + 非阻塞 |
| **适用场景** | 简单场景，稳妥 | 高性能场景，减少 epoll_wait 唤醒次数 |

**为什么 ET 必须循环读？**

```
假设 socket 收到了 2000 字节

LT: epoll_wait 返回 → 你读 1024 字节 → 还剩 976 字节 → 下次 epoll_wait 还会返回
ET: epoll_wait 返回 → 你只读 1024 字节 → 还剩 976 字节 → 
    但 socket 没有再收到新数据 → 没有新的"边缘"事件 → 再也不通知了！
    那 976 字节永远丢失。
```

**正确的 ET 读：**

```cpp
while (true) {
    int n = recv(fd, buf, size, 0);
    if (n == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;        // 读完
        // 真正的错误...
    } else if (n == 0) {
        // 对端关闭连接
    }
    // 处理这 n 字节
}
```

### Step 5：EPOLLONESHOT

```cpp
ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
```

`EPOLLONESHOT` 的含义：触发一次后，该 fd 自动从 epoll 监听中"脱钩"，直到程序显式 `epoll_ctl(EPOLL_CTL_MOD)` 重新注册。

**用途：** 在多线程环境下，保证同一个 fd 同一时刻只有一个线程在处理。线程 A 拿到事件 → fd 脱钩 → 线程 B 拿不到同 fd 的事件 → 线程 A 处理完 → 重新注册 MOD。

如果不加 EPOLLONESHOT，两个线程可能同时处理同一个 fd 的数据，导致响应乱序。

### Step 6：完整 echo 服务器

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#define MAX_EVENTS 1024
#define BUF_SIZE 4096

int setnonblocking(int fd) {
    int old = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old | O_NONBLOCK);
    return old;
}

void addfd(int epollfd, int fd, bool oneshot) {
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLET;
    if (oneshot) ev.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    setnonblocking(fd);
}

int main() {
    // --- socket / bind / listen ---
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(9006);
    bind(listenfd, (sockaddr*)&addr, sizeof(addr));
    listen(listenfd, 5);

    // --- epoll ---
    int epollfd = epoll_create(5);
    addfd(epollfd, listenfd, false);  // listenfd 不用 EPOLLONESHOT

    epoll_event events[MAX_EVENTS];
    char buf[BUF_SIZE];

    while (true) {
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == listenfd) {
                // --- 新连接 ---
                sockaddr_in client;
                socklen_t len = sizeof(client);
                int connfd = accept(listenfd, (sockaddr*)&client, &len);
                if (connfd < 0) continue;

                addfd(epollfd, connfd, true);  // connfd 加 EPOLLONESHOT
                printf("New connection: fd=%d\n", connfd);

            } else if (events[i].events & EPOLLIN) {
                // --- 可读 ---
                while (true) {
                    int n = recv(fd, buf, BUF_SIZE, 0);
                    if (n == -1) {
                        if (errno == EAGAIN) break;
                        close(fd); break;
                    } else if (n == 0) {
                        printf("Close: fd=%d\n", fd);
                        close(fd); break;
                    } else {
                        // echo: 原样返回
                        send(fd, buf, n, 0);
                    }
                }
                // ET + EPOLLONESHOT：处理完后重新注册
                epoll_event ev;
                ev.data.fd = fd;
                ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev);

            } else {
                // EPOLLERR / EPOLLHUP 等
                printf("Error: fd=%d\n", fd);
                close(fd);
            }
        }
    }
    close(listenfd);
    close(epollfd);
    return 0;
}
```

### Step 7：测试

```bash
# 终端 1：启动服务器
./build/echo_server

# 终端 2：用 telnet 连接
telnet 127.0.0.1 9006
# 输入 "hello" 回车 → 返回 "hello"
# 输入 "world" → 返回 "world"

# 终端 3：再开一个 telnet，两个同时交互
telnet 127.0.0.1 9006

# 终端 4：用 curl 测试
curl -v http://127.0.0.1:9006/
```

### Step 8：`SO_LINGER` 优雅关闭

```cpp
struct linger tmp = {1, 1};  // l_onoff=1, l_linger=1
setsockopt(fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
close(fd);
```

| 配置 | close 行为 |
|------|-----------|
| 默认 | close 立即返回，内核继续发送缓冲区中剩余数据（四次挥手正常） |
| `{0, *}` | 禁用 SO_LINGER，同默认 |
| `{1, 0}` | 直接发 RST 关闭，丢弃缓冲区数据 |
| `{1, N}` | close 阻塞最多 N 秒，尝试发完缓冲区数据 |

---

## 验证方法

- [ ] telnet 连接后 echo 正常
- [ ] 多个 telnet 同时连接，各自独立交互
- [ ] 关闭 telnet，服务器正确释放 fd
- [ ] 用 `strace -e trace=epoll_wait ./echo_server` 观察 epoll_wait 的唤醒频率

---

## 踩坑记录

1. **`EPOLLERR` 和 `EPOLLHUP` 无需显式注册。** 内核会自动通知这些事件，只需要在处理时检查即可。

2. **`EPOLLONESHOT` + ET 忘记重新 MOD。** 这会导致 fd 永远不再被触发。记住：EPOLLONESHOT 触发后 fd 自动脱钩，必须 `epoll_ctl(MOD)` 重新装上。

3. **ET 模式下 `accept` 也要循环。** 如果在高并发下，一次 `epoll_wait` 返回时可能有多个连接同时在等待。ET 模式下必须循环 `accept` 直到返回 EAGAIN。

4. **`listen` 的 backlog。** 并不是连接数的上限。实际最大连接数受 `somaxconn` 内核参数控制：`cat /proc/sys/net/core/somaxconn`。

---

## 阶段小结

你掌握了 Linux 网络编程的核心技术栈：
- socket / bind / listen / accept
- fcntl 非阻塞设置
- epoll_create / epoll_ctl / epoll_wait
- LT vs ET 的本质区别
- EPOLLONESHOT 的多线程安全保障
- SO_LINGER 优雅关闭

下一阶段也是最重要的阶段：**服务集成**——把 Phase 1-7 的所有模块组装成一个完整的 Web 服务器。
