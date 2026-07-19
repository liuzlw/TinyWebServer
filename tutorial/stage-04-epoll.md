# Stage 4：epoll 与 Reactor 事件驱动

> 🎯 **本阶段目标**：用 epoll 重写服务器的事件分发层 —— 单线程就能同时监视上万个连接，
> 「哪个连接有数据才处理哪个」。这是 TinyWebServer 高并发的核心，也是整个项目最硬核的一阶段。

## 📚 理论铺垫

### 4.1 Stage 3 留下的两个问题

1. **工作线程被慢客户端占住**：工作线程调用 `read` 时，如果对方数据还没来，线程就阻塞了。
2. **主线程串行 accept**：所有事件都挤在一个循环里，没有「按需处理」。

根源：**阻塞 I/O**。线程必须在 fd 上「傻等」数据。

### 4.2 I/O 多路复用：一个人盯一万个门

epoll 的思路：把一堆 fd 交给内核监视，**内核告诉你哪些 fd 有事件发生**，
应用程序只处理「有事」的 fd，没事的完全不用管。

三个系统调用搞定一切：

```cpp
int epfd = epoll_create(5);                        // 创建 epoll 实例（也是个 fd）
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);        // 把 fd 加入监视（可改可删）
int n = epoll_wait(epfd, events, MAX, timeout);    // 阻塞等待，返回"有事"的事件数
```

工作流程：

```
listenfd 注册进 epoll
   │
   ▼
epoll_wait 返回 ──→ listenfd 可读？→ accept 新连接，connfd 注册进 epoll
   │            ──→ connfd 可读？ → read 数据，处理，注册"可写"事件
   │            ──→ connfd 可写？ → write 响应，关闭或重新注册"可读"
   ▼
回到 epoll_wait
```

### 4.3 LT 与 ET：epoll 的两种触发模式

| | LT（Level Trigger，水平触发，默认） | ET（Edge Trigger，边沿触发） |
|---|---|---|
| 通知时机 | 只要缓冲区**还有数据**就反复通知 | 只在「无数据→有数据」的**边沿**通知一次 |
| 编程要求 | read 一次没读完没关系，下次还会通知 | **必须循环 read 直到 EAGAIN**，否则丢数据 |
| 效率 | 通知次数多，稍慢 | 通知次数少，高效 |
| 类比 | 水表只要没读就天天催你 | 只在新账单到达时敲一次门 |

**ET 必须搭配非阻塞 fd**：因为循环 read 直到没数据为止，如果 fd 是阻塞的，
最后一次 read 会永远卡住。设置非阻塞：

```cpp
int flags = fcntl(fd, F_GETFL);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

非阻塞 fd 在没数据时 read 返回 -1 且 `errno == EAGAIN` —— 这不是错误，是「读完了」的信号。

### 4.4 Reactor 与 Proactor

- **Reactor（反应器）**：epoll 只通知「fd 可读了」，**应用程序自己 read/write**。
  本项目 Reactor 模式：主线程检测到事件 → 把任务扔给线程池 → 工作线程自己 read 数据并处理。
- **Proactor（前摄器）**：**主线程先把数据读好**，再把「已完成的读事件 + 数据」交给线程池处理。
  本项目模拟 Proactor：主线程 read 完，工作线程只做业务处理。

Stage 3 我们写的其实是丐版 Reactor。本阶段主线上 Reactor，Stage 9 补 Proactor 开关。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| `fcntl` 与位运算 `\|` | 设置非阻塞 |
| `errno` 与 `EAGAIN` | 非阻塞 read 的"读完"判断 |
| `struct epoll_event`、联合体 `epoll_data` | epoll 事件 |
| 数组模拟连接表 `http_task users[MAX_FD]` | 按 fd 找到任务对象 |
| `while(true)` 事件循环 | Reactor 主循环 |

## 🔨 动手实现

把 `main.cpp` 重写为 epoll 版（lock/threadpool 保持不变）：

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <string>
#include "threadpool/threadpool.h"

const int PORT = 9006;
const int MAX_FD = 65536;
const int MAX_EVENT_NUMBER = 10000;
const int READ_BUF = 2048;
const char* DOC_ROOT = "./root";

// 设置非阻塞
int setnonblocking(int fd) {
    int old = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, old | O_NONBLOCK);
    return old;
}

// 把 fd 加入 epoll，ET 模式 + EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;  // RDHUP: 对方关闭连接
    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 重置 ONESHOT：处理完后重新武装这个 fd
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

class http_task {
public:
    void init(int fd, int epfd) { m_fd = fd; m_epollfd = epfd; }

    // Reactor 模式：工作线程自己 read（对比 Stage 9 的 Proactor）
    void process() {
        char buf[READ_BUF];
        std::string request;
        // ET 模式：必须循环读到 EAGAIN
        while (true) {
            int n = read(m_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 读完了
                removefd(m_epollfd, m_fd); return;
            }
            if (n == 0) { removefd(m_epollfd, m_fd); return; }       // 对方关闭
            request.append(buf, n);
        }

        // —— 下面是 Stage 2/3 的简易解析，Stage 5 换成状态机 ——
        size_t sp1 = request.find(' ');
        size_t sp2 = request.find(' ', sp1 + 1);
        std::string url = (sp1 != std::string::npos && sp2 != std::string::npos)
                          ? request.substr(sp1 + 1, sp2 - sp1 - 1) : "/";
        if (url == "/") url = "/index.html";

        std::string path = std::string(DOC_ROOT) + url;
        struct stat st;
        std::string body;
        int status = 200;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            FILE* fp = fopen(path.c_str(), "rb");
            body.resize(st.st_size);
            fread(&body[0], 1, st.st_size, fp);
            fclose(fp);
        } else { status = 404; body = "<html><body><h1>404</h1></body></html>"; }

        std::string response;
        char header[512];
        snprintf(header, sizeof(header),
                 "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 status, status == 200 ? "OK" : "Not Found", body.size());
        response = std::string(header) + body;

        // 简单起见直接 write（大文件部分写的情况 Stage 5 处理）
        write(m_fd, response.data(), response.size());
        removefd(m_epollfd, m_fd);   // 短连接：写完就关
    }

private:
    int m_fd;
    int m_epollfd;
};

http_task users[MAX_FD];

int main() {
    threadpool<http_task>* pool = new threadpool<http_task>(8, 10000);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }

    // ---- epoll 三件套 ----
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    // listenfd 用 LT（不设 EPOLLET）、不用 ONESHOT
    epoll_event levent;
    levent.data.fd = listenfd;
    levent.events = EPOLLIN | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &levent);
    printf("epoll server on port %d\n", PORT);

    while (true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) { perror("epoll_wait"); break; }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd) {
                // 新连接
                sockaddr_in client;
                socklen_t len = sizeof(client);
                int connfd = accept(listenfd, (sockaddr*)&client, &len);
                if (connfd < 0) continue;
                if (connfd >= MAX_FD) { close(connfd); continue; }
                users[connfd].init(connfd, epollfd);
                addfd(epollfd, connfd, true);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 异常：对方断开或出错，直接关闭
                removefd(epollfd, sockfd);
            }
            else if (events[i].events & EPOLLIN) {
                // 可读：交给线程池处理（Reactor：工作线程自己 read）
                pool->append(&users[sockfd]);
            }
        }
    }
    close(epollfd);
    close(listenfd);
    return 0;
}
```

### EPOLLONESHOT：本项目的一个精妙细节

```
时刻1: connfd 有数据 → epoll 通知 → 扔给线程池的工作线程 A 处理
时刻2: A 还在处理时，客户端又发了数据 → epoll 又通知 → 又扔给线程池
       → 工作线程 B 也在处理同一个 connfd！两个线程同时读写一个 fd，乱了！
```

`EPOLLONESHOT` 解决的就是这个：**一个 fd 的事件被取出后，epoll 暂时不再报告它**，
直到你调用 `epoll_ctl(MOD)` 重新注册（即 `modfd`）。这保证了
**同一时刻最多只有一个线程在处理某个连接**。原始项目大量使用这个技巧。

> 📝 本阶段代码里 process 结束时直接 removefd 关连接，所以 modfd 还没用上；
> Stage 5 支持 keep-alive 后，`modfd` 就是标准流程了。

```bash
cd build && cmake .. && make && ./server
```

## ✅ 验证

**验证 1：基本功能**

浏览器访问 `http://127.0.0.1:9006/` 正常；curl 返回 200。

**验证 2：慢客户端不再拖垮服务器（关键对比实验）**

```bash
# 开一个"慢连接"：连上但不发数据
nc 127.0.0.1 9006
```

保持这个 nc 不断开，同时浏览器/curl 继续访问 —— **完全不受影响**！
Stage 3 的版本里，8 个这样的慢连接就能让所有工作线程瘫痪。
现在工作线程只在「fd 确实有数据」时才会被启动。

**验证 3：并发连接数**

```bash
# 装个并发工具（二选一）
sudo apt install -y apache2-utils     # 提供 ab
ab -n 1000 -c 100 http://127.0.0.1:9006/index.html
# 期望：1000 个请求全部完成，观察 Requests per second
```

**验证 4：观察 epoll 的行为（可选但强烈推荐）**

```bash
strace -f -e trace=epoll_wait,epoll_ctl,accept,read,write ./server 2>&1 | head -50
# 浏览器访问一次，对照系统调用序列理解事件流
```

## 🐛 常见问题

**Q1: ET 模式下偶尔收不到数据/响应卡住？**
十有八九是 `read` 没有循环到 `EAGAIN`。ET 只通知一次，没读完的数据再也不会触发新事件。

**Q2: `accept` 返回 -1，errno 是 EAGAIN？**
正常现象：多进程/多线程抢同一个 listenfd 时可能发生（惊群）。判 `connfd < 0` 时 continue 即可。

**Q3: 客户端断开时服务器崩溃？**
往已关闭的连接 write 会触发 SIGPIPE 信号杀死进程！
正式写法：`signal(SIGPIPE, SIG_IGN)` 忽略它（Stage 9 整合时会加上，
原始项目在 `Utils` 类里做了这件事）。

## 🤔 思考与练习

1. 把 connfd 的 `addfd` 改成 LT（去掉 `EPOLLET`），process 里不循环 read（只读一次），
   用大文件请求测试 —— LT 下为什么不容易出问题？效率上差在哪？
2. 打印日志观察：一次请求中 epoll_wait 返回了几次、每次是什么事件。画出事件时序图。
3. 思考题：listenfd 为什么用 LT 而不用 ET？（提示：ET 下 accept 要怎么写才对？
   答案：ET 下也要循环 accept 到 EAGAIN。原始项目两种都实现了，Stage 9 会合入。）
4. 阅读参考答案 `webserver.cpp` 的 `eventListen()` 和 `eventLoop()`，
   对照你刚写的 main，找出结构上的对应关系 —— 你已经写出了它的雏形。

---

➡️ 下一阶段：[Stage 5：HTTP 状态机解析](stage-05-http-parser.md)
