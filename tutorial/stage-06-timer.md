# Stage 6：定时器处理非活跃连接

> 🎯 **本阶段目标**：实现 `timer/` 模块 —— 给每个连接装一个「倒计时」，
> 15 秒（3 × TIMESLOT）没有任何活动就强制断开，防止恶意/僵死连接耗尽服务器资源。

## 📚 理论铺垫

### 6.1 为什么需要定时器？

Stage 5 之后服务器支持 keep-alive，新问题随之而来：
客户端连上后可以**永远不发请求**，占着一个 fd、一份 http_conn 资源。
一万个这样的僵尸连接就能挤爆服务器。

解决：给每个连接设超时时间，期间有任何读写活动就**顺延**，
到点了就由服务器主动关闭。

### 6.2 三种定时方案，本项目选哪种？

| 方案 | 数据结构 | 触发方式 | 复杂度 |
|------|----------|----------|--------|
| **升序链表**（本项目） | 双向链表按超时时间排序 | SIGALRM + alarm 周期tick | 插入 O(n)，tick O(过期的) |
| 时间轮 | 哈希槽 + 链表 | 滴答推进 | O(1) |
| 最小堆 | 堆 | 取堆顶比较 | O(log n) |

升序链表最简单直观：链表头永远是**最先过期**的定时器，
每次 tick 只需从头往后摘「已过期」的节点，遇到第一个没过期的就停。

### 6.3 信号统一事件源：信号处理的最优实践

Linux 信号（SIGALRM）和 I/O 事件是两套机制，混在一起很难写对。
经典技巧（本项目采用）：

```
1. socketpair 创建一对 Unix 域 socket（m_pipefd[0] 读端，m_pipefd[1] 写端）
2. 信号处理函数里不做任何逻辑，只往管道写一个字节：信号编号
3. 把 m_pipefd[0] 注册进 epoll
4. 信号来了 → 管道可读 → epoll_wait 在主循环里"顺便"处理信号
```

好处：信号处理**和其他 I/O 事件在同一个线程、同一个循环里串行处理**，
不用在信号处理函数里干危险的事（信号处理函数里只能调用 async-signal-safe 函数，
连 printf 都不能用！）。

### 6.4 定时器与连接的绑定关系

```
┌─────────────┐   ┌──────────────┐
│ client_data │←─→│  util_timer  │      定时器链表（按 expire 升序）
│ socket,addr │   │ expire, cb   │      head → [5s] → [8s] → [12s] → tail
└─────────────┘   └──────────────┘
```

- 每个连接（client_data）挂一个定时器（util_timer），**互相持有指针**
- 连接有活动时：`adjust_timer` 把定时器摘出来、expire += 3×TIMESLOT、重新插回正确位置
- alarm 触发 tick：从头摘掉所有已过期的，执行回调 `cb_func`（关闭连接）

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| 双向链表手动实现（指针操作） | `sort_timer_lst` —— 数据结构实战 |
| 函数指针 `void (*)(client_data*)` | 定时器回调 `cb_func` |
| `socketpair`、`sigaction`、`alarm` | 信号统一事件源 |
| `time(NULL)` | 获取当前时间戳 |
| 前向声明 `class util_timer;` | 解决 client_data 和 util_timer 互相引用 |

## 🔨 动手实现

在 `my_tiny_webserver/timer/` 下创建 `lst_timer.h` 和 `lst_timer.cpp`。
（原版逻辑都在头文件里，本教程按 .h/.cpp 分开写，与新版仓库一致。）

### 6.1 `timer/lst_timer.h`

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

class util_timer;          // 前向声明

// 用户数据：连接 + 定时器
struct client_data {
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

    time_t expire;                          // 到期时间（绝对时间戳）
    void (*cb_func)(client_data*);          // 到期回调：关连接
    client_data* user_data;
    util_timer* prev;
    util_timer* next;
};

// 升序定时器链表
class sort_timer_lst {
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer* timer);      // 按 expire 升序插入
    void adjust_timer(util_timer* timer);   // 顺延：expire 变了，重新归位
    void del_timer(util_timer* timer);      // 摘除
    void tick();                            // 处理所有已到期的

private:
    void add_timer(util_timer* timer, util_timer* lst_head);  // 在子链表中插入
    util_timer* head;
    util_timer* tail;
};

// 信号处理与定时器调度的工具类
class Utils {
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);
    int setnonblocking(int fd);
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    static void sig_handler(int sig);       // 信号处理：只写管道
    void addsig(int sig, void(handler)(int), bool restart = true);
    void timer_handler();                   // tick + 重新 alarm
    void show_error(int connfd, const char* info);

public:
    static int* u_pipefd;                   // 管道（静态，信号处理函数要用）
    sort_timer_lst m_timer_lst;
    int m_epollfd;
    int m_TIMESLOT;
};

void cb_func(client_data* user_data);       // 超时回调：关闭连接

#endif
```

### 6.2 `timer/lst_timer.cpp` 核心实现

**add_timer：保持升序插入**

```cpp
void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer) return;
    if (!head) { head = tail = timer; return; }
    // 比头还早 → 插到头部
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);    // 否则在 head 之后的子链表里找位置
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) {
    util_timer* prev = lst_head;
    util_timer* tmp = prev->next;
    while (tmp) {
        if (timer->expire < tmp->expire) {   // 找到位置：插在 prev 和 tmp 之间
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    if (!tmp) {              // 比所有都晚 → 插到尾部
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}
```

**adjust_timer：连接有活动，顺延 3 个 TIMESLOT**

```cpp
void sort_timer_lst::adjust_timer(util_timer* timer) {
    if (!timer) return;
    util_timer* tmp = timer->next;
    // 还在正确位置（或已是尾节点）就不用动
    if (!tmp || (timer->expire < tmp->expire)) return;
    if (timer == head) {          // 头节点：摘下来重新全表插入
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    } else {                      // 中间节点：摘下来在后续子链表插入
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
```

> 🔑 为什么只需要和 `next` 比较？因为 expire 只会**变大**（顺延），
> 节点只会向后移动，绝不会需要前移 —— 这个优化利用了业务特性。

**tick：处理到期的定时器**

```cpp
void sort_timer_lst::tick() {
    if (!head) return;
    time_t cur = time(NULL);
    util_timer* tmp = head;
    while (tmp) {
        if (cur < tmp->expire) break;   // 第一个没过期的 → 后面都不用过期
        tmp->cb_func(tmp->user_data);   // 执行超时回调（关连接）
        head = tmp->next;               // 从链表摘除
        if (head) head->prev = NULL;
        delete tmp;
        tmp = head;
    }
}
```

**信号统一事件源**

```cpp
int* Utils::u_pipefd = 0;

void Utils::init(int timeslot) { m_TIMESLOT = timeslot; }

void Utils::sig_handler(int sig) {
    // 信号处理函数：只把信号编号写进管道，绝不做其他事
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);     // 重新上闹钟：每 TIMESLOT 秒 tick 一次
}
```

### 6.3 接入主循环

main.cpp 需要改动的点（对照原始项目 `webserver.cpp`）：

```cpp
// 全局
client_data* users_timer = new client_data[MAX_FD];
Utils utils;
const int TIMESLOT = 5;

// eventListen 里：
utils.init(TIMESLOT);
ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
utils.setnonblocking(m_pipefd[1]);
utils.addfd(m_epollfd, m_pipefd[0], false, 0);   // 管道读端进 epoll
utils.addsig(SIGALRM, utils.sig_handler, false);
utils.addsig(SIGTERM, utils.sig_handler, false);
alarm(TIMESLOT);                                  // 上第一个闹钟
Utils::u_pipefd = m_pipefd;
```

accept 新连接时创建定时器：

```cpp
users_timer[connfd].address = client_address;
users_timer[connfd].sockfd = connfd;
util_timer* timer = new util_timer;
timer->user_data = &users_timer[connfd];
timer->cb_func = cb_func;
timer->expire = time(NULL) + 3 * TIMESLOT;    // 15 秒超时
users_timer[connfd].timer = timer;
utils.m_timer_lst.add_timer(timer);
```

连接有读写活动时顺延：

```cpp
util_timer* timer = users_timer[sockfd].timer;
if (timer) {
    timer->expire = time(NULL) + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
}
```

事件循环里处理信号：

```cpp
else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
    char signals[1024];
    int ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    for (int i = 0; i < ret; ++i) {
        switch (signals[i]) {
        case SIGALRM: timeout = true; break;      // 标记：稍后 tick
        case SIGTERM: stop_server = true; break;  // 优雅停机
        }
    }
}
// 循环末尾：
if (timeout) { utils.timer_handler(); timeout = false; }
```

cb_func 的实现（超时踢连接）：

```cpp
void cb_func(client_data* user_data) {
    epoll_ctl(utils.m_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    printf("kick idle connection fd=%d\n", user_data->sockfd);
}
```

CMakeLists.txt 加入 `timer/lst_timer.cpp`。

## ✅ 验证

**验证 1：空闲连接被踢（核心验收）**

```bash
# 终端 A：启动服务器（可把 TIMESLOT 临时改小到 2 秒加速测试）
./server

# 终端 B：连上后什么都不发，等 3*TIMESLOT 秒
nc 127.0.0.1 9006
# 期望：约 15 秒后连接被服务器主动断开（nc 退出/显示断开）
# 服务器终端打印：kick idle connection fd=xx
```

**验证 2：活跃连接不被踢**

```bash
# 连上后每隔几秒发一次请求（每 5 秒一个请求，超时 15 秒）
while true; do curl -s -o /dev/null http://127.0.0.1:9006/index.html; sleep 5; done
# 期望：一直正常运行，服务器不踢这个连接
```

**验证 3：keep-alive 连接超时被回收**

```bash
curl -v http://127.0.0.1:9006/index.html
# curl 退出后连接其实处于半开状态；15 秒后服务器日志显示踢掉
```

**验证 4：SIGTERM 优雅停机**

```bash
kill -15 $(pgrep -f './server')
# 期望：服务器正常退出而不是被直接杀死（stop_server 生效）
```

## 🐛 常见问题

**Q1: 定时器从不触发？**
检查链：`alarm(TIMESLOT)` 是否调用过、`timer_handler` 里是否重新 `alarm`、
SIGALRM 是否被 `addsig` 注册、管道读端是否加入了 epoll。四个环节缺一不可。

**Q2: 触发一次后就不再触发了？**
`timer_handler()` 末尾忘了 `alarm(m_TIMESLOT)`。alarm 是一次性的，必须每次续上。

**Q3: 踢连接后服务器崩溃（double free / 段错误）？**
定时器和连接的生命周期要同步：连接被正常关闭时，一定要 `del_timer` 并置空
`users_timer[fd].timer`，否则 tick 时会对已关闭的连接再关一次。

**Q4: 信号处理函数里加 printf 调试，程序行为诡异？**
printf 不是 async-signal-safe！信号处理函数里只能写管道，调试信息在主循环里打。

## 🤔 思考与练习

1. 用 gdb 在 `tick()` 打断点，观察链表里定时器的 expire 顺序。
2. 画出 add/adjust/del/tick 四个操作对链表的指针变化图（纸笔画 3 个节点的例子）。
3. 面试题：为什么不用 `sleep` 而是 `alarm`？为什么用 socketpair 而不是直接
   在信号处理函数里 tick？（答案就在 6.3，用自己的话复述一遍）
4. 拓展阅读：了解「时间轮」方案（Linux 内核定时器用的就是它），
   思考它比升序链表强在哪、适合什么场景。
5. 思考题：如果 TIMESLOT=5、超时=3×TIMESLOT，一个连接最长可能多久才被踢？
   最短呢？（提示：alarm 和连接创建的时刻是异步的，误差最大一个 TIMESLOT）

---

➡️ 下一阶段：[Stage 7：同步/异步日志系统](stage-07-log.md)
