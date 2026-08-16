# Stage 4：epoll 事件驱动

> 上一阶段我们用"一个连接一个线程"的方式解决了并发：每个客户端来了就丢给一个工作线程。这种方式在几十个连接时还好，但 Web 服务器动辄上万连接，每连接一个线程会让内存和上下文切换直接爆炸。这一阶段我们要掌握 Linux 的高性能并发利器——**epoll**：让**一个主线程**同时管理成千上万个连接，并在最后把 epoll 与上一阶段的线程池结合起来，得到本项目并发模型的雏形。

学完本阶段你能：
- 讲清楚五种 IO 模型，以及 select / poll / epoll 三者的区别；
- 独立写出 epoll 版的 echo 服务器，理解 LT 与 ET 的编程差异；
- 理解 EPOLLONESHOT 为什么要"用一次重新上一次"；
- 理解 Reactor 与 Proactor 两种并发模型，并复现本项目的 **Proactor 雏形**。

---

## 前置要求

- 已完成 [Stage 2](stage-02-socket-echo.md)（socket、recv/send）与 [Stage 3](stage-03-threadpool.md)（线程池、锁、信号量）。
- 工作区 `my_tiny_webserver/` 当前文件：
  - `lock/locker.h` —— 与仓库一致（`sem` / `locker` / `cond`）；
  - `threadpool/threadpool.h` —— Stage 3 简化版线程池；
  - `echo_server.cpp` —— 线程池版 echo 服务器（`task` 类含 `m_sockfd` 与缓冲区，`process()` 做 recv/send 回显）；
  - `makefile`。
- 本阶段**只改 `echo_server.cpp` 和 `makefile`**，`lock/`、`threadpool/` 不动。
- 建议先快速回看 Stage 2 里"阻塞 recv 会一直等"的现象，本阶段要频繁对比它。

---

## 理论学习

### 一、为什么要 epoll：一个线程管所有连接

先回忆 Stage 2 / Stage 3 的做法：

- Stage 2（阻塞 echo）：`accept` 拿到连接后，`while` 循环里 `recv`，没数据就**原地卡住**，这期间别的客户端根本连不进来——因为只有一个线程在服务所有人。
- Stage 3（线程池）：主线程 `accept` 后把 `connfd` 丢给线程池，工作线程去 `recv`。并发是上来了，但代价是**每个连接占用一个线程**：1 万个连接就要 1 万个线程，每个线程有独立的栈（默认 8 MB），光栈就是 80 GB，还伴随海量上下文切换。

我们希望的是：**一个线程，同时盯着一万个连接，谁有数据就处理谁，没数据的别挡道**。这正是 IO 多路复用（IO multiplexing）要解决的问题，而 Linux 上最强大的多路复用 API 就是 `epoll`。

### 二、五种 IO 模型（重点：前三种）

Linux 下一次完整的"读数据"要经历两步：**等数据就绪** + **把数据从内核拷到用户态**。按"等待阶段怎么等"，IO 分五种模型：

| 模型 | 等数据时进程在干嘛 | 一句话 |
|---|---|---|
| 阻塞 IO | 阻塞，`recv` 不返回 | 最常见，Stage 2 用的就是它 |
| 非阻塞 IO | 忙轮询，`recv` 立刻返回 `EAGAIN`，自己循环重试 | 不阻塞但**空耗 CPU** |
| IO 多路复用 | 阻塞在 `select`/`poll`/`epoll_wait` 上，一个线程同时等多个 fd | **本阶段主角** |
| 信号驱动 IO | 数据就绪时内核发信号通知 | 少见，本项目不用 |
| 异步 IO | 连"拷贝"都由内核完成，完成后再通知 | 最彻底，本项目不用 |

关键对比——**阻塞 vs 非阻塞 vs 多路复用**：

```text
阻塞 IO：        recv(fd) ──► 没数据就睡，直到有数据才返回
非阻塞 IO：      recv(fd) ──► 没数据立刻返回 -1（errno=EAGAIN），程序自己 while 循环重试
IO 多路复用：    epoll_wait() ──► 一个线程同时睡等 N 个 fd，谁就绪就返回谁
```

多路复用可以理解为"把 N 个阻塞 IO 的等待，合并成一次等待"。它本身并不比阻塞更快，而是**用更少的线程管理更多连接**。

### 三、select / poll / epoll 对比

| 维度 | select | poll | epoll |
|---|---|---|---|
| 每次调用是否要把 fd 集合从用户态**拷贝**到内核 | 是（整个 fd_set） | 是（整个 pollfd 数组） | 否（fd 只注册一次，保存在内核事件表） |
| 内核如何找就绪 fd | 遍历全部 fd | 遍历全部 fd | 就绪 fd 直接挂在**就绪链表**上，`O(1)` 取出 |
| 最大 fd 数 | 有上限（`FD_SETSIZE`，通常 1024） | 无硬上限，但线性扫描慢 | 无上限（受系统 `fs.file-max` 限制） |
| 触发方式 | 水平触发 | 水平触发 | **水平 + 边沿**两种 |
| 监听大量连接时 | 慢 | 慢 | 快，本项目用这个 |

一句话记忆：**select/poll 每次都要"重新登记并全量扫描"，epoll 是"登记一次，就绪回调"**。

### 四、epoll 三个核心函数

```c
int epoll_create(int size);          // 创建一个 epoll 实例（内核事件表），返回其 fd
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);  // 向事件表增删改 fd
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout); // 等待就绪
```

- `epoll_create`：`size` 在新内核里只作提示，返回一个 `epollfd`（它本身也是一个 fd，用完 `close`）。
- `epoll_ctl`：`op` 有三种取值：
  - `EPOLL_CTL_ADD` —— 把 `fd` 加入事件表；
  - `EPOLL_CTL_MOD` —— 修改 `fd` 在事件表中监听的事件；
  - `EPOLL_CTL_DEL` —— 把 `fd` 从事件表删除。
- `epoll_wait`：阻塞等待，返回就绪事件个数 `n`，就绪事件被写入 `events[0..n-1]`；`timeout=-1` 表示永久阻塞。

```text
         应用进程
            │  epoll_create() 创建内核事件表
            ▼
      ┌──────────────┐
      │  epoll 事件表 │◄──── epoll_ctl(ADD/MOD/DEL) 增删改要监听的 fd 和事件
      └──────────────┘
            ▲
            │ 内核把"已就绪"的 fd 串进就绪链表
            │
      epoll_wait() 把就绪事件拷贝进 events 数组并返回
            │
            ▼
      应用 for 循环遍历 events，逐个 accept / recv / send
```

### 五、事件类型（`epoll_event.events` 的位标志）

`struct epoll_event` 里最常用的是两个字段：`events`（监听哪些事件）和 `data`（用户自定义数据，通常存 `fd`）。

| 标志 | 含义 |
|---|---|
| `EPOLLIN` | fd 可读（有数据到达，或对方关闭时也会触发） |
| `EPOLLOUT` | fd 可写（发送缓冲有空闲） |
| `EPOLLET` | 边沿触发（默认是水平触发） |
| `EPOLLONESHOT` | 触发一次后自动"下枪"，需 `modfd` 重新注册 |
| `EPOLLRDHUP` | 对端关闭连接或半关闭写端（`shutdown(SHUT_WR)`） |
| `EPOLLHUP` | 连接挂起（对端直接关闭） |
| `EPOLLERR` | 连接出错 |

`EPOLLRDHUP` / `EPOLLHUP` / `EPOLLERR` 会被内核**自动**加入 `events`，不需要我们主动注册；判断时用位运算 `events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)`。

### 六、LT vs ET 深入（本阶段最重要的一节）

**水平触发（Level Triggered, LT）**：只要 fd 的读缓冲里**还有数据**，`epoll_wait` 就会**反复**返回这个事件，催你读到空为止。

**边沿触发（Edge Triggered, ET）**：只有当 fd 的读缓冲**由空变非空**（数据到达的那条"边"）时，才通知**一次**。之后哪怕缓冲里还剩 1 万字节没读，`epoll_wait` 也不再提醒你。

```text
LT：缓冲区里有数据就一直催
        ┌───────────┐
内核缓冲 │■■■■■■■■■■│  读走 4 字节，还剩 6 字节
        └───────────┘
   └──► epoll_wait 再次返回 EPOLLIN，直到读空为止

ET：只在"空 ──► 非空"那一刻通知一次
     空  ──数据到达──►  非空     ← 只在这一刻通知一次
     之后剩下多少数据都不再提醒，必须自己一次读完
```

**为什么 ET 必须"循环读到 EAGAIN"**：因为 ET 只提醒一次，如果你只 `recv` 一次，读缓冲里剩下的数据就"卡"住了（没有新数据到达就没有新"边"，永远等不来第二次通知），表现为**丢数据**。正确做法是拿到 EPOLLIN 后，用一个 `while` 循环反复 `recv`，直到 `recv` 返回 `-1` 且 `errno == EAGAIN`（读空了）才停。

**为什么 ET 必须配合非阻塞 fd**：阻塞 fd 在"读空之后再来一次 recv"时不会返回，而是**永久睡死**——循环永远走不到"读空就退出"。而把 fd 设为非阻塞后，读空时 `recv` 立刻返回 `-1`、`errno=EAGAIN`，循环据此退出。所以 ET 的两条铁律是：

1. fd 设为非阻塞（`fcntl(fd, F_SETFL, O_NONBLOCK)`）；
2. 每次就绪都循环读/写，直到 `EAGAIN`。

**一句话总结**：LT 容错性好（少读一次下次还会提醒），ET 效率高（少一次内核通知、减少事件触发次数），但要写对循环和非阻塞。

### 七、Reactor vs Proactor

这两个词描述"**谁负责读数据**"，直接决定主线程和线程池怎么分工：

```text
Reactor（反应器）：主线程只"分发"，不碰数据
   主线程 epoll_wait ──► 发现 EPOLLIN ──► 把 fd 交给线程池 ──► 工作线程 recv + 处理 + send

Proactor（前摄器）：主线程替工作线程把数据"读好"，工作线程只做业务
   主线程 epoll_wait ──► 自己 recv 读好数据 ──► 把"已读好的数据"交给线程池 ──► 工作线程只 send
```

结合本项目：仓库 `webserver.cpp` 的 `dealwithread` 里有两个分支——`m_actormodel == 1` 是 Reactor（`append` 之后工作线程自己在 `run()` 里 `read_once()`），`else` 是 Proactor（主线程先 `read_once()` 再 `append_p`）。本阶段先只学概念并复现 **Proactor 分支的雏形**，完整实现留到 [Stage 9](stage-09-integration.md)。

---

## 本阶段 C++ 知识点

### 1. 枚举 `enum`

把一组有名字的整数常量捆在一起，比满屏的裸 `0/1/2` 可读得多：

```cpp
enum { TRIG_LT = 0, TRIG_ET = 1 };   // 触发模式：水平 / 边沿
const int TRIGMODE = TRIG_ET;        // 改这一行即可切换
```

注意：`epoll` 的 `EPOLLIN`、`EPOLLET` 这些其实是 `<sys/epoll.h>` 里的 `#define` 宏，不是 C++ 枚举；但"用一组有意义的标志位 + 位运算组合"正是枚举的典型用法。Stage 5 的 `http_conn` 会用大量真正的 `enum`（`CHECK_STATE`、`LINE_STATUS` 等），本阶段先混个脸熟。

### 2. 位运算 `|` `&` `~`

事件标志用二进制的一位（bit）表示"有 / 无"，于是：

```cpp
event.events = EPOLLIN | EPOLLRDHUP;     // 或 |：把两个标志"并"起来
if (1 == TRIGMode)
    event.events |= EPOLLET;             // |= ：再追加一个标志位
if (one_shot)
    event.events |= EPOLLONESHOT;

// 判断某位是否被置位：与 &
if (events[i].events & EPOLLIN) { ... }
```

- `|`：任一位为 1 则结果该位为 1 —— 用来"添加标志"；
- `&`：两位都为 1 才为 1 —— 用来"检测标志"；
- `~`：按位取反 —— 用来"清除标志"（如 `flag & ~O_NONBLOCK`）。

### 3. `errno` 与 `EAGAIN`

`errno` 是记录"上一个系统调用失败原因"的全局错误码（`<errno.h>`）。非阻塞 fd 读空/写满时，`recv`/`send` 返回 `-1` 且 `errno == EAGAIN`（`EWOULDBLOCK` 与之同值）。**EAGAIN 不是错误**，它表示"现在没数据可读/没空间可写，稍后再试"，ET 的读循环正是靠它判断"读空了"：

```cpp
if (bytes_read == -1)
{
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;      // 读空，正常退出循环
    return false;   // 其它错误，连接出问题了
}
```

### 4. `fcntl` 设置非阻塞

`fcntl(fd, cmd, ...)` 是对 fd 做"控制操作"的系统调用。设非阻塞要三步：先取旧标志，再或上 `O_NONBLOCK`，最后写回：

```cpp
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);       // 1. 读当前标志
    int new_option = old_option | O_NONBLOCK;  // 2. 追加"非阻塞"位
    fcntl(fd, F_SETFL, new_option);            // 3. 写回
    return old_option;
}
```

### 5. 类成员数组

数组可以作为类的成员，大小在编译期定死：

```cpp
class task
{
public:
    char m_buf[BUFFER_SIZE];   // 每个 task 对象自带的 8192 字节读缓冲
    // ...
};
```

另外本阶段的 `epoll_event events[MAX_EVENT_NUMBER];` 是 `main` 里的局部数组；而仓库把 `epoll_event events[MAX_EVENT_NUMBER];` 声明成 `WebServer` 的**成员**（`webserver.h` 第 70 行）——因为仓库把整个服务器封装成了一个类。两者语义一样：一段连续内存、下标从 0 开始、越界即未定义行为。

### 6. 静态成员（预告）

看一行 Stage 5 会出现的代码：

```cpp
class http_conn {
public:
    static int m_epollfd;    // 声明（注意：这里只是声明，定义要写在 .cpp 里）
};
```

`static` 成员**属于类本身，而不是某个对象**：无论创建多少个 `http_conn` 对象，`m_epollfd` 都只有一份，所有对象共享。这样"所有连接共用同一个 epoll 实例"就很自然。定义时要在类外写 `int http_conn::m_epollfd = -1;`。本阶段 `task` 里每个对象自己存一份 `m_epollfd` 也够用，但 Stage 5 会看到共享版本。

---

## 动手实现

> 约定：本节每一步的完整文件都给出；**只贴改动的片段会明确标注"节选"**。学习者文件用 `my_tiny_webserver/xxx` 表示。

### 0. 现状回顾：Stage 3 的线程池版 echo（节选）

你现在的 `echo_server.cpp` 核心长这样（节选，帮助回忆）：

```cpp
// 节选：Stage 3 的 task 与主循环
class task
{
public:
    task(int sockfd) : m_sockfd(sockfd) {}
    void process()               // 工作线程：阻塞 recv + send 回显
    {
        char buf[1024];
        int len = recv(m_sockfd, buf, sizeof(buf) - 1, 0);
        if (len > 0)
            send(m_sockfd, buf, len, 0);
    }
private:
    int m_sockfd;
};
```

它的痛点：主线程 `accept` 后必须立刻决定"这个连接丢给哪个线程"，而工作线程则各自阻塞在 `recv` 上。接下来我们一步步换成 epoll。

### 1. 第一步：epoll LT 版（accept/recv/send 改造成事件分发）

先写最朴素的 LT 版本，体会"主循环不再自己 recv，而是 epoll_wait 通知谁可读再去 recv"。

完整文件 `my_tiny_webserver/echo_server.cpp`：

```cpp
// echo_server.cpp —— Step 1：epoll LT 版
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>

#define MAX_EVENT_NUMBER 1024  // epoll_wait 一次最多取回的事件数
#define BUFFER_SIZE 1024       // 读缓冲区大小

// 把 fd 设为非阻塞（LT 下并非必须，先写出来为 ET 铺垫）
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 把 fd 加入 epoll 内核事件表（本步只用 LT，注册 EPOLLIN）
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN;    // 水平触发：fd 可读就反复返回该事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
}

// 把 fd 从事件表删除并关闭
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        printf("usage: %s port\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    // 1. 创建监听 socket（阻塞）
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    listen(listenfd, 5);

    // 2. 创建 epoll 事件表，注册 listenfd
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    addfd(epollfd, listenfd);

    // 3. 事件循环：epoll_wait 分发，谁就绪处理谁
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 新连接：accept 并注册进 epoll
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                    continue;
                addfd(epollfd, connfd);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方关闭或出错
                printf("close fd:%d\n", sockfd);
                removefd(epollfd, sockfd);
            }
            else if (events[i].events & EPOLLIN)
            {
                // 可读：读一次并回显（LT 下没读完会再次触发，读一次也安全）
                char buf[BUFFER_SIZE];
                memset(buf, '\0', BUFFER_SIZE);
                int ret = recv(sockfd, buf, BUFFER_SIZE - 1, 0);
                if (ret <= 0)
                {
                    removefd(epollfd, sockfd);
                    continue;
                }
                printf("get %d bytes from fd:%d\n", ret, sockfd);
                send(sockfd, buf, ret, 0);
            }
        }
    }

    close(epollfd);
    close(listenfd);
    return 0;
}
```

关键行讲解：

- `epoll_event events[MAX_EVENT_NUMBER];` —— 就绪事件数组，`epoll_wait` 把就绪事件写进来。
- `event.data.fd = fd;` —— `data` 是用户自定义数据，这里存 `fd`，取回时才能知道是哪个连接就绪。
- `epoll_wait(..., -1)` —— 永久阻塞等待；返回 `number` 个就绪事件。
- `if (number < 0 && errno != EINTR)` —— `epoll_wait` 可能被信号打断返回 `EINTR`，此时不应判错退出。
- 主循环里的 `if (sockfd == listenfd)` 是"新连接"分支，否则是"已连接 fd"分支——这正是仓库 `webserver.cpp::eventLoop` 的分发骨架。

先编译跑通它（命令见"编译与运行"），`nc` 回显应正常。**LT 版读一次就行**：假设客户端发了 5000 字节，你只 `recv` 了 1024，因为缓冲里还剩数据，`epoll_wait` 会**再次**返回 EPOLLIN，下一轮继续读——这就是 LT 的容错。

### 2. 第二步：LT → ET 升级（循环读到 EAGAIN）

把 `connfd` 的触发方式换成 ET。改动两处：

**改动 1：`addfd` 注册 `EPOLLET` 并设非阻塞**（节选）：

```cpp
// 节选：addfd 增加 ET 与非阻塞
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;   // 边沿触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);                 // ET 必须配合非阻塞
}
```

**改动 2：读事件分支改成循环读到 EAGAIN**（节选）：

```cpp
// 节选：ET 下必须循环读，直到 EAGAIN
else if (events[i].events & EPOLLIN)
{
    char buf[BUFFER_SIZE];
    int len = 0;
    while (true)
    {
        if (len >= BUFFER_SIZE - 1)
            break;   // 缓冲满了：echo 场景先停下，避免长度下溢（负数变巨大 size_t）
        int ret = recv(sockfd, buf + len, BUFFER_SIZE - 1 - len, 0);
        if (ret == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;              // 读空，退出循环
            removefd(epollfd, sockfd);
            break;
        }
        else if (ret == 0)
        {
            removefd(epollfd, sockfd);  // 对方关闭
            break;
        }
        len += ret;
    }
    printf("get %d bytes from fd:%d\n", len, sockfd);
    send(sockfd, buf, len, 0);
}
```

**演示 ET 下"只读一次"会丢数据**：把上面的 `while` 循环改回"只 `recv` 一次"（也就是保留 LT 那种读法），然后用超过 1024 字节的输入测试。原理如下：

```text
客户端一次 send 5000 字节（内核读缓冲一下有了 5000 字节）
        ┌────────────────┐
内核缓冲 │■■■■■■■■ 5000 字节│
        └────────────────┘
ET 只在"空→非空"通知一次 → 服务端 recv 一次最多取走 1023 字节（BUFFER_SIZE - 1）
        ┌────────────────┐
内核缓冲 │■■■■■■ 还剩 3976 字节│   ← 服务端不再收到任何通知！
        └────────────────┘
结果：只回显了不到 1024 字节，其余"卡"在缓冲区里，表现就是丢数据
```

"两次 send 分片"是同一现象的另一种触发方式——客户端 `send("第一部分")` 后紧跟着 `send("第二部分")`，两片数据几乎同时到达，内核缓冲里凑齐了超过一次 recv 容量的数据，ET 却只通知一次：

```cpp
// 节选：客户端演示"分片发送"（两次 send 凑成一条消息）
send(sockfd, part1, strlen(part1), 0);   // 第一片
send(sockfd, part2, strlen(part2), 0);   // 第二片，紧接着发
```

顺带点一句**粘包**：TCP 是字节流，没有"消息边界"。客户端发"AB"和"CD"两次，服务端可能一次 `recv` 就收到"ABCD"，也可能分四次收到。ET 的"循环读到 EAGAIN"只是把**当前缓冲里**的数据读完，并不能替你切分消息——如何按协议切分，正是 Stage 5 HTTP 状态机的任务。

### 3. 第三步：引入 EPOLLONESHOT

**为什么需要**：第二步里，同一个 fd 的读事件可能被主线程处理；如果之后我们把 fd 交给线程池，可能出现"主线程和一个工作线程同时操作同一个 fd"的竞态。`EPOLLONESHOT` 规定：**这个 fd 触发一次事件后自动下枪，直到你用 `modfd` 显式重新注册**。这样任何时刻最多只有一个线程在碰这个 fd。

改动：`addfd` 增加 `one_shot` 参数，并新增 `modfd`（与仓库 `http_conn.cpp` 第 59~95 行的 `addfd`/`modfd` 同构）：

```cpp
// 节选：ONESHOT 版 addfd / modfd（参数带 TRIGMode，与仓库同构）
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if (1 == TRIGMode)
        event.events |= EPOLLET;
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    if (1 == TRIGMode)
        event.events |= EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
```

要点：**`modfd` 就是"重新上枪"**。谁处理完了这个 fd，谁就调用 `modfd(epollfd, fd, EPOLLIN, TRIGMODE)` 让它恢复可监听。下一步你会在 `process()` 末尾看到它。

### 4. 第四步：接入线程池（Proactor 雏形）

现在把上一阶段的线程池接进来，得到 **Proactor 雏形**：

- **主线程**：`epoll_wait` 发现 `EPOLLIN` → 亲自 `read_once()` 把数据读进 `task` 的缓冲区 → `append` 到线程池；
- **工作线程**：从队列取出 `task` → `process()` 里只做 `send` 回显 + `modfd` 重新上枪；
- **主线程读完之后不再碰该 fd**：因为有 `EPOLLONESHOT` 兜底，fd 已下枪，工作线程处理完之前不会再有该 fd 的事件到达。

这一步同时用上 `lock/` 与 `threadpool/`，`threadpool/threadpool.h` 沿用 Stage 3 的简化版（无需修改，完整贴出便于对照）：

```cpp
// my_tiny_webserver/threadpool/threadpool.h —— Stage 3 简化版，本阶段沿用
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"

template <typename T>
class threadpool
{
public:
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

private:
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;
    int m_max_requests;
    pthread_t *m_threads;
    std::list<T *> m_workqueue;
    locker m_queuelocker;
    sem m_queuestat;
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        request->process();   // 取到任务直接处理（本阶段无 actor_model / 连接池）
    }
}

#endif
```

> 简化点：仓库的线程池构造函数是 `threadpool(int actor_model, connection_pool*, int thread_number, int max_requests)`，`run()` 里按 `actor_model` 分支且接入了数据库连接池。本阶段没有数据库与模型切换，故简化为上面这个只调用 `process()` 的版本。Stage 8 / 9 会恢复这些参数。

最终完整文件 `my_tiny_webserver/echo_server.cpp`：

```cpp
// echo_server.cpp —— Stage 4 最终版：epoll(LT/ET 可切换) + EPOLLONESHOT + 线程池(Proactor 雏形)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "threadpool/threadpool.h"

#define MAX_FD 65536           // 最大文件描述符数（与仓库 webserver.h 一致）
#define MAX_EVENT_NUMBER 1024  // epoll_wait 一次最多取回的事件数
#define BUFFER_SIZE 8192       // 读缓冲区大小

// 触发模式：改成 TRIG_LT 即切回水平触发（验收时两种都要测）
enum { TRIG_LT = 0, TRIG_ET = 1 };
const int TRIGMODE = TRIG_ET;

// 把 fd 设为非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 把 fd 加入 epoll 事件表
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP;
    if (1 == TRIGMode)
        event.events |= EPOLLET;
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从事件表删除并关闭 fd
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改 fd 监听的事件（EPOLLONESHOT 处理完后必须重新"上枪"）
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    if (1 == TRIGMode)
        event.events |= EPOLLET;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 任务类：一个连接对应一个对象（proactor 中主线程把数据读进它的缓冲区）
class task
{
public:
    task() : m_sockfd(-1), m_epollfd(-1), m_len(0) {}

    void init(int sockfd, int epollfd)
    {
        m_sockfd = sockfd;
        m_epollfd = epollfd;
        m_len = 0;
    }

    // 主线程调用：把 fd 上的数据读进 m_buf（ET 必须循环读到 EAGAIN）
    bool read_once()
    {
        m_len = 0;
        int bytes_read = 0;

        if (TRIGMODE == TRIG_LT)
        {
            // LT：读一次即可，没读完 epoll 会再次提醒
            bytes_read = recv(m_sockfd, m_buf, BUFFER_SIZE - 1, 0);
            if (bytes_read <= 0)
                return false;
            m_len = bytes_read;
            return true;
        }
        else
        {
            // ET：必须循环读，直到 EAGAIN / EWOULDBLOCK
            while (true)
            {
                if (m_len >= BUFFER_SIZE - 1)
                    break;    // 缓冲区满，防止越界（对 echo 而言即"消息过长"）
                bytes_read = recv(m_sockfd, m_buf + m_len, BUFFER_SIZE - 1 - m_len, 0);
                if (bytes_read == -1)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                        break;
                    return false;
                }
                else if (bytes_read == 0)
                {
                    return false;   // 对方关闭连接
                }
                m_len += bytes_read;
            }
            return true;
        }
    }

    // 工作线程调用：把主线程读好的数据回显，然后重新注册读事件
    void process()
    {
        send(m_sockfd, m_buf, m_len, 0);
        modfd(m_epollfd, m_sockfd, EPOLLIN, TRIGMODE);
    }

    char m_buf[BUFFER_SIZE];  // 读缓冲区（类成员数组）
    int m_len;                // 本次读到的字节数
    int m_sockfd;
    int m_epollfd;
};

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        printf("usage: %s port\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);

    // 1. 监听 socket（listenfd 用 LT，保持阻塞即可）
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    listen(listenfd, 5);

    // 2. epoll 事件表：listenfd 用 LT 注册
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    addfd(epollfd, listenfd, false, TRIG_LT);

    // 3. 线程池 + 连接对象数组（每个 fd 对应一个 task，与仓库 users 数组同构）
    threadpool<task> *pool = new threadpool<task>(8, 10000);
    task *users = new task[MAX_FD];

    // 4. 事件循环
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 新连接：accept 后注册读事件（开 ONESHOT）
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                    continue;
                users[connfd].init(connfd, epollfd);
                addfd(epollfd, connfd, true, TRIGMODE);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                printf("close fd:%d\n", sockfd);
                removefd(epollfd, sockfd);
            }
            else if (events[i].events & EPOLLIN)
            {
                // proactor：主线程负责读，读完交给线程池
                if (users[sockfd].read_once())
                {
                    pool->append(&users[sockfd]);
                }
                else
                {
                    removefd(epollfd, sockfd);
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}
```

关键点讲解：

- `task *users = new task[MAX_FD];` —— 预分配一个"连接对象数组"，用 `connfd` 当下标直接索引。这正是仓库 `webserver.cpp` 里 `users = new http_conn[MAX_FD]` 的做法（`webserver.cpp` 第 6 行）。代价是一次性占用约 `65536 × 8192 ≈ 512 MB` 内存——echo 阶段够用，但 Stage 5 会把它缩到 2048 字节读缓冲；真实工程会用更精细的内存管理。
- `addfd(epollfd, connfd, true, TRIGMODE)` —— 新连接**开 ONESHOT**，保证同一 fd 同一时刻最多一个线程碰。
- `read_once()` 返回 `false` 表示连接该关了（对方关闭或出错），主线程直接 `removefd`。
- `process()` 里 `send` 完立刻 `modfd(..., EPOLLIN, ...)` 重新上枪，否则该连接从此收不到任何事件。
- **为什么主线程读完后安全**：`read_once()` 已经把数据读进 `task::m_buf`，随后 `append` 通过队列互斥锁建立了"主线程写 → 工作线程读"的顺序；同时 `EPOLLONESHOT` 让该 fd 在 `modfd` 之前不会再触发，所以主线程不会再碰它。

> 简化点 1：`process()` 里 `send` 只调一次，大消息可能只发出去一部分（`send` 返回实际发送字节数）。真实做法是循环 `send` 直到发完——Stage 5 的 `http_conn::write()` 会给出完整的分批发送写法，本阶段回显测试数据量小，先不管。
> 简化点 2：`listenfd` 固定用 LT、只 `accept` 一次。仓库的 `dealclientdata()` 在 ET 模式下会用 `while` 循环 `accept` 直到 `EAGAIN`（`webserver.cpp` 第 222~240 行），以一次性吃光积压的连接，本阶段先不展开。

---

## 编译与运行

`my_tiny_webserver/makefile`（注意：第二行起是 **Tab 缩进**，不要用空格）：

```makefile
CXX ?= g++
CXXFLAGS += -g -Wall

echo_server: echo_server.cpp
	$(CXX) -o echo_server echo_server.cpp $(CXXFLAGS) -lpthread

clean:
	rm -f echo_server
```

编译运行：

```bash
cd ~/projects/my_tiny_webserver
make
./echo_server 9006
```

另开终端测试回显：

```bash
(printf 'hello epoll\n'; sleep 1) | nc -q 2 127.0.0.1 9006
# 预期输出：hello epoll
```

> **为什么测试要"发完先别断"**：本服务器的 `read_once()` 与仓库语义一致——`recv` 返回 `0`（对端关闭）就当作"连接结束"并丢弃刚读到的数据；且 `eventLoop` 里 `EPOLLRDHUP` 分支优先于 `EPOLLIN`，客户端"发完立刻断开"时数据与 FIN 一起到达，服务器会直接走进关闭分支，回显来不及发出。所以一次性测试要让连接**保持 1 秒**再断开：`(printf ...; sleep 1) | nc -q 2` 中，`sleep 1` 让管道晚 1 秒才 EOF（`nc` 也就在 1 秒后才发 FIN），服务器先读到 `EAGAIN` 完成回显；`-q 2` 表示 stdin EOF 后再等 2 秒才退出。交互式 `nc`（手动敲字符）不受此影响，因为连接一直开着。

---

## 验收清单

> 每条都必须实际跑过并看到预期结果。切换 LT/ET：改 `echo_server.cpp` 里 `const int TRIGMODE = TRIG_ET;` 那一行为 `TRIG_LT`，重新 `make`。

- [ ] **LT 版回显**：`TRIGMODE = TRIG_LT`，`make && ./echo_server 9006`，另开终端 `(printf 'hello lt\n'; sleep 1) | nc -q 2 127.0.0.1 9006`，输出 `hello lt`。
- [ ] **ET 版回显**：`TRIGMODE = TRIG_ET`，重新 `make && ./echo_server 9006`，同样命令输出 `hello lt`（内容自定，能回显即对）。
- [ ] **大输入不丢数据（ET）**：
  ```bash
  head -c 5000 /dev/urandom > /tmp/echo_in.bin
  (cat /tmp/echo_in.bin; sleep 1) | nc -q 2 127.0.0.1 9006 > /tmp/echo_out.bin
  cmp /tmp/echo_in.bin /tmp/echo_out.bin && echo "OK: 5000 字节回显无损"
  ```
  预期输出 `OK: 5000 字节回显无损`（`cmp` 两文件逐字节一致）。
- [ ] **复现"只读一次丢数据"的 bug**：把 `read_once()` 的 ET 分支临时改成只 `recv` 一次（去掉 `while`），`make && ./echo_server 9006`，然后用 20000 字节测试：
  ```bash
  head -c 20000 /dev/urandom > /tmp/echo_big.bin
  (cat /tmp/echo_big.bin; sleep 1) | nc -q 2 127.0.0.1 9006 > /tmp/echo_out.bin
  cmp /tmp/echo_big.bin /tmp/echo_out.bin || echo "OK: 复现了丢数据"
  ```
  预期：`cmp` 报差异（`echo_out.bin` 最多只有一次 `recv` 的量，即 ≤ 8191 字节），打印 `OK: 复现了丢数据`。看完记得把 `while` 循环改回来。
- [ ] **gdb 观察事件循环**：
  ```bash
  gdb ./echo_server
  (gdb) break echo_server.cpp:182   # 用 list 找到 epoll_wait 之后那行 for 循环，替换成实际行号
  (gdb) run 9006
  ```
  另开终端 `(printf 'x\n'; sleep 1) | nc -q 2 127.0.0.1 9006`，gdb 停住后 `print number` 应等于 1，`print events[0].data.fd` 应等于 `listenfd` 的值。
- [ ] **多客户端并发**：
  ```bash
  for i in 1 2 3 4 5; do ((printf "client $i\n"; sleep 1) | nc -q 2 127.0.0.1 9006) & done; wait
  ```
  预期：5 行 `client 1`~`client 5` 全部回显，顺序可乱但不缺。
- [ ] **服务端无崩溃退出**：上述所有测试后，服务端进程仍存活、能继续服务，`Ctrl+C` 才退出。

---

## 参考答案对照

对照仓库时，用 `grep -n` / 编辑器按行号定位：

| 本阶段代码 | 仓库对应 | 说明 |
|---|---|---|
| `addfd` / `modfd` / `removefd` / `setnonblocking` | `http/http_conn.cpp` 第 51~95 行 | 本阶段与仓库**同构**：参数、`EPOLLET`/`EPOLLONESHOT`/`EPOLLRDHUP` 的组合、`fcntl` 设非阻塞均一致 |
| `eventLoop` 主循环分发 | `webserver.cpp` 第 377~434 行 | 分支顺序（listenfd → RDHUP/HUP/ERR → 读 → 写）一致；本阶段还没信号、定时器、写事件分支 |
| `trig_mode`（LT/ET 组合） | `webserver.cpp` 第 47~73 行 | 仓库把 listenfd/connfd 的 LT/ET 拆成 4 种组合（`m_TRIGMode` 0~3）；本阶段用单一 `TRIGMODE` 常量简化，listenfd 固定 LT |
| `dealwithread` 的 proactor 分支 | `webserver.cpp` 第 310~328 行 | `read_once()` 成功则 `append_p`，失败则关闭——本阶段 `read_once()+append` 即此分支的雏形 |
| `events[MAX_EVENT_NUMBER]` | `webserver.h` 第 70 行（成员） | 仓库是 `WebServer` 类成员，本阶段是 `main` 局部变量 |
| `MAX_FD` / `MAX_EVENT_NUMBER` | `webserver.h` 第 18~19 行 | 值一致（65536 / 10000；本阶段 events 用 1024 也够） |
| `users = new http_conn[MAX_FD]` | `webserver.cpp` 第 6 行 | 本阶段 `new task[MAX_FD]` 与之同构，类型从 `http_conn` 换成 `task` |

**主要差异**：本阶段 `echo` 无定时器、无信号管道、无写事件分支、无 `Utils` 类（仓库把这些放在 `timer/lst_timer.h` 的 `Utils` 里，Stage 6 引入），因此主循环比仓库 `eventLoop` 精简。

---

## 常见问题

1. **ET 模式下小消息能回显、大消息只回显一部分**。典型症状：5000 字节只回来 1024。原因是 ET 只通知一次，你却只 `recv` 一次。解决：读事件里 `while` 循环 `recv` 直到 `errno == EAGAIN`。
2. **ET 版程序卡死、不返回也不回显**。ET 却忘了把 fd 设成非阻塞：循环里最后一次 `recv` 在阻塞 fd 上永久等待，走不到 EAGAIN。解决：`addfd` 里必须 `setnonblocking(fd)`。
3. **加了 ONESHOT 后，连接只回显一次就不再响应**。ONESHOT 触发一次就下枪，你处理完忘了 `modfd` 重新注册。解决：在 `process()` 末尾 `modfd(m_epollfd, m_sockfd, EPOLLIN, TRIGMODE)`。
4. **`epoll_wait` 返回 -1**。多半是被信号打断（`errno == EINTR`），不要当错误退出，`continue` 即可；其它 errno 才排查。
5. **客户端断开后服务端反复打 `close fd:N` 或 CPU 飙高**。对方关闭时 `EPOLLIN` 会触发，`recv` 返回 0，要在循环里 `removefd` 并 `break`，否则死循环反复 `recv(0)`。
6. **`send` 没有发完整条消息**。`send` 的返回值是"实际发送字节数"，可能小于你要求发的长度（尤其非阻塞 fd）。echo 小数据可先忽略，Stage 5 的 `write()` 用 `bytes_to_send`/`bytes_have_send` 完整解决。
7. **make 报 `missing separator`**。makefile 命令行必须以 **Tab** 开头；从网页复制粘贴容易变成空格，用 `cat -A makefile` 看行首应是 `^I`。
8. **一次性测试 `printf ... | nc -q 1` 时回显为空（或只有一半）**。原因有二：一是 `read_once()` 里 `recv` 返回 0（对端 FIN）会当作"连接结束"，把刚读到的数据一起丢弃（这是与仓库一致的语义）；二是 `eventLoop` 里 `EPOLLRDHUP` 分支排在 `EPOLLIN` 前面，客户端"发完立刻断开"时数据与 FIN 一起到达，服务器直接走关闭分支。交互式 `nc`（手动敲字符）不受影响；脚本化一次性测试请用 `(printf ...; sleep 1) | nc -q 2` 让连接多存活 1 秒（见"编译与运行"一节的说明）。这也侧面说明：**读半包/粘包的处理和"EOF 即关闭"的语义，必须和协议设计配合**——Stage 5 的 HTTP 服务器里浏览器不会发完请求立刻断，所以没有问题。

---

## 思考题

1. 为什么 ET 模式必须把 fd 设为非阻塞？如果保留阻塞，程序会停在哪一行、发生什么？
2. 用你自己的话解释：`EPOLLONESHOT` 和线程池之间是什么关系？如果没有 ONESHOT，本阶段哪个地方可能出现两个线程同时操作同一个 fd 的竞态？
3. 本阶段是 Proactor 雏形（主线程读好数据再交给线程池）。如果改成 Reactor（主线程只把 fd 交给线程池、由工作线程 recv），代码要怎么改？两种方式各有什么优缺点？
4. LT 模式下，读事件里只 `recv` 一次为什么不会丢数据？这带来什么额外开销？
5. `read_once()` 里 ET 循环读到 `EAGAIN` 就停，能否保证客户端发来的一条完整"消息"恰好被一次读完？如果不能，粘包/半包问题该由谁解决？
6. 仓库 `dealclientdata()` 在 ET 模式下为什么要用 `while(1)` 循环 `accept`，而 LT 模式只 `accept` 一次？

---

## 下一步

你已经掌握 epoll 事件驱动，并能把线程池接进 Proactor 雏形。下一步把这些零件组装成一台真正的服务器：在 [Stage 5：HTTP 服务器](stage-05-http.md) 里，我们会把 echo 换成"解析 HTTP 请求、返回网页和图片"，`task` 会升级为仓库里的 `http_conn` 类，并学会主从状态机、`mmap`、`writev`。
