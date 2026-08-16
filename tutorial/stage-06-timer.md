# Stage 6：定时器（清理非活跃连接）

> 本阶段目标：给 Stage 5 的 HTTP 服务器加上"超时踢人"能力——用一条**升序链表**管理所有连接的超时时间，配合 `alarm` 周期信号与 epoll 的**统一事件源**，让"连上之后啥也不干"的挂死连接在 15 秒后被自动关闭，同时保证"活跃连接"永远不会被误杀。完成后你的服务器能扛住大量空闲连接而不耗尽文件描述符。

学完本阶段，你会掌握：双向链表的四类边界操作、函数指针回调、static 成员跨文件定义、Linux 信号（SIGALRM / SIGPIPE / SIGTERM）与 `socketpair` 管道。

---

## 前置要求

- 已完成 [Stage 5：HTTP 服务器](stage-05-http.md)，工作区状态如下（本阶段起点，**必须与之一致**）：

```text
my_tiny_webserver/
├── lock/locker.h                 # 与仓库一致
├── threadpool/threadpool.h       # 简化版：threadpool(int thread_number=8,int max_requests=10000)
│                                 #          bool append(T*)；run() 调用 request->process()
├── http/http_conn.h              # Stage 5 简化版（与仓库同构）
├── http/http_conn.cpp            # 含 static int m_epollfd、static int m_user_count 的定义
├── server.cpp                    # epoll + 线程池 proactor 版 HTTP 主程序
├── root/index.html
└── makefile
```

- 学习者版 `http_conn` 的关键公开接口（本阶段会用到，务必确认已存在）：

```cpp
// 节选：my_tiny_webserver/http/http_conn.h（Stage 5 已写好的部分）
class http_conn
{
public:
    // 简化点：学习者版 init 只有 4 个参数（仓库版是 8 个，Stage 7/8 逐步补全）
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode);
    void close_conn(bool real_close = true);
    void process();                          // 线程池 run() 会调用它
    bool read_once();                        // 读一次请求（成功返回 true）
    bool write();                            // 写响应（还需继续发返回 true，应关闭返回 false）
    sockaddr_in *get_address() { return &m_address; }

public:
    static int m_epollfd;                    // 必须在 http_conn.cpp 里定义（如 int http_conn::m_epollfd = -1;）
    static int m_user_count;                 // 必须在 http_conn.cpp 里定义（如 int http_conn::m_user_count = 0;）
    // ... 其余成员略（Stage 5 版没有 m_state，它要到 Stage 8 升级线程池时才出现）
};
```

- 前置知识：epoll 事件循环、线程池、信号量的基本概念（Stage 3/4/5）；`alarm`/`signal` 等 POSIX 信号 API 本阶段现学即可。

---

## 理论学习

### 1. 非活跃连接问题：为什么必须"踢掉挂死连接"

服务器用 `accept` 接受连接后，每个连接都会占用一个**文件描述符（fd）**，同时占用 `http_conn` 对象、读缓冲区等内存资源。但现实中大量连接是"半死不活"的：

- 客户端发起连接后**不发送任何数据**（僵尸连接）；
- 客户端网线被拔、进程崩溃、中间网络中断，但服务器**收不到 FIN**（半开连接）；
- 客户端恶意只连接不请求，慢慢耗死你的服务器（**慢速攻击**）。

如果这些连接永远不被清理，每个只占一个 fd 的"尸体"最终会把 fd 用光：

```text
客户端 A ──连接──▶ 服务器（占用 fd 4，之后不再说话）
客户端 B ──连接──▶ 服务器（占用 fd 5，之后不再说话）
   ...
客户端 N ──连接──▶ 服务器（占用 fd 65535，之后不再说话）   ← fd 表耗尽！
新客户端 ──连接──▶ ✗ accept 返回 EMFILE，无法服务新用户
```

操作系统的 fd 数量是有上限的（`ulimit -n`，默认 1024，服务器通常调到几万）。**谁先占用谁先得，但没人释放就谁都得不到。** 解决思路很朴素：给每个连接挂一个"倒计时闹钟"——超过 N 秒没有数据往来，就主动 `close` 掉它；一有数据往来就"续命"。这个闹钟，就是**定时器**。

### 2. 常见定时器容器对比

定时器需要支持三种操作：**添加**（新连接）、**删除**（连接主动关闭）、**调整**（连接有活动，续期）、以及**到期检测**（周期性地找出哪些超时了）。不同数据结构有不同的取舍：

| 容器 | 添加 | 删除 | 找最小到期 | 到期处理 | 说明 |
|---|---|---|---|---|---|
| **升序链表** | O(n) | O(1)（已知结点指针） | O(1)（就是头结点） | 从头部逐个处理 | 实现最简单，代码易读；连接数不大时性能完全够用 |
| 时间轮 | O(1) | O(1) | 轮盘转动 | 每个槽批量处理 | 适合海量定时器，但精度受槽数限制，代码复杂 |
| 时间堆（最小堆） | O(log n) | O(log n) | O(1)（堆顶） | 弹堆顶 | 综合效率高，但堆调整代码对初学者不友好 |

**本项目为什么选升序链表？** 三个原因：

1. 这是**教学项目**，目标是让你彻底理解"定时器"这件事本身，链表最直观；
2. 本项目连接数通常不大（个人服务器），O(n) 插入完全可接受；
3. 链表删除是 O(1)，而"连接主动关闭"恰好是高频操作——`adjust_timer` 也只需要"把结点向后挪"，链表做这个很自然。

本阶段结束后你会明白：**升序链表里，越靠前 expire 越小，tick 时从头部开始处理，一遇到"还没到期"的结点就可以 break**（因为后面所有结点到期时间更晚）。

### 3. SIGALRM 与 alarm：一个"周期闹钟"信号

Linux 里 `alarm(seconds)` 会在 `seconds` 秒后向**当前进程**发送一个 `SIGALRM` 信号：

```cpp
alarm(5);   // 5 秒后，进程收到 SIGALRM 信号
```

`SIGALRM` 默认动作是**终止进程**。所以你必须为它注册一个处理函数（`sigaction`），否则服务器每 5 秒自杀一次。本项目的用法是：

```text
alarm(TIMESLOT)  ──(5秒后)──▶ SIGALRM ──▶ sig_handler ──▶ 写管道 ──▶ epoll 唤醒 ──▶ timer_handler() 里 tick() 后又 alarm(TIMESLOT)
```

注意 `alarm` 是**一次性**的：到期触发一次后就没了。所以 `timer_handler()` 里处理完链表后要**重新 `alarm(m_TIMESLOT)`**，形成"周期闹钟"。`alarm(0)` 会取消之前设定的闹钟。

### 4. 统一事件源：为什么信号要走 epoll

服务器的主循环是 `epoll_wait` 阻塞等待事件。现在突然多了"信号"这种**不属于任何 fd 的事件**，怎么让主循环既等 socket 又等信号，还能不忙轮询？

**统一事件源（Unified Event Source）** 的思路：把"信号"翻译成"某个 fd 可读"，于是主循环只监听 epoll 就够了。具体做法：

```text
SIGALRM / SIGTERM ──▶ sig_handler（信号处理函数，只做一件事）
                              │
                              │ send(pipefd[1], &sig, 1, 0)   ← 写 1 字节到 socketpair 管道
                              ▼
                        socketpair 管道（pipefd[0] <──> pipefd[1]）
                              │
              pipefd[0] 变得可读，被 epoll 监听到
                              │
                              ▼
              eventLoop 里 recv(pipefd[0]) 读到信号值 → 执行 tick() 或退出
```

- `socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd)` 创建一对**本地全双工** socket：往 `pipefd[1]` 写，`pipefd[0]` 就能读。它和 `pipe()` 的区别是全双工、且两端都是 socket（可设非阻塞）。
- `pipefd[0]` 用 `addfd` 注册进 epoll（监听 `EPOLLIN`），于是"信号到了"就等价于"`pipefd[0]` 可读了"，主循环无需任何轮询。

**为什么不能在信号处理函数里做复杂操作？——可重入性**

信号处理函数是**异步**执行的：它可能在主程序任意一行代码执行到一半时被内核"插队"调用。如果处理函数里调用 `printf`、`malloc`、拿锁、遍历链表等，而被打断的那行代码恰好也在用同一把锁 / 同一个缓冲区，就会发生**死锁或数据错乱**。这类"被打断后还能安全执行"的函数必须是**异步信号安全（async-signal-safe）**的，POSIX 只保证极少数函数安全：`write`、`send`、`read`、`recv`、`close`、`_exit` 等（`printf`、`malloc`、`lock` 都不在其中）。

所以本项目 `sig_handler` 只做一件最安全的事——**把信号编号塞进管道**，把真正的"重活"（tick 链表、关连接）留给主循环在正常上下文中做：

```cpp
void Utils::sig_handler(int sig)
{
    int save_errno = errno;          // 先备份 errno，见下
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);   // 只写 1 字节，async-signal-safe
    errno = save_errno;              // 还原，避免污染主流程的 errno
}
```

**为什么先保存再恢复 errno？** 信号处理函数可能在主流程"刚设完 errno、正准备读它"的间隙插入。如果处理函数里调用的 `send` 失败了（改写了 `errno`），主流程随后读到的 `errno` 就是被污染的假值。所以处理函数要**像借书一样：先备份，用完归还**。

### 5. SIGPIPE：为什么要忽略它

当服务器向一个**已经被对方关闭**的 socket 写数据时，内核会向进程发送 `SIGPIPE` 信号，默认动作是**终止进程**。在 Web 服务器里，客户端随时可能粗暴断开（刷新、关页面、网络抖动），服务器却还在 `write`——如果不忽略 `SIGPIPE`，一次普通的客户端断开就能把整个服务器打死：

```cpp
utils.addsig(SIGPIPE, SIG_IGN);   // 忽略 SIGPIPE：write 会返回 -1 并置 errno=EPIPE，而不是杀死进程
```

忽略后，`write` 会正常返回 `-1`、`errno == EPIPE`，我们把这次写当作"连接该关了"处理即可。

### 6. SIGTERM：优雅退出

`kill -TERM <pid>` 是运维最常用的"请程序退出"信号（`kill` 默认就是 TERM）。优雅退出的含义是：**不要死在任意一行代码上，要让主循环自然走到退出点、顺手关掉 fd 释放资源**。本项目的做法是：把 `SIGTERM` 也接进统一事件源，主循环读到它后把 `stop_server` 置 true，`while` 循环退出，走 cleanup 代码。这就是"优雅"。

---

## 本阶段 C++ 知识点

### 1. 链表：双向链表与四类边界情况

本阶段核心数据结构是 `sort_timer_lst`——一条**按 `expire` 升序**排列的**双向链表**。每个结点 `util_timer` 有 `prev` 和 `next` 两个指针，链表有 `head`（头）和 `tail`（尾）哨兵指针：

```text
        ┌──────────────┐        ┌──────────────┐        ┌──────────────┐
head ─▶ │ expire = 10  │ ◀────▶ │ expire = 25  │ ◀────▶ │ expire = 40  │ ◀── tail
        │ prev  = NULL │        │ prev  = ...  │        │ prev  = ...  │
        │ next  = ...  │        │ next  = ...  │        │ next  = NULL │
        └──────────────┘        └──────────────┘        └──────────────┘
        （头结点，最小 expire）                            （尾结点，最大 expire）
```

链表的难点不在"会写"，而在**边界情况不漏**。插入一个结点，按位置分三种：

```text
① 空链表：head = tail = timer；
② 头插（timer->expire < head->expire）：新结点成为新头；
③ 中间/尾插：从头向后找到第一个 expire 更大的结点，插到它前面。
```

删除一个结点，分四种：

```text
① 唯一结点（timer == head == tail）：删完 head = tail = NULL；
② 头结点：head 后移，新头 prev = NULL；
③ 尾结点：tail 前移，新尾 next = NULL；
④ 中间结点：前驱 next 指向后继，后继 prev 指向前驱。
```

**调整（adjust）** 一个结点：把它的 `expire` 改大后，它只可能向**链表后方**挪（原理见 `adjust_timer` 讲解），所以只需"摘下来 + 向后插入"。

> 记忆口诀：**插入看前后，删除看头尾，唯一结点单拎出来**。

### 2. 函数指针：回调机制

```cpp
void (* cb_func)(client_data *);   // 声明一个函数指针成员
```

`cb_func` 是一个**指向"参数为 `client_data*`、返回 void 的函数"的指针**。每个定时器结点存一个这样的指针，tick 时调用它：

```cpp
tmp->cb_func(tmp->user_data);   // 通过函数指针调用回调
```

为什么用回调？因为"超时后做什么"（关 fd、减计数）是**使用者**决定的，而链表类 `sort_timer_lst` 只负责"管理定时"。链表类不关心回调内容，只负责在正确时刻调用它——这就是**解耦**。本项目里回调统一指向 `cb_func`（关闭连接）。

> 读函数指针技巧：把 `(*cb_func)` 看成一个函数名。`void (*cb_func)(client_data *)` 读作"cb_func 是一个指针，指向接受 client_data*、返回 void 的函数"。

### 3. static 成员：为什么 `u_pipefd` / `u_epollfd` 用 static

```cpp
class Utils {
public:
    static int *u_pipefd;   // 只有声明，没有定义
    static int u_epollfd;
};
// lst_timer.cpp 里：
int *Utils::u_pipefd = 0;   // 这才是"定义"，分配真实存储空间
int Utils::u_epollfd = 0;
```

- `static` 成员**属于类本身，不属于任何对象**：所有 `Utils` 对象共享同一份。
- 为什么要 static？因为 `sig_handler` 是**静态成员函数**（信号处理函数必须是 `static`，否则它隐含带 `this` 指针、签名不符），而静态成员函数**只能访问静态成员**——它要往管道写、要从 epoll 删除 fd，就必须能拿到 `u_pipefd`、`u_epollfd`，所以这两个变量必须是 static。
- **声明 ≠ 定义**：类内只是"声明"（告诉编译器有这个东西），必须在**某一个 .cpp 里写一次定义**分配内存。漏掉定义会得到链接错误 `undefined reference to 'Utils::u_pipefd'`。

### 4. 前向声明

```cpp
class util_timer;        // 前向声明：告诉编译器"util_timer 是一个类，稍后定义"
struct client_data {
    ...
    util_timer *timer;   // 这里只需要"指针"，不需要完整定义，所以前向声明就够
};
```

当某个类只需**指针/引用**指向另一个类型（不访问其成员、不算大小）时，前向声明即可，避免头文件互相包含造成死循环。

### 5. struct 与 class 的区别

本项目 `client_data` 是 struct，`util_timer` / `sort_timer_lst` / `Utils` 是 class。两者在 C++ 里**唯一区别是默认访问权限**：

| | 默认访问权限 |
|---|---|
| `struct` | `public` |
| `class` | `private` |

`client_data` 全是公开数据、没有行为（就是"一捆数据"），所以用 struct；有行为、需要封装内部状态（head/tail）的用 class。**仅此而已**，其余完全等价。

---

## 动手实现

本阶段新增 2 个文件 + 改 2 个文件（`server.cpp`、`makefile`）。`http_conn.*` 无需改动。

### 步骤 1：`my_tiny_webserver/timer/lst_timer.h`

完整文件如下（与仓库 `timer/lst_timer.h` 逐行对应）：

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
// 简化点：仓库版这里还有 #include "../log/log.h"，但本头文件其实没用到任何 Log 符号，
// 属于历史遗留。学习者 Stage 6 还没写日志模块，故省略；Stage 7 想逐行对齐可补回。

class util_timer;    // 前向声明：client_data 里只需 util_timer 的指针

struct client_data
{
    sockaddr_in address;    // 客户端地址
    int sockfd;             // 连接对应的 socket 描述符
    util_timer *timer;      // 指向该连接绑定的定时器
};

class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;                      // 到期时间（绝对时间戳，单位秒）

    void (* cb_func)(client_data *);    // 回调函数指针：到期后调用
    client_data *user_data;             // 回调参数：指向对应的 client_data
    util_timer *prev;                   // 前驱
    util_timer *next;                   // 后继
};

class sort_timer_lst
{
public:
    sort_timer_lst();
    ~sort_timer_lst();

    void add_timer(util_timer *timer);      // 添加定时器（自动按 expire 升序插入）
    void adjust_timer(util_timer *timer);   // 调整定时器（expire 变大后向后挪）
    void del_timer(util_timer *timer);      // 删除定时器
    void tick();                            // 心搏函数：处理所有已到期定时器

private:
    void add_timer(util_timer *timer, util_timer *lst_head);  // 重载：从 lst_head 起向后找位置插入

    util_timer *head;   // 链表头
    util_timer *tail;   // 链表尾
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将 fd 注册进内核事件表（读事件；TRIGMode==1 用 ET，否则 LT；可选 EPOLLONESHOT）
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数（static：信号处理函数不能带 this）
    static void sig_handler(int sig);

    // 设置信号处理函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务：tick 后重新 alarm，周期性触发 SIGALRM
    void timer_handler();

    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;   // 统一事件源的管道（静态，供 sig_handler 使用）
    sort_timer_lst m_timer_lst;
    static int u_epollfd;   // epoll 句柄（静态，供 cb_func 使用）
    int m_TIMESLOT;
};

void cb_func(client_data *user_data);   // 回调函数声明（定义在 lst_timer.cpp）

#endif
```

**逐成员讲解：**

- `client_data`：连接与定时器的"绑定物"。`sockfd` 是要管理的连接，`timer` 指向它的闹钟。数组 `client_data users_timer[MAX_FD]` 以 fd 为下标，实现 `fd → 定时器` 的 O(1) 查询。
- `util_timer`：链表结点。`expire` 用**绝对时间戳**（`time(NULL) + 3*TIMESLOT`），比较起来简单直观。`cb_func` 是回调指针，`user_data` 是回调要用的参数。
- `sort_timer_lst`：链表管理器。对外暴露 add/adjust/del/tick 四个操作，`head`/`tail` 私有。私有重载 `add_timer(timer, lst_head)` 用于"从指定位置开始向后找插入点"。
- `Utils`：工具类，收拢 fd 与信号的零碎操作。注意 `u_pipefd`/`u_epollfd` 是 static（见"本阶段 C++ 知识点"），`m_timer_lst` 是**对象成员**（每个 Utils 实例一条链表）。

### 步骤 2：`my_tiny_webserver/timer/lst_timer.cpp`

完整文件如下（与仓库 `timer/lst_timer.cpp` 基本一致，含 2 处简化点标注）：

```cpp
#include "lst_timer.h"
#include "../http/http_conn.h"
// 简化点：仓库版 lst_timer.cpp 头两行就是这两条 include。学习者版 include 的是
// 自己 my_tiny_webserver/http/http_conn.h，里面已有 public 的 static int m_user_count，
// 与仓库同构，所以这条 include 路径照抄即可。

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
        delete tmp;         // 逐个释放链表结点
        tmp = head;
    }
}

// 对外 add_timer：先处理空链表、头插两个边界，其余交给私有重载
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)                       // ① 空链表：头尾都指向它
    {
        head = tail = timer;
        return;
    }
    if (timer->expire < head->expire)  // ② 头插：新结点到期最早
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);          // ③ 中间/尾插：从 head 起向后找
}

void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;
    // 到期时间被改大后，只需和"后一个结点"比：没有后一个，或仍比后一个早，就不用动
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }
    if (timer == head)               // 被调整的是头结点：头后移，再把 timer 插回
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else                             // 中间/尾结点：摘下后向后插
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))   // ① 唯一结点
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)                        // ② 头结点
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)                        // ③ 尾结点
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;          // ④ 中间结点
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    time_t cur = time(NULL);      // 当前时间
    util_timer *tmp = head;
    while (tmp)
    {
        if (cur < tmp->expire)    // 升序：头部是最早到期的，一旦没到期，后面全没到期
        {
            break;
        }
        tmp->cb_func(tmp->user_data);  // 先回调（关闭连接）
        head = tmp->next;              // 把已处理结点从链表头摘掉
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;                    // 释放定时器结点
        tmp = head;
    }
}

// 私有重载：从 lst_head 起向后找第一个 expire 更大的结点，插到它前面
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
    if (!tmp)                    // 走到链表尾还没找到更大的：插到末尾
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

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件；TRIGMode==1 用 ET，否则 LT；one_shot 决定是否加 EPOLLONESHOT
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

// 信号处理函数：只做"把信号编号写进管道"这一件安全的事
void Utils::sig_handler(int sig)
{
    int save_errno = errno;                 // 备份 errno，保证可重入
    int msg = sig;
    send(u_pipefd[1], (char *)&msg, 1, 0);  // 只写 1 字节（SIGALRM=14、SIGTERM=15 都 <256）
    errno = save_errno;                     // 归还 errno
}

// 设置信号处理函数
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

// 定时处理任务：tick 之后重新 alarm，不断触发 SIGALRM
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::u_pipefd = 0;   // 静态成员的定义（跨文件，只此一份）
int Utils::u_epollfd = 0;

class Utils;                // 简化点：这是仓库原样照搬的一行冗余前向声明（Utils 已由头文件完整定义），
                            // 留着不影响编译，写上来只为与仓库逐行一致。

void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);  // 从 epoll 移除
    assert(user_data);                                                 // 注意：仓库把 assert 放这里，理论上应在解引用前
    close(user_data->sockfd);                                          // 关闭连接
    http_conn::m_user_count--;                                         // 在线连接数减一
}
```

**逐函数讲解：**

- **`add_timer(timer)`（对外）**：先排掉 `timer==NULL`、空链表、头插三种情况。空链表时头尾都是它；头插时新结点 exp 比旧头还小，成为新头；否则走私有重载。
- **`add_timer(timer, lst_head)`（私有重载）**：从 `lst_head` 的**后继**开始找，找到第一个 `expire` 更大的结点就插到它前面；若 `while` 走完 `tmp==NULL`，说明该结点到期最晚，插到末尾并更新 `tail`。
- **`adjust_timer`**：这是本阶段最精妙的一处。它的前置条件是"调用者已把 `timer->expire` 改大"（`expire = cur + 3*TIMESLOT`，只会比原来晚）。因为链表按 expire 升序，**结点只会往链表后方挪，绝不会向前挪**，所以只需要：① 若没有后继，或新的 expire 仍小于后继的 expire，原地不动；② 否则摘下来，从头/从原后继起向后重新插入。这也是思考题 2 的答案。
- **`del_timer`**：四种边界见知识点 1。注意每一路都 `delete timer`，避免内存泄漏。
- **`tick`**：取当前时间，从 `head` 逐个看；因为升序，`cur < tmp->expire` 即可 `break`。到期结点先回调（回调里关闭连接、移除 epoll、减计数），再从链表摘掉并 `delete`。
- **`sig_handler`**：见"理论学习 4"。`send` 只发 1 字节是因为信号编号都小于 256，够用。
- **`addsig`**：`sigaction` 注册；`SA_RESTART` 决定"被该信号打断的系统调用是否自动重启"；`sigfillset` 在处理期间屏蔽所有信号，避免嵌套。
- **`timer_handler`**：先 `tick()` 处理到期定时器，再 `alarm(m_TIMESLOT)` 续上下一轮闹钟。
- **`cb_func`**：超时回调。从 epoll 删掉 fd → `close` → `m_user_count--`。注意它**不删除定时器结点**——删除由 `tick()` 完成（分工：回调负责"关闭连接"，链表负责"回收结点"）。

> 简化点总结：与仓库唯一的功能性差异是——仓库版 `cb_func` 里 `http_conn::m_user_count--` 引用的是它自己的 `http_conn.h`，学习者版引用的也是**你自己 Stage 5 写好的** `http_conn.h`（已含 public 的 `static int m_user_count` 且已在 http_conn.cpp 定义），二者同构，无需额外改动。另：仓库 lst_timer.cpp 头部 `class Utils;` 与 `assert` 位置属原样保留的瑕疵/冗余，本教程忠实照抄并标注。

### 步骤 3：集成进 `my_tiny_webserver/server.cpp`

学习者版**没有 `WebServer` 类**，把定时器逻辑平铺进 `main` 与几个自由函数（标注：Stage 9 会把这些收拢成 `WebServer` 类的成员函数，仓库 `webserver.cpp` 的 `timer/adjust_timer/deal_timer/dealwithsignal` 就是下面这些逻辑的"成员函数形态"）。

完整文件如下：

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>

#include "timer/lst_timer.h"
#include "http/http_conn.h"
#include "threadpool/threadpool.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

// ---- 全局变量（简化点：仓库版这些是 WebServer 类的成员，Stage 9 会收拢回类中）----
static int listenfd = -1;
static int epollfd = -1;
static int pipefd[2];
static int trig_mode = 0;          // 0 = listenfd LT + connfd LT（仓库支持 0~3，见 Stage 9）
static char *doc_root = NULL;
static http_conn *users = NULL;           // 连接对象数组，下标 = fd
static client_data *users_timer = NULL;   // 定时器绑定数组，下标 = fd
static Utils utils;
static threadpool<http_conn> *pool = NULL;

// 新连接：初始化 http_conn，并创建、绑定一个定时器
void timer(int connfd, struct sockaddr_in client_address)
{
    // 简化点：仓库版 init 有 8 个参数，学习者版目前 4 个（Stage 7/8 逐步补全）
    users[connfd].init(connfd, client_address, doc_root, trig_mode);

    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *t = new util_timer;
    t->user_data = &users_timer[connfd];
    t->cb_func = cb_func;
    time_t cur = time(NULL);
    t->expire = cur + 3 * TIMESLOT;   // 15 秒不活动即超时（3 个 TIMESLOT）
    users_timer[connfd].timer = t;
    utils.m_timer_lst.add_timer(t);
}

// 有数据往来：续命 15 秒，并把定时器在链表中向后调整
void adjust_timer(util_timer *t)
{
    time_t cur = time(NULL);
    t->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(t);
    printf("adjust timer once\n");
}

// 主动关闭连接：触发回调（关 fd、减计数）并删除定时器结点
void deal_timer(util_timer *t, int sockfd)
{
    t->cb_func(&users_timer[sockfd]);
    if (t)
    {
        utils.m_timer_lst.del_timer(t);
    }
    printf("close fd %d\n", users_timer[sockfd].sockfd);
}

// 处理新连接（LT：每次 accept 一个）
bool dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    // 简化点：只演示 LT。仓库版按 LISTENTrigmode 分 LT/ET 两套（ET 要循环 accept 到 EAGAIN）
    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
    if (connfd < 0)
    {
        printf("accept error, errno is:%d\n", errno);
        return false;
    }
    if (http_conn::m_user_count >= MAX_FD)
    {
        utils.show_error(connfd, "Internal server busy");
        printf("Internal server busy\n");
        return false;
    }
    timer(connfd, client_address);
    return true;
}

// 处理信号：从管道读信号字节，映射成 timeout / stop_server
bool dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1 || ret == 0)
    {
        return false;
    }
    for (int i = 0; i < ret; ++i)
    {
        switch (signals[i])
        {
        case SIGALRM:
        {
            timeout = true;
            break;
        }
        case SIGTERM:
        {
            stop_server = true;
            break;
        }
        }
    }
    return true;
}

// 读事件：proactor——主线程替工作线程读好数据，成功则续命并投递线程池
void dealwithread(int sockfd)
{
    util_timer *t = users_timer[sockfd].timer;
    if (users[sockfd].read_once())
    {
        printf("deal with the client(%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        pool->append(users + sockfd);
        if (t)
        {
            adjust_timer(t);
        }
    }
    else
    {
        deal_timer(t, sockfd);
    }
}

// 写事件：proactor——主线程替工作线程发数据，成功则续命，失败则关闭
void dealwithwrite(int sockfd)
{
    util_timer *t = users_timer[sockfd].timer;
    if (users[sockfd].write())
    {
        printf("send data to the client(%s)\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if (t)
        {
            adjust_timer(t);
        }
    }
    else
    {
        deal_timer(t, sockfd);
    }
}

int main(int argc, char *argv[])
{
    // 1. 站点根目录：getcwd 得到运行目录 + "/root"
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    doc_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(doc_root, server_path);
    strcat(doc_root, root);

    // 2. 监听 socket（复用地址，避免重启时 TIME_WAIT 报错）
    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(9006);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // 3. epoll
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    utils.addfd(epollfd, listenfd, false, 0);   // 监听 socket 用 LT，不开 oneshot
    http_conn::m_epollfd = epollfd;

    // 4. 统一事件源：socketpair 管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    utils.setnonblocking(pipefd[1]);
    utils.addfd(epollfd, pipefd[0], false, 0);  // pipefd[0] 注册读事件（LT，读完信号）

    // 5. 信号设置（先于 alarm：让 Utils 的静态指针就位，见常见问题 7）
    Utils::u_pipefd = pipefd;
    Utils::u_epollfd = epollfd;

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);
    alarm(TIMESLOT);                              // 启动周期闹钟

    // 6. 资源与线程池
    users = new http_conn[MAX_FD];
    users_timer = new client_data[MAX_FD];
    pool = new threadpool<http_conn>(8, 10000);   // 简化点：线程池放堆上，通过全局指针访问

    // 7. 事件循环
    bool timeout = false;
    bool stop_server = false;
    while (!stop_server)
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

            if (sockfd == listenfd)                                  // 新连接
            {
                bool f = dealclientdata();
                if (!f)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))  // 对端断开/出错
            {
                util_timer *t = users_timer[sockfd].timer;
                deal_timer(t, sockfd);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))   // 信号
            {
                bool f = dealwithsignal(timeout, stop_server);
                if (!f)
                    printf("dealwithsignal failure\n");   // 简化点：仓库这里文案误写为 dealclientdata，已改正
            }
            else if (events[i].events & EPOLLIN)                        // 读事件
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)                       // 写事件
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)                                   // 本轮收到了 SIGALRM → 做一次超时检查
        {
            utils.timer_handler();
            printf("timer tick\n");
            timeout = false;
        }
    }

    // 8. 清理
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
```

**集成要点逐条讲：**

1. **`timer(connfd, address)`**（对应仓库 `WebServer::timer`）：`init` 一个连接，再 `new` 一个 `util_timer`，把 `cb_func` 指向 `cb_func`、`user_data` 指向 `&users_timer[connfd]`，`expire = now + 3*TIMESLOT`（15 秒），最后 `add_timer` 入链。
2. **`adjust_timer`**（对应 `WebServer::adjust_timer`）：续命到 `now + 15s`，再让链表调整位置。
3. **`deal_timer`**（对应 `WebServer::deal_timer`）：先 `cb_func` 关闭连接，再 `del_timer` 回收结点。
4. **`dealclientdata`**：accept 一个新连接，满了就 `show_error` 发"Internal server busy"并关闭。
5. **`dealwithsignal`**（对应 `WebServer::dealwithsignal`）：从 `pipefd[0]` 读信号字节；`SIGALRM → timeout=true`，`SIGTERM → stop_server=true`。
6. **eventLoop 新增三个分支**：`(EPOLLRDHUP|EPOLLHUP|EPOLLERR)` 分支直接 `deal_timer`；`pipefd[0]` 可读分支调 `dealwithsignal`；循环末尾 `if(timeout)` 调 `timer_handler()`（tick + 重置 alarm）并打印 `timer tick`。
7. **读/写分支里"续命"**：proactor 下主线程读/写成功就 `adjust_timer`，失败就 `deal_timer`——这正是"活跃连接不断线、挂死连接 15 秒被踢"的机制。

### 步骤 4：更新 `my_tiny_webserver/makefile`

```makefile
CXX ?= g++
CXXFLAGS += -g -Wall

# 简化点：仓库版还编译 CGImysql/log 并链接 -lmysqlclient，Stage 7/8 再补
server: server.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread

clean:
	rm -f server
```

---

## 编译与运行

```bash
cd ~/projects/my_tiny_webserver
make          # 或：g++ -o server server.cpp timer/lst_timer.cpp http/http_conn.cpp -lpthread
./server
```

预期：服务器启动后**无输出**（epoll_wait 阻塞等待），约 5 秒后开始**每 5 秒打印一行 `timer tick`**。

---

## 验收清单

每行都是一条"命令/操作 + 明确预期输出"，全部打勾才算过关。

- [ ] **编译通过**：`make` 输出 `g++ -o server ...`，无 error，目录出现可执行文件 `server`（`ls -l server` 有 x 权限）。
- [ ] **周期信号链路通**：前台运行 `./server`，5 秒后终端开始每 5 秒出现一行 `timer tick`，且进程不退出（说明 SIGALRM → 管道 → epoll → timer_handler 全链路正常）。
- [ ] **观察进程**：另开终端 `ps -ef | grep server`，能看到 `./server` 进程及其 PID（记录下这个 PID，后面要用）。
- [ ] **空闲连接 15 秒被断开**：另开终端执行 `nc 127.0.0.1 9006`，**连接后不输入任何字符**；约 15 秒后 `nc` 自动退出（回到 shell 提示符）。注意：这条路径是 `tick()` 直接调 `cb_func` 关闭连接，服务器终端**不会**打印 `close fd`（那行输出属于 `deal_timer` 的"读写出错/对端断开"路径），只会继续每 5 秒一行 `timer tick`。想看到"连接数减一"的证据，按附录 B 案例 c 用 gdb 在 `cb_func` 断点观察 `http_conn::m_user_count`。
- [ ] **活跃连接不断开**：再执行 `nc 127.0.0.1 9006`，连接后**每隔 2~3 秒随便敲几个字符并回车**（如 `aa`、`bb`）；持续 30 秒以上 `nc` 依然保持连接不退出，服务器终端反复打印 `adjust timer once`。
- [ ] **SIGTERM 优雅退出**：在服务器终端 `Ctrl+Z` 不行——正确做法：另开终端执行 `kill -TERM <上面记录的 PID>`；服务器进程**正常退出**（前台终端回到 shell），随后 `echo $?` 输出 `0`。
- [ ] **gdb 观察定时器链表**：`gdb ./server`，进入后依次输入：
  ```text
  handle SIGALRM nostop noprint pass
  break sort_timer_lst::tick
  run
  ```
  等待约 5 秒，gdb 停在 `tick()` 断点；输入 `print head`（或 `print *head`）能看到链表头结点的 `expire` 与 `next`（空连接时为 NULL）；`continue` 后每 5 秒停一次。`quit` 退出。
- [ ] **日志观察 "timer tick"**：保持 `./server` 运行超过 15 秒，确认 `timer tick` 稳定周期性输出（这就是本阶段用 printf 模拟的"心跳日志"，Stage 7 会换成真正的 LOG_INFO）。

---

## 参考答案对照

| 学习者的文件 | 仓库参考答案 | 差异说明 |
|---|---|---|
| `timer/lst_timer.h` | `timer/lst_timer.h` | 基本一致；唯一差异是省略了仓库头文件里的 `#include "../log/log.h"`（历史遗留、未使用，Stage 7 可补回） |
| `timer/lst_timer.cpp` | `timer/lst_timer.cpp` | 基本一致；`cb_func` 里 `http_conn::m_user_count--` 引用的都是各自的 http_conn.h，同构 |
| `server.cpp` 里的 `timer/adjust_timer/deal_timer/dealwithsignal` | `webserver.cpp` 的同名成员函数 + `webserver.h` | 学习者版是 main 里的平铺逻辑（自由函数 + 全局变量），仓库版是 `WebServer` 类的成员函数与成员变量。`WebServer::eventListen` 对应步骤 3 的 socket/epoll/socketpair/signal 设置，`WebServer::eventLoop` 对应事件循环，`WebServer::dealwithread/write` 对应读写续命逻辑 |
| `server.cpp` 里的 `Utils utils` / `users_timer` / `users` | `webserver.h` 的 `Utils utils`、`client_data *users_timer`、`http_conn *users` | 学习者版为全局变量，仓库版为成员 |

> 对照时重点看三点：① `adjust_timer` 为什么只需后移；② `tick` 为什么可以提前 break；③ 信号如何经 `socketpair` 变成 epoll 事件。

---

## 常见问题

1. **链接错误 `undefined reference to 'Utils::u_pipefd'` / `'Utils::u_epollfd'`**：static 成员在类里只是声明，忘了在 `lst_timer.cpp` 里写定义（`int *Utils::u_pipefd = 0;`）。补上即可。
2. **`http_conn::m_user_count` 未定义或不可访问**：确认 `m_user_count` 是 `public` 的 static 成员，且在 `http_conn.cpp` 里有一行 `int http_conn::m_user_count = 0;` 的定义；否则 `cb_func` 里的 `--` 会链接失败。
3. **`nc` 连接后 15 秒就是不断开**：按链路排查——`alarm(TIMESLOT)` 是否调用？`SIGALRM` 是否 `addsig` 注册？`pipefd[0]` 是否 `addfd` 进 epoll？`dealwithsignal` 是否把 SIGALRM 映射成 `timeout`？`if(timeout)` 是否调了 `timer_handler()`？`tick()` 里的 `expire` 是否用**绝对时间戳**（`cur + 15`）而不是相对值（15）？
4. **活跃连接也被 15 秒踢掉**：八成是读/写分支里忘了 `adjust_timer(t)`。没有续命，连接再活跃也会在首次超时点被 `tick` 关掉。
5. **服务器跑着跑着莫名被杀**：检查是否漏了 `utils.addsig(SIGPIPE, SIG_IGN);`。漏掉后，客户端粗暴断开时服务器 `write` 触发 SIGPIPE 直接终止进程。
6. **gdb 调试被 SIGALRM 反复打断**：SIGALRM 每 5 秒来一次，默认会让 gdb 停下。在 gdb 里先执行 `handle SIGALRM nostop noprint pass` 再 `run`（见验收清单）。
7. **理论上：`alarm` 与静态指针赋值的竞态**：仓库把 `alarm(TIMESLOT)` 写在 `Utils::u_pipefd = m_pipefd` **之前**，若 5 秒内没执行到赋值、恰好又触发 SIGALRM，`sig_handler` 会解引用 NULL 指针崩溃。5 秒足够跑完两行赋值，实际不会发生；本教程已把赋值提前，更稳妥。
8. **内存占用大 / 启动即分配几百 MB**：`new http_conn[MAX_FD]`（65536 个对象）会一次性申请约 200+MB。机器内存紧张时可把 `MAX_FD` 临时改小（如 4096）练习，但最终验收建议保持与仓库一致。

---

## 思考题

1. 为什么信号处理函数只向管道 `send` 一个字节，而不是直接在信号处理函数里调用 `tick()` 关闭连接？如果强行在信号处理函数里遍历链表 + `printf`，会发生什么问题？
2. `adjust_timer` 为什么只需要把定时器**向后**移，从不需要向前移？（提示：`expire` 只会被改成 `cur + 3*TIMESLOT`，它相对其它结点是变大还是变小？）
3. SIGALRM 到来时，正在阻塞的 `epoll_wait` 会返回什么？为什么 `eventLoop` 里要写 `if (number < 0 && errno != EINTR)` 而不是直接 `break`？（提示：与 `SA_RESTART` 有关）
4. `tick()` 里为什么 `cur < tmp->expire` 就可以 `break`？如果链表不是升序的，这个优化还成立吗？
5. 如果忘了在 `timer_handler()` 里重新 `alarm(m_TIMESLOT)`，程序行为会变成什么样？
6. `pipefd[0]` 为什么用 LT（默认）模式注册而不是 ET？如果一次信号周期内连续收到多个信号字节，ET 模式会漏处理吗？

---

## 下一步

下一阶段 [Stage 7：日志系统](stage-07-log.md) 会把本阶段那些 `printf`（`timer tick`、`close fd`、`adjust timer once` 等）替换成真正的**同步/异步日志**，并用**生产者-消费者阻塞队列**把磁盘 IO 从请求线程里剥离出去。

返回主索引：[README.md](README.md)
