# Phase 5 —— 定时器

## 目标

实现一个**升序双向链表定时器**，配合信号机制定期扫描超时连接并清理。

**可见结果：** 每个连接在创建时绑定一个定时器（默认 3×TIMESLOT 秒后超时）。如果连接持续活跃，定时器不断重置；如果空闲超时，连接被关闭并从 epoll 中移除。

---

## 前置知识

- Phase 1 的 `locker.h`
- Phase 2 的日志系统
- Linux 信号基础：知道 `SIGALRM` 是闹钟信号

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **gdb** | `handle SIGALRM nostop noprint pass` - 信号调试 |

---

## 分步实现

### Step 1：为什么要定时器

客户端可能非正常断开（网线拔了、浏览器崩溃了），服务器不知道。如果没有定时器：
- 死连接永远占用 epoll 和内存
- 文件描述符越来越多，最终耗尽（每个进程默认上限 1024）

定时器定期扫描：如果连接在 N 秒内无任何活动 → 关闭它。

### Step 2：定时器数据结构

用**升序双向链表**而不是优先级队列或红黑树——在连接数小于 10000 时，链表的 add/adjust/del 足够快，且代码简单。

```cpp
// lst_timer.h
#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <time.h>
#include <netinet/in.h>
#include "../log/log.h"

class util_timer;

// 每个连接绑定的客户端数据
struct client_data {
    sockaddr_in address;   // socket 地址
    int sockfd;            // socket fd
    util_timer* timer;     // 指向该连接的定时器
};

// 定时器节点
class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

    time_t expire;                      // 超时时间（绝对时间戳）
    void (*cb_func)(client_data*);      // 超时回调函数
    client_data* user_data;             // 指向该连接的 client_data
    util_timer* prev;
    util_timer* next;
};

// 升序双向链表
class sort_timer_lst {
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    ~sort_timer_lst();

    void add_timer(util_timer* timer);
    void adjust_timer(util_timer* timer);
    void del_timer(util_timer* timer);
    void tick();                        // 心跳：处理所有到期的定时器

private:
    void add_timer(util_timer* timer, util_timer* lst_head);
    util_timer* head;
    util_timer* tail;
};

#endif
```

### Step 3：链表的四个核心操作

```cpp
// lst_timer.cpp
void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer) return;
    if (!head) { head = tail = timer; return; }

    // 如果新定时器比头还早，插在头部
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);  // 否则从 head 开始找插入位置
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) {
    util_timer* prev = lst_head;
    util_timer* tmp = prev->next;
    while (tmp) {
        if (timer->expire < tmp->expire) {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 到最后还没找到 → 插在尾部
    if (!tmp) {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

void sort_timer_lst::adjust_timer(util_timer* timer) {
    // 连接有活动 → 超时时间往后延
    // 如果当前节点仍在正确位置（比后继小），就不用动
    if (!timer) return;
    util_timer* tmp = timer->next;
    if (!tmp || (timer->expire < tmp->expire)) return;

    // 从链表摘下
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    } else {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer* timer) {
    if (!timer) return;
    // 链表只有一个节点
    if (timer == head && timer == tail) {
        delete timer;
        head = tail = NULL;
        return;
    }
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::tick() {
    if (!head) return;
    time_t cur = time(NULL);

    util_timer* tmp = head;
    while (tmp) {
        if (cur < tmp->expire) break;  // 链表有序：第一个未过期的后面都未过期

        tmp->cb_func(tmp->user_data);  // 执行回调（关闭连接）
        head = tmp->next;
        if (head) head->prev = NULL;
        delete tmp;
        tmp = head;
    }
}
```

### Step 4：用信号驱动 tick

`tick()` 需要被定期调用。最自然的方式是用 `SIGALRM`（闹钟信号）+ `alarm()`：

```cpp
// Utils 工具类
class Utils {
public:
    void init(int timeslot) { m_TIMESLOT = timeslot; }

    int  setnonblocking(int fd);
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    static void sig_handler(int sig);
    void addsig(int sig, void(handler)(int), bool restart = true);
    void timer_handler();
    void show_error(int connfd, const char* info);

public:
    static int* u_pipefd;      // 信号管道
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
};
```

### Step 5：信号 → 管道 → epoll

信号处理函数能做的工作非常有限（必须是可重入函数）。项目中用 `socketpair` 创建管道把信号转换为主循环中的普通 IO 事件：

```cpp
// 创建管道（在 WebServer::eventListen 中调用）
socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);

// 信号处理器：往管道写一个字节
void Utils::sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

// 注册信号
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时任务：执行 tick + 重新设置 alarm
void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);  // 每隔 TIMESLOT 秒再次触发 SIGALRM
}
```

**信号到 epoll 的完整流：**

```
alarm(5) → 5 秒后 → SIGALRM → sig_handler → send(m_pipefd[1]) 
→ epoll 检测到 m_pipefd[0] 可读 → dealwithsignal → timer_handler → tick()
```

### Step 6：超时回调函数

```cpp
int* Utils::u_pipefd = 0;
int  Utils::u_epollfd = 0;

void cb_func(client_data* user_data) {
    // 从 epoll 移除
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;  // 活跃连接数减 1
}
```

### Step 7：创建和调整定时器

```cpp
// 新连接到来时创建定时器
void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer* timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    timer->expire = time(NULL) + 3 * TIMESLOT;  // 3 个周期后超时

    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 连接有活动时调整（延后超时时间）
void WebServer::adjust_timer(util_timer* timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
}
```

### Step 8：gdb 信号调试

```bash
gdb ./build/server

(gdb) handle SIGALRM nostop noprint pass
# 让 SIGALRM 不打断调试（nostop）、不打印（noprint）、交给程序（pass）

(gdb) handle SIGPIPE nostop noprint pass
# SIGPIPE 也类似处理——往已关闭的 socket 写数据时会触发
```

---

## 验证方法

- [ ] 启动 echo 服务器（Phase 7），telnet 连接后 15 秒不发送数据 → 连接自动断开
- [ ] 连接后持续发送数据 → 连接保持不断
- [ ] 日志中出现 "adjust timer once" 和 "close fd N" 说明定时器正常工作

---

## 踩坑记录

1. **`alarm` 和 `sleep` 冲突。** `alarm` 和 `sleep` 都会使用 `SIGALRM`，不要混用。本项目只用 `alarm`。

2. **信号处理函数的限制。** `sig_handler` 中只能调用异步信号安全的函数（`send` 是安全的，`printf` 不是！）。这就是为什么通过管道转发给主循环处理。

3. **链表未排序？** `add_timer` 每次从头部线性查找插入位置。O(n) 对几万连接还行，但如果是几十万连接就应改用时间轮（timer wheel）。

4. **`timer_handler` 中的 `alarm` 会覆盖之前的。** `alarm` 设置的是单次闹钟。每次 `tick` 之后必须重新 `alarm(m_TIMESLOT)`，否则闹钟只响一次。

---

## 阶段小结

你实现了：
- 升序双向链表定时器（add、adjust、del、tick）
- SIGALRM + socketpair 的信号转 IO 机制
- 超时回调自动关闭无活动连接

`timer_handler` 将在 `eventLoop` 的每次 tick 中被调用，维持整个服务器的连接健康。

下一阶段：**HTTP 解析器**——这是整个项目最复杂的模块，用状态机解析 HTTP 请求并生成响应。
