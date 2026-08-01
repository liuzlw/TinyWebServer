# Stage 4 epoll 事件循环

> 🧠 **本项目的灵魂**。epoll 是 Linux 高性能 I/O 的核心,也是原项目支撑上万并发连接的关键。

## 1. 本阶段目标

- [ ] 理解 epoll 三件套:`epoll_create` / `epoll_ctl` / `epoll_wait`
- [ ] 分清 **LT(水平触发)** 与 **ET(边缘触发)** 及各自读取方式
- [ ] 把 S3 的"每连接一个工作线程"改成"单线程 + 事件驱动"
- [ ] 理解 Reactor / Proactor 两种模型的概念

**最终效果:** 服务器是**单线程**,却能同时服务成百上千个连接——5KB 的大数据包一次性发来,也能完整收下并回显。

## 2. 前置知识

- S1:socket 全流程
- S3:阻塞模型与线程池
- 新增:`fcntl` 设置非阻塞、`errno` 的 `EAGAIN`

## 3. 问题:S3 的线程池哪里还不够?

S3 用 8 个线程处理连接,但有个隐患——**每个工作线程的 `read()` 还是阻塞的**:

```text
8 个线程,每个在 read(connfd) 上阻塞等待
如果 8 个客户端都连上来却都不发数据 → 8 个线程全卡死
第 9 个连接来了 → 没有线程可用,只能排队
```

线程数再多也撑不住"连接多、活跃少"的场景。**真正的解法是:让一个线程同时盯着成千上万个连接,谁有数据就处理谁**——这就是 epoll。

## 4. epoll 三件套

epoll 是内核维护的一张"事件表"。三个系统调用:

### epoll_create:创建事件表

```cpp
int epollfd = epoll_create(1024);   // 参数在较新内核里被忽略,传正数即可
```

返回一个"事件表"的文件描述符。

### epoll_ctl:往表里注册/修改/删除 fd

```cpp
epoll_event ev;
ev.data.fd = fd;               // 这个 fd 是"谁"
ev.events = EPOLLIN;           // 关心它"什么时候"就绪(可读)
epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);   // ADD / MOD / DEL

// 常用事件:
// EPOLLIN   可读
// EPOLLOUT  可写
// EPOLLRDHUP 对端关闭(检测断开很有用)
// EPOLLET   边缘触发(下面讲)
// EPOLLONESHOT 一次性事件(Stage 5 讲)
```

### epoll_wait:阻塞等待就绪

```cpp
epoll_event events[1024];
int n = epoll_wait(epollfd, events, 1024, -1);   // -1 表示一直等
// 返回 n = 有几个 fd 就绪了,就绪信息在 events 数组里
```

**一次 `epoll_wait` 返回后,遍历 `events[0..n)`,处理就绪的 fd。**

### 完整模型

```text
                主线程(单线程!)
   ┌───────────────────────────────────────────┐
   │  while (1) {                               │
   │    n = epoll_wait(...)          ← 睡觉,被事件叫醒
   │    for i in 0..n:                          │
   │      如果 fd == listenfd  → accept 新连接,注册进 epoll
   │      如果 fd 可读          → read 数据 → 处理 → 回显
   │  }                                         │
   └───────────────────────────────────────────┘
```

**连接再多,主线程只有一个。** 它不挨个等连接,而是"等事件":谁可读了就处理谁。这就是**事件驱动**。

## 5. LT 与 ET:两种触发模式

这是 epoll 最容易绕晕的地方,务必看仔细。

| | LT(水平触发,默认) | ET(边缘触发,`EPOLLET`) |
|---|---|---|
| 触发条件 | 只要缓冲区**还有数据**,就持续通知 | 数据从"无"变"有"的**那一刻**通知一次 |
| 读不完会怎样 | 下次 `epoll_wait` **还会**通知你 | 不再通知,**数据就丢在缓冲区里** |
| 读取方式 | 读一次即可(内核会再叫你) | **必须循环读到 `EAGAIN`**(把数据一次拿干净) |
| 性能 | 相对多些无效唤醒 | 更高,但**代码必须正确** |

**打个比方:** 门铃。
- LT:你家里一直有人(数据没读完),门铃就一直响
- ET:只在你**刚进门的那一下**响铃,之后不响——你得在那一次把该拿的都拿完

### ET 为什么必须循环读?

```cpp
// ET 模式下,一次 epoll_wait 通知后:
while (true) {
    r = read(fd, buf, sizeof(buf));
    if (r > 0)        { write(fd, buf, r); }     // 有数据就处理
    else if (r == 0)  { closed = true; break; }  // 对端关闭
    else {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;                    // ★ 数据读完了,退出循环
        closed = true; break;
    }
}
```

`EAGAIN` 是非阻塞 fd 的"读完了"信号:返回 -1 + `errno == EAGAIN` 表示**暂时没有更多数据**。ET 必须靠它判断何时读完——所以 **ET 必须配非阻塞 socket**。

> **LT 模式就简单多了**:`read` 一次就行,没读完内核下次还会通知。这也是为什么项目里 LT 和 ET 代码的 `read_once` 写法不同(Stage 5 你会看到两个分支)。

## 6. Reactor 与 Proactor(概念)

项目支持的两种"事件处理模型",区别在于**谁负责真正读写数据**:

| | Reactor | Proactor(项目是模拟的) |
|---|---|---|
| 主线程做什么 | 只通知"某 fd 就绪了" | 自己先把数据**读完**,再交给工作线程 |
| 工作线程做什么 | **自己 read/write** 并处理业务 | 只处理已读好的数据 |
| 流程 | epoll_wait → 告诉 worker → worker 读+处理 | epoll_wait → 主线程 read → 交给 worker 处理 |

```text
Reactor:     主线程:  "fd 5 可读了!"        工作线程: read(fd5) → 处理
模拟Proactor: 主线程:  read(fd5) 读好了      工作线程: 处理已读数据
```

原项目用参数 `-a` 切换:`-a 0` 是 Reactor,`-a 1` 是模拟 Proactor(主线程读完再交给线程池)。Stage 9 整合时会看到两种代码路径。

> 本阶段的服务器是**单线程**,主线程自己既监听又读写。Reactor/Proactor 的分工要在 Stage 9 加了线程池后才真正体现——现在先建立概念。

## 7. 完整代码

**替换 `my_tiny_webserver/main.cpp`**(ET 模式版):

```cpp
// main.cpp —— epoll 事件循环 echo 服务器(Stage 4, ET 模式)
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

const int PORT = 9006;
const int MAX_EVENTS = 1024;
const int MAX_CONN = 1024;

// 设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old_option | O_NONBLOCK);
    return old_option;
}

// 注册到 epoll,支持 LT / ET 两种模式
void addfd(int epollfd, int fd, bool et)
{
    epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLRDHUP;
    if (et)
        ev.events |= EPOLLET;        // 边缘触发
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev);
    setnonblocking(fd);
}

int main()
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }

    // epoll 实例
    int epollfd = epoll_create(MAX_CONN);
    addfd(epollfd, listenfd, true);          // 监听 socket 用 ET

    std::cout << "epoll 服务器已启动, 监听端口 " << PORT << std::endl;

    epoll_event events[MAX_EVENTS];
    while (true)
    {
        int n = epoll_wait(epollfd, events, MAX_EVENTS, -1);   // 阻塞等待事件
        if (n < 0) { perror("epoll_wait"); continue; }

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            // 1. 有新的连接
            if (fd == listenfd)
            {
                while (true)
                {
                    struct sockaddr_in client;
                    socklen_t len = sizeof(client);
                    int connfd = accept(listenfd, (struct sockaddr *)&client, &len);
                    if (connfd < 0) break;              // ET 模式要一次 accept 干净
                    std::cout << "新连接 " << connfd << std::endl;
                    addfd(epollfd, connfd, true);
                }
                continue;
            }

            // 2. 已有连接可读(或对端关闭)
            if (events[i].events & (EPOLLIN | EPOLLRDHUP))
            {
                char buf[1024];
                bool closed = false;
                while (true)
                {
                    ssize_t r = read(fd, buf, sizeof(buf));
                    if (r > 0)
                    {
                        write(fd, buf, r);              // 回显
                    }
                    else if (r == 0)
                    {
                        closed = true;                  // 对端关闭
                        break;
                    }
                    else
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;                      // 数据读完了
                        closed = true;                  // 出错
                        break;
                    }
                }
                if (closed)
                {
                    std::cout << "关闭连接 " << fd << std::endl;
                    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
                    close(fd);
                }
            }
        }
    }
    close(epollfd);
    close(listenfd);
    return 0;
}
```

**关键点讲解:**

| 代码 | 说明 |
|---|---|
| `epoll_create(MAX_CONN)` | 创建事件表,返回 `epollfd` |
| `addfd(epollfd, listenfd, true)` | 监听 socket 注册进 epoll,`true` = ET |
| `epoll_wait(epollfd, events, MAX_EVENTS, -1)` | 阻塞等就绪事件,`-1` 无限等待 |
| `if (fd == listenfd)` | 通过 fd 判断是"新连接"还是"已有连接" |
| `while(true) { accept }` | **ET 模式**:可能同时来了多个连接,一次全部 accept 完(否则漏掉) |
| `while(true) { read }` | **ET 模式**:一次把数据读干净(否则数据留在缓冲区且不再通知) |
| `errno == EAGAIN` | 非阻塞读"读完了"的标志,ET 靠它退出循环 |
| `EPOLLRDHUP` | 对端关闭半连接也能检测到 |

## 8. 编译与运行

`CMakeLists.txt` 不变(main.cpp 内容换了而已):

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

**预期输出:**

```text
epoll 服务器已启动, 监听端口 9006
```

**测试回显(另开终端):**

```bash
nc 127.0.0.1 9006        # 输入文字 → 回显
```

## 9. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行服务器 | 输出启动信息 | ☐ |
| 2 | 单连接 `nc` 回显 | 输入即回显 | ☐ |
| 3 | **同时开 3 个 nc**,都发文字 | 三个都回显,服务器日志显示多连接并存 | ☐ |
| 4 | **大包测试**:一次性发 5000 字节 | 完整回显 5000 字节(证明 ET 循环读到 EAGAIN 正确) | ☐ |
| 5 | **慢客户端测试**:一个 nc 连上后不发任何字,再开另一个 nc | 第二个正常回显(单线程也扛得住,因为不阻塞) | ☐ |
| 6 | 服务器日志 | 显示各连接的新建与关闭 | ☐ |

> **第 4 条最检验 ET 写没写对**:如果只 `read` 一次就退出循环,5000 字节里超过缓冲区 1024 的部分会**永远读不到**(ET 不再通知)——回显就会不全。这正是"ET 必须循环读"的实证。

## 10. 调试技巧

### gdb 观察 epoll_wait 返回

```bash
gdb ./build/server
```

```text
(gdb) break main.cpp:66      ← 断在 epoll_wait 那行
(gdb) run
(gdb) next                   ← 此时阻塞在 epoll_wait;另开终端 nc 连一下,再回车
(gdb) print n
$1 = 1                       ← 有 1 个事件就绪
(gdb) print events[0].data.fd
$2 = 5                       ← 就绪的 fd
```

### strace 看系统调用(进阶,可选)

```bash
strace -f -e epoll_wait,epoll_ctl ./build/server
```

能看到 epoll 相关的每个系统调用,直观感受"事件驱动"的内核交互。

## 11. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 大包数据回显不全 | ET 模式只读了一次就退出 | 必须 `while(true)` 循环读到 `EAGAIN` |
| 同时来多个连接只 accept 到 1 个 | ET 模式 accept 只做了一次 | `while(true)` 循环 accept 到 `-1` |
| `epoll_wait` 返回 `-1 EINTR` | 被信号打断 | 检查 `errno` 为 `EINTR` 时 continue(Stage 6 信号处理会遇到) |
| 忘设非阻塞,ET 卡死 | ET 必须配非阻塞 | `addfd` 里 `setnonblocking(fd)`(代码已有) |
| 读到了对端关闭但没处理 | 没监听 `EPOLLRDHUP` | 注册事件加 `EPOLLRDHUP` |

## 12. 与原项目对照

| 本阶段 | 原项目对应 |
|---|---|
| `epoll_create` + 事件循环 | `webserver.cpp` 的 `eventLoop()`(`epoll_wait` + 分发) |
| `addfd` / `setnonblocking` | 原项目 `http_conn.cpp` 里的同名函数(位置不同,逻辑一样) |
| listenfd 就绪 → accept | `webserver.cpp` 的 `dealclientdata()` |
| 连接就绪 → read/处理 | `webserver.cpp` 的 `dealwithread()` / `dealwithwrite()` |
| 单线程处理 | 原项目把"处理"交给线程池(Stage 9) |

> 原项目的事件循环就是本阶段的骨架,只是分发逻辑更复杂(拆成 `dealwithread`/`dealwithwrite`/`dealwithsignal` 等函数),并且把每个连接包成了 `http_conn` 对象——这正是下一个阶段的主题。

## 13. 下一步

进入 **[Stage 5 完整 HTTP 状态机](stage-05-http-state-machine.md)**——把 S4 的"echo"升级成"解析 HTTP、返回文件",用状态机解决"粘包/半包"问题,这是项目最值得反复看的部分。
