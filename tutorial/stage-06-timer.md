# Stage 6 定时器:清理非活动连接

> keep-alive 连接挂着一大堆不关,连接会越积越多,最终拖垮服务器。定时器定期扫描,把"长期不活动"的连接关掉。

## 1. 本阶段目标

- [ ] 理解"升序链表定时器"的 add / adjust / del / tick
- [ ] 理解**信号 → 事件**的转换:为什么不用信号回调而用 `socketpair`
- [ ] 给 S5 的服务器加上"空闲连接超时关闭"
- [ ] 验证:空闲连接 15 秒被关,活跃连接不被误关

**最终效果:** 连接 15 秒不发任何数据 → 服务器主动关闭它;期间有数据的连接,超时时间自动顺延。

## 2. 前置知识

- S5:http_conn 状态机;S4:epoll 事件循环
- 新增:信号、`alarm`、`socketpair`、定时器链表

## 3. 问题:keep-alive 连接的隐患

S5 支持了 `Connection: keep-alive`——连接处理完一个请求不关闭,复用下一个。但浏览器可能发完请求就**再也不来了**,这条连接就**永远挂着**。

假设 10000 个客户端每人挂 1 条死连接,服务器就欠了 10000 个 fd。**必须定期把"长期不活动"的连接踢掉。**

## 4. 升序链表定时器:设计

### 定时器长什么样

每个连接配一个 `util_timer`,记录它的**绝对到期时刻** `expire`:

```cpp
struct util_timer {
    time_t expire;              // 到期时刻(time(NULL) + 超时秒数)
    void (*cb_func)(client_data *);  // 到期的回调(关闭连接)
    client_data *user_data;     // 回调参数(哪个连接)
    util_timer *prev, *next;    // 双向链表指针
};
```

### 数据结构:按到期时刻升序的双向链表

```text
head ──► [expire=100] ──► [expire=105] ──► [expire=120] ──► [expire=130] ──► NULL
```

**为什么升序?** 因为 `tick()` 每次只需看链表头:头没到期,后面的更晚,肯定也没到期,直接停止扫描。到期了的,从头往后一个个摘掉执行回调。

四个操作:

| 操作 | 干什么 | 什么时候调 |
|---|---|---|
| `add_timer` | 按 expire 升序插进链表 | 新连接 accept 时 |
| `adjust_timer` | 连接有活动,更新 expire 并重新排序 | 读/写事件发生时 |
| `del_timer` | 从链表移除并删除 | 连接关闭时 |
| `tick` | 从头扫描,执行所有到期连接的回调 | 定时器信号触发时 |

## 5. 信号 → 事件:为什么这套机制这么绕

定时器靠 `alarm(TIMESLOT)` 周期产生 `SIGALRM` 信号。**信号处理函数里直接调 `tick()` 行不行?**

不行。信号可能打断正在执行的操作(比如正在 `process()` 处理请求),在信号处理函数里做复杂操作**不安全**(不是可重入的)。

**原项目的解法:信号处理函数只做一件事——往一个 `socketpair` 里写一个字节。** 然后 epoll 发现这个 socket 可读,主循环在**安全的时机**读它,再调 `tick()`。

```text
alarm(5) 每 5 秒
   │
   ▼
SIGALRM 信号
   │
   ▼
信号处理函数 send(socketpair写端, 1字节)   ← 只干这一件小事,安全
   │
   ▼
epoll 发现 socketpair 读端可读
   │
   ▼
主循环:读信号字节 → 判断是 SIGALRM → tick() + alarm(5)   ← 安全环境里做正事
```

**为什么是 `socketpair` 而不是 `pipe`?**

⚠️ 这是我实测踩过的坑:**信号处理函数用的是 `send()`,`send()` 只对 socket 有效,对 pipe 会失败(ENOTSOCK)**。所以必须是 `socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd)`——它创建一对互相连接的 socket。如果你用 `pipe()`,信号字节写不进去,`tick()` 永远不会被调,定时器静默失效。原项目正是用 `socketpair`。

## 6. timer/lst_timer.h + lst_timer.cpp

在 `my_tiny_webserver/` 下新建 `timer/lst_timer.h`(**简化版**:去掉了原项目的 `#include "../log/log.h"`,本模块没用日志):

```cpp
#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
// S6 简化版:去掉了原项目的 #include "../log/log.h"(本模块没有用到日志)

class util_timer;

struct client_data
{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                       // 超时时刻(绝对时间)

    void (*cb_func)(client_data *);      // 超时回调函数
    client_data *user_data;              // 回调参数
    util_timer *prev;
    util_timer *next;
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);
    void adjust_timer(util_timer *timer);
    void del_timer(util_timer *timer);
    void tick();

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    int setnonblocking(int fd);

    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    static void sig_handler(int sig);

    void addsig(int sig, void(handler)(int), bool restart = true);

    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
```

在 `my_tiny_webserver/timer/` 下新建 `lst_timer.cpp`(与 `timer/lst_timer.cpp` 一致,仅 cb_func 里加了一行 printf 便于观察):

```cpp
#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

//按 expire 升序插入链表
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
        return;
    if (!head)
    {
        head = tail = timer;                 // 空链表
        return;
    }
    if (timer->expire < head->expire)        // 比头还早,插到最前
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);                  // 否则从 head 开始找位置
}

//活动后更新 expire,链表重新排
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
        return;
    util_timer *tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire))
        return;                              // 排在后一个之前,位置不用动
    if (timer == head)                       // 是头,摘下来重新插
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
        return;
    if ((timer == head) && (timer == tail))  // 链表只有一个结点
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

//到点检查:从头开始,把所有已过期的定时器摘掉并执行回调
void sort_timer_lst::tick()
{
    if (!head)
        return;

    time_t cur = time(NULL);
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)               // 还没到期,后面的更晚,直接停
            break;
        tmp->cb_func(tmp->user_data);        // 执行超时回调(关闭连接)
        head = tmp->next;
        if (head)
            head->prev = NULL;
        delete tmp;
        tmp = head;
    }
}

//把 timer 插到 lst_head 开头的链表里(保持升序)
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp)                                // 走到末尾,插到尾部
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//信号处理函数:把信号"写进 socketpair",让主循环像处理普通 IO 一样处理信号
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);          // 重新设定,让 SIGALRM 周期性地来
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

//超时回调:关闭连接
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    printf("定时器关闭空闲连接 %d\n", user_data->sockfd);   // ← 教程加的观察日志,原项目没有
}
```

## 7. main.cpp:把定时器接进服务器

**替换 `my_tiny_webserver/main.cpp`**(在 S5 基础上加定时器,约 90 行新增):

```cpp
// main.cpp —— epoll + http_conn + 定时器(Stage 6)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http/http_conn.h"
#include "timer/lst_timer.h"

const int PORT = 9006;
const int MAX_FD = 65535;
const int MAX_EVENT_NUMBER = 10000;
const int TRIGMode = 1;        // 0 = LT, 1 = ET
const int TIMESLOT = 5;        // 每 5 秒扫描一次定时器

http_conn *users = new http_conn[MAX_FD];
// 每个连接对应一份 client_data(sockfd + 它的定时器)
client_data *users_timer = new client_data[MAX_FD];
sort_timer_lst timer_lst;

// socketpair:信号处理函数往里写,主循环从中读
int pipefd[2];

extern void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev, int TRIGMode);

// 为连接创建一个定时器,加入升序链表
void init_timer(int connfd, const sockaddr_in &client_address)
{
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;                 // 超时回调:关闭连接
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;       // 3 个周期(15 秒)没活动就超时
    users_timer[connfd].timer = timer;
    timer_lst.add_timer(timer);
}

// 连接有活动,更新它的超时时间
void adjust_timer(int connfd)
{
    util_timer *timer = users_timer[connfd].timer;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    timer_lst.adjust_timer(timer);
    printf("连接 %d 有活动, 超时顺延\n", connfd);
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

    int epollfd = epoll_create(5);
    http_conn::m_epollfd = epollfd;
    addfd(epollfd, listenfd, false, TRIGMode);

    // --- 信号 → 套接字对的机制 ---
    // 用 socketpair 而不是 pipe:信号处理函数里用的是 send(),
    // send() 只对 socket 有效,所以必须是 socketpair(原项目如此)
    Utils utils;
    utils.init(TIMESLOT);
    Utils::u_epollfd = epollfd;
    Utils::u_pipefd = pipefd;
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    utils.setnonblocking(pipefd[0]);          // 读端非阻塞
    utils.setnonblocking(pipefd[1]);          // 写端非阻塞
    addfd(epollfd, pipefd[0], false, 0);      // 读端注册进 epoll
    // 只捕获 SIGALRM;restart=false 让 epoll_wait 能返回 EINTR(与原项目一致)。
    // 不碰 SIGINT/SIGTERM,让 Ctrl+C / kill 能正常终止服务器
    utils.addsig(SIGALRM, Utils::sig_handler, false);
    alarm(TIMESLOT);                          // 开始周期计时

    char root[] = "root";
    printf("服务器已启动, 监听端口 %d, 定时器超时 %d 秒\n", PORT, 3 * TIMESLOT);

    epoll_event events[MAX_EVENT_NUMBER];
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) { perror("epoll_wait"); break; }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // socketpair 可读:收到了信号
            if (sockfd == pipefd[0])
            {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
                for (int j = 0; j < ret; j++)
                {
                    if (signals[j] == SIGALRM)
                    {
                        timer_lst.tick();            // 清理超时连接
                        alarm(TIMESLOT);             // 重新计时
                    }
                }
                continue;
            }

            if (sockfd == listenfd)
            {
                while (true)
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0) break;
                    if (connfd >= MAX_FD) { close(connfd); continue; }
                    users[connfd].init(connfd, client_address, root, TRIGMode);
                    init_timer(connfd, client_address);     // 新建定时器
                    printf("accept, 当前连接数: %d\n", http_conn::m_user_count);
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                timer_lst.del_timer(users_timer[sockfd].timer);   // 删定时器
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                if (users[sockfd].read_once())
                {
                    adjust_timer(sockfd);              // 有活动,顺延超时
                    users[sockfd].process();
                }
                else
                {
                    timer_lst.del_timer(users_timer[sockfd].timer);
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                adjust_timer(sockfd);                  // 有活动,顺延超时
                if (!users[sockfd].write())
                {
                    timer_lst.del_timer(users_timer[sockfd].timer);
                    users[sockfd].close_conn();
                }
            }
        }
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete[] users_timer;
    return 0;
}
```

**定时器接入的三个时机:**

| 时机 | 调用 |
|---|---|
| accept 新连接 | `init_timer(connfd, addr)`——创建定时器,15 秒后到期 |
| 连接有读/写活动 | `adjust_timer(connfd)`——expire 顺延 15 秒 |
| 连接关闭 | `timer_lst.del_timer(...)`——移除定时器 |
| SIGALRM 到达 | `timer_lst.tick()`——关闭所有已到期连接 |

## 8. 编译与运行

更新 `CMakeLists.txt`(加入 `timer/lst_timer.cpp`):

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp http/http_conn.cpp timer/lst_timer.cpp)
target_link_libraries(server pthread)
```

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

## 9. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9006/welcome.html` | `200`(功能未破坏) | ☐ |
| 2 | **空闲连接被关**:`nc 127.0.0.1 9006` 连上后**不发任何数据**,等约 20 秒 | nc 提示连接关闭/EOF,服务器日志出现 `定时器关闭空闲连接` | ☐ |
| 3 | **活跃连接不误关**:写个脚本每 4 秒发一次 `GET ... keep-alive` 请求,持续 20 秒 | 每次都收到 200,连接不被关(日志无 `定时器关闭` 或间隔正确) | ☐ |
| 4 | 连接正常关闭(发完请求就断开) | 日志无 `定时器关闭`(是正常 `close`) | ☐ |
| 5 | `Ctrl+C` 停止服务器 | 服务器正常退出(没被信号搞死) | ☐ |

> 第 2 条等待约 20 秒即可(超时设的是 3×5=15 秒,扫描是每 5 秒一次,实际在 15~20 秒之间触发)。想加快调试,可以把 `TIMESLOT` 改成 1(超时 3 秒)。

## 10. 调试技巧

### gdb 观察 tick 清理

```bash
gdb ./build/server
```

```text
(gdb) break timer/lst_timer.cpp:96      ← 断在 tick()
(gdb) run
(gdb) print head->expire                ← 看链表头结点的到期时刻
$1 = 1750000000
(gdb) print time(NULL)                  ← 和当前时间比,判断会不会被清
$2 = 1749999995
```

### 查看当前有几个连接

```text
(gdb) print http_conn::m_user_count
$1 = 3
```

## 11. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| **定时器完全不工作**(空闲连接永远不被关) | 信号写进了 `pipe()` 而不是 `socketpair()`——`send()` 对 pipe 失效(ENOTSOCK) | **用 `socketpair(PF_UNIX, SOCK_STREAM, 0, ...)`,不是 `pipe()`**(本阶段教程用的就是 socketpair) |
| `epoll_wait` 频繁返回 `-1 EINTR` | 信号到达,epoll_wait 被中断 | 代码里 `errno != EINTR` 才 break,已经处理 |
| 连接刚连上就被关 | 定时器到期时间算错 | `expire = time(NULL) + 3*TIMESLOT`,别少乘 |
| 活跃连接被误关 | 忘记调 `adjust_timer` | 读/写事件分支里都要调 |
| `alarm` 只触发一次 | 忘记在 tick 后重新 `alarm(TIMESLOT)` | 每收到 SIGALRM 都要重新设 alarm |

## 12. 与原项目对照

| 本阶段 | 原项目 |
|---|---|
| `timer/lst_timer.h` + `lst_timer.cpp` | 与原项目**基本一致**(去掉 log 头文件;cb_func 加了一行 printf) |
| `init_timer` / `adjust_timer` | 对应 `webserver.cpp` 的 `timer(connfd, addr)` 与各分支里的 `adjust_timer` |
| main 里的定时器初始化 | 对应 `webserver.cpp` 的 `eventListen()` 信号部分(`socketpair`/`addsig`/`alarm`) |
| 事件循环里读信号 | 对应 `webserver.cpp` 的 `dealwithsignal()` |
| `Utils` 类 | 原项目同一个类,还多了 `initmysql_result` 等(Stage 8/9 见) |

> **diff 对照**:
> ```bash
> diff my_tiny_webserver/timer/lst_timer.cpp timer/lst_timer.cpp
> ```

## 13. 下一步

进入 **[Stage 7 日志系统](stage-07-log.md)**——把服务器运行状态写进文件,同步/异步两种模式,顺带用上 C5 学的生产者消费者。
