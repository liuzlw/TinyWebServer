# Stage 9：整合收官 + 压测

这是最后一段：把前面 8 个阶段的所有模块装配成一台与仓库源码逐文件对应的完整服务器。上一阶段结束时，你的逻辑还平铺在 `server.cpp` 的 `main` 里；本阶段把它重组成 `WebServer` 类 + `Config` 命令行解析 + `main.cpp` 装配入口，补齐 Reactor 并发模型和 LT/ET 四组合，最后用 WebBench 压测。

学完本阶段，`my_tiny_webserver/` 与仓库源码一一对应：

```text
my_tiny_webserver/                 仓库对应文件
├── webserver.h                    webserver.h
├── webserver.cpp                  webserver.cpp
├── config.h                       config.h
├── config.cpp                     config.cpp
├── main.cpp                       main.cpp
├── makefile                       makefile
├── build.sh                       build.sh
├── lock/locker.h                  lock/locker.h
├── threadpool/threadpool.h        threadpool/threadpool.h
├── http/http_conn.h、.cpp         http/http_conn.h、.cpp
├── timer/lst_timer.h、.cpp        timer/lst_timer.h、.cpp
├── log/block_queue.h、log.h、.cpp log/...
├── CGImysql/sql_connection_pool.* CGImysql/sql_connection_pool.*
└── root/                          root/（可直接从仓库拷贝）
```

## 前置要求

- 已完成 [Stage 8](stage-08-mysql.md)，`my_tiny_webserver/` 能完成注册登录全流程。
- 熟悉本项目的各模块：线程池（Stage 3）、epoll ET/LT（Stage 4）、HTTP 状态机（Stage 5）、定时器（Stage 6）、日志（Stage 7）、连接池（Stage 8）。
- 已安装 WebBench 依赖（见"WebBench 压测"小节）。

## 理论学习

### 完整架构图

```text
                          ┌─────────────────────────────────────────────┐
                          │                  WebServer                   │
                          │                                             │
 浏览器 ──TCP──> listenfd ─┤  epoll_wait(epollfd)  ──事件分发──┐         │
                          │        │                          │         │
                          │   ┌────┴─────┬────────┬───────────┤         │
                          │   ▼          ▼        ▼           ▼         │
                          │ listenfd   pipefd   connfd      connfd      │
                          │ (新连接)   (信号)   (EPOLLIN)   (EPOLLOUT)   │
                          │   │          │        │           │         │
                          │   ▼          ▼        ▼           ▼         │
                          │ accept    dealwith  dealwith    dealwith    │
                          │            signal   read        write       │
                          │   │                   │           │         │
                          │   ▼                   └────┬──────┘         │
                          │ timer/initmysql    线程池 threadpool        │
                          │   │                     │                   │
                          └───┼─────────────────────┼───────────────────┘
                              │                     │
                    ┌─────────┼─────────┐   ┌───────┴────────┐
                    ▼         ▼         ▼   ▼                ▼
                 http_conn  定时器     日志  连接池          MySQL
                (状态机) (lst_timer) (log) (connection_pool)
```

一句话串起来：浏览器连上 `listenfd` → `epoll_wait` 拿到事件 → 按 fd 类型分发（新连接 / 信号 / 读 / 写）→ 读写的活交给线程池 → `http_conn` 状态机解析请求 → 需要时从连接池取连接访问 MySQL → 定时器负责把不活跃连接踢下线 → 日志记录全过程。

### Reactor vs Proactor：本项目真实代码里的差异

两种模型的区别是"**谁负责读数据**"：

- **Proactor（-a 0）**：主线程（事件循环线程）亲自把数据读进缓冲区，再把"已读好的请求"交给线程池处理。工作线程只做解析 + 生成响应。
- **Reactor（-a 1）**：主线程只**分发事件**，真正读/写 socket 的活由工作线程干。

对照代码看差异最清楚。先说线程池 `run()`（`threadpool/threadpool.h`）：

```cpp
if (1 == m_actor_model)            // Reactor
{
    if (0 == request->m_state)     // 读事件：工作线程自己读
    {
        if (request->read_once())
        {
            request->improv = 1;
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
        else { request->improv = 1; request->timer_flag = 1; }
    }
    else                           // 写事件：工作线程自己写
    {
        if (request->write()) request->improv = 1;
        else { request->improv = 1; request->timer_flag = 1; }
    }
}
else                               // Proactor
{
    connectionRAII mysqlcon(&request->mysql, m_connPool);
    request->process();            // 数据主线程已读好，这里只管处理
}
```

再看主线程侧（`webserver.cpp` 的 `dealwithread`）：

```cpp
// Reactor：主线程只投递，然后自旋等结果
if (1 == m_actormodel)
{
    if (timer) adjust_timer(timer);
    m_pool->append(users + sockfd, 0);   // 0 = 读事件
    while (true)                          // 忙等工作线程把 improv 置 1
    {
        if (1 == users[sockfd].improv)
        {
            if (1 == users[sockfd].timer_flag) { deal_timer(timer, sockfd); users[sockfd].timer_flag = 0; }
            users[sockfd].improv = 0;
            break;
        }
    }
}
// Proactor：主线程自己读
else
{
    if (users[sockfd].read_once())
    {
        m_pool->append_p(users + sockfd);
        if (timer) adjust_timer(timer);
    }
    else deal_timer(timer, sockfd);
}
```

**主线程为什么 `while (users[sockfd].improv != 1)` 自旋等待？** 因为 Reactor 模式下，连接 fd 挂了 `EPOLLONESHOT`，同一时刻只允许一个线程碰这个连接。主线程把"读"这个任务交给工作线程后，必须**等工作线程读完并处理完**，才能把这个连接重新挂回 epoll（否则可能两个线程同时操作同一个 socket）。工作线程干完会 `request->improv = 1`，主线程靠自旋（忙等）等到这个标志。

**为什么这里用忙等是可以接受的？** 因为等待的是"读一个 socket + 处理一个 HTTP 请求"，这个时间窗口很短（通常几十微秒到几毫秒），而且 Reactor 模式下主线程本来就没别的更急的事可做——它必须等这个连接处理完才能继续投递下一个事件。代价是这一小段 CPU 空转。更好的做法是条件变量 / 回调 / 每个连接一把锁，本项目为了教学清晰选择了最直白的标志位 + 自旋。这是思考题里常被问到的点。

### LT/ET 四组合

`-m` 参数同时决定 **listenfd** 和 **connfd** 的触发模式：

| `-m` | listenfd | connfd | 说明 |
|---|---|---|---|
| 0 | LT | LT | 默认，最稳 |
| 1 | LT | ET | connfd 用 ET，`read_once` 需循环读到 EAGAIN |
| 2 | ET | LT | listenfd 用 ET，`accept` 需循环到 EAGAIN |
| 3 | ET | ET | 两边都 ET，最高效也最易出错 |

对应 `webserver.cpp` 的 `trig_mode()`：

```cpp
if (0 == m_TRIGMode)      { m_LISTENTrigmode = 0; m_CONNTrigmode = 0; }  // LT + LT
else if (1 == m_TRIGMode) { m_LISTENTrigmode = 0; m_CONNTrigmode = 1; }  // LT + ET
else if (2 == m_TRIGMode) { m_LISTENTrigmode = 1; m_CONNTrigmode = 0; }  // ET + LT
else if (3 == m_TRIGMode) { m_LISTENTrigmode = 1; m_CONNTrigmode = 1; }  // ET + ET
```

- **listenfd 用 ET**：`dealclientdata()` 里 `accept` 要写成 `while(1)` 循环，直到返回 -1（EAGAIN）才停；LT 只 `accept` 一次。
- **connfd 用 ET**：`read_once()` 里 `recv` 要 `while(1)` 循环读到 `EAGAIN`；LT 只 `recv` 一次。

### SO_LINGER 优雅关闭

`close()` 一个还有未发完数据的 socket 时，内核默认是"立即返回，剩余数据尽量后台发完"（可能丢失）。`SO_LINGER` 让行为可控：

| 设置 | 语义 |
|---|---|
| `linger{0, 1}`（本项目 `-o 0`） | `close` 立即返回，不等待；剩余数据丢弃（`l_onoff=0` 时 `l_linger` 被忽略） |
| `linger{1, 1}`（本项目 `-o 1`） | 优雅关闭：`close` 阻塞 1 秒，尝试把剩余数据发完，超时则丢弃 |

对应 `eventListen()` 里对 `m_listenfd` 的 `setsockopt`。注意本项目只对 listenfd 设置（演示用），真正影响连接优雅关闭的是 connfd；更严谨的做法是给 connfd 也设置。

### 信号处理闭环回顾

定时器靠 `SIGALRM` 驱动，但信号处理函数里不能做重活（要可重入），于是本项目用"信号 → 管道 → epoll"把它变成普通 IO 事件：

```text
alarm(5) ──5秒后──> SIGALRM ──> sig_handler() ──send---> pipefd[1]
                                                          │
epoll_wait 返回 <──────────────────pipefd[0] 可读 ────────┘
      │
      ▼
dealwithsignal(): timeout = true
      │
      ▼
eventLoop 末尾: if (timeout) { utils.timer_handler(); }  // tick 链表 + 重新 alarm(5)
```

`sig_handler` 只做一件事：把信号值 `send` 进管道。主循环把管道另一头注册进 epoll，收到就置 `timeout`，然后调用 `timer_handler()` 去 `tick()` 定时器链表并重新 `alarm(TIMESLOT)`，形成闭环。

## 本阶段 C++ 知识点

### 1. 多文件项目组织（头文件 / 实现分离 + 依赖图）

本项目各文件的 include 依赖树（箭头 = "被谁 include"）：

```text
webserver.h ──> threadpool/threadpool.h ──> CGImysql/sql_connection_pool.h ──> lock/locker.h
    │                 │                          └──> log/log.h ──> log/block_queue.h ──> lock/locker.h
    │                 └──> lock/locker.h
    └──> http/http_conn.h ──> lock/locker.h
             ├──> CGImysql/sql_connection_pool.h
             ├──> timer/lst_timer.h ──> log/log.h
             └──> log/log.h

config.h ──> webserver.h
main.cpp ──> config.h
```

**为什么 makefile 里只列 `.cpp`、不列头文件？** 因为头文件是通过 `#include` 文本展开进 `.cpp` 的，编译 `.cpp` 时编译器自动顺着 include 找到它们。头文件只负责"声明"，`.cpp` 负责"定义"；把声明放头文件让多个 `.cpp` 共享，把定义放在唯一一个 `.cpp` 里避免重复符号。

**哪些 `.cpp` 要列进 makefile？** 所有"定义了函数/变量"的实现文件：`main.cpp`、`webserver.cpp`、`config.cpp`、`http/http_conn.cpp`、`timer/lst_timer.cpp`、`log/log.cpp`、`CGImysql/sql_connection_pool.cpp`。纯头文件实现（`locker.h`、`threadpool.h`、`block_queue.h` 全是模板或内联类）不用列。

### 2. 类组合（WebServer 持有多个模块）

`WebServer` 类用"组合"持有各模块的指针/对象，而不是继承：

```cpp
http_conn *users;                     // 连接数组
connection_pool *m_connPool;          // 连接池（单例指针）
threadpool<http_conn> *m_pool;        // 线程池
client_data *users_timer;             // 定时器用户数据数组
Utils utils;                          // 工具类（定时器链表 + fd/信号操作）
```

这就是"把一个类对象/指针作为另一个类的成员"。`WebServer` 是"总装车间"，它不自己实现 HTTP 解析或连接池，而是把它们**组合**进来调度。

### 3. static 成员跨文件定义规则

类的 `static` 成员**在类内只是声明，必须在某个 `.cpp` 里定义一次**（分配真正的存储）：

```cpp
// http/http_conn.h（声明）
static int m_epollfd;
static int m_user_count;

// http/http_conn.cpp（定义，注意：不能再写 static）
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
```

```cpp
// timer/lst_timer.h（声明）
static int *u_pipefd;
static int u_epollfd;

// timer/lst_timer.cpp（定义）
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;
```

规则：定义只能出现**一次**，否则链接报 `multiple definition`；定义处不带 `static` 关键字（`static` 在声明处表示"类共享一份"，定义处要用 `类名::成员` 限定）。

### 4. getopt 解析命令行

`config.cpp` 用 POSIX 的 `getopt` 解析参数：

```cpp
const char *str = "p:l:m:o:s:t:c:a:";
while ((opt = getopt(argc, argv, str)) != -1)
{
    switch (opt)
    {
    case 'p': PORT = atoi(optarg); break;       // 端口
    case 'l': LOGWrite = atoi(optarg); break;   // 日志方式
    case 'm': TRIGMode = atoi(optarg); break;   // 触发组合
    case 'o': OPT_LINGER = atoi(optarg); break; // 优雅关闭
    case 's': sql_num = atoi(optarg); break;    // 连接池数量
    case 't': thread_num = atoi(optarg); break; // 线程数
    case 'c': close_log = atoi(optarg); break;  // 关闭日志
    case 'a': actor_model = atoi(optarg); break;// 并发模型
    default: break;
    }
}
```

- 格式串 `"p:l:m:o:s:t:c:a:"`：每个字母是一个选项，后面的 `:` 表示**该选项要带参数**。所以 `-p 9007` 中 `9007` 会放进全局变量 `optarg`。
- `atoi(optarg)` 把字符串参数转成 `int`（`atoi` = ASCII to int，C 标准库函数）。
- 需要 `#include <unistd.h>`（`getopt`）和 `#include <stdlib.h>`（`atoi`）。

### 5. 构造 / 析构顺序

`WebServer` 构造函数里 `new` 两个大数组，析构函数里对称清理：

```cpp
WebServer::WebServer()
{
    users = new http_conn[MAX_FD];         // 连接数组
    // ... 计算站点根目录 m_root ...
    users_timer = new client_data[MAX_FD]; // 定时器数组
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}
```

规律：`new` 的对应 `delete`、`new[]` 的对应 `delete[]`；构造里分配的资源，析构里逆序释放。`m_root` 是 `malloc` 出来的（见下一条），析构里没 `free`（这是仓库留的小瑕疵，可作思考题）。

### 6. malloc/free 与 new/delete 对比

`m_root` 用的是 C 的 `malloc`：

```cpp
char server_path[200];
getcwd(server_path, 200);
char root[6] = "/root";
m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
strcpy(m_root, server_path);
strcat(m_root, root);
```

| | `malloc/free` | `new/delete` |
|---|---|---|
| 来源 | C 库 `<stdlib.h>` | C++ 关键字 |
| 是否需要类型转换 | 是，`(char*)` | 否 |
| 是否调用构造函数 | 否 | 是（`new` 调构造，`delete` 调析构） |
| 数组 | `malloc` + 手动算大小 | `new T[n]` / `delete[]` |

`getcwd` 拿到当前工作目录，再拼上 `"/root"` 得到站点根目录的绝对路径——这就是为什么必须从 `my_tiny_webserver/` 目录启动服务器（`root` 相对它）。

## 动手实现

### 第 1 步：`webserver.h`

把上一阶段 `server.cpp` 里的所有全局变量、自由函数，收进 `WebServer` 类。**与仓库一致**：

```cpp
#ifndef WEBSERVER_H
#define WEBSERVER_H

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

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int TIMESLOT = 5;             // 最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port, string user, string passWord, string databaseName,
              int log_write, int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void eventLoop();
    void timer(int connfd, struct sockaddr_in client_address);
    void adjust_timer(util_timer *timer);
    void deal_timer(util_timer *timer, int sockfd);
    bool dealclientdata();
    bool dealwithsignal(bool &timeout, bool &stop_server);
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

public:
    // 基础
    int m_port;
    char *m_root;
    int m_log_write;
    int m_close_log;
    int m_actormodel;

    int m_pipefd[2];
    int m_epollfd;
    http_conn *users;

    // 数据库相关
    connection_pool *m_connPool;
    string m_user;         // 数据库用户名
    string m_passWord;     // 数据库密码
    string m_databaseName; // 数据库名
    int m_sql_num;

    // 线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    // epoll_event 相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;

    // 定时器相关
    client_data *users_timer;
    Utils utils;
};
#endif
```

### 第 2 步：`webserver.cpp`

这是"总装"文件，把 `server.cpp` 里的平铺逻辑搬进类方法，并补上 Reactor 分支。**与仓库一致**，逐段讲解写在代码后：

```cpp
#include "webserver.h"

WebServer::WebServer()
{
    // http_conn 类对象
    users = new http_conn[MAX_FD];

    // root 文件夹路径：当前工作目录 + /root
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志：m_log_write 为 1 时异步（阻塞队列长度 800），否则同步
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表（把 user 表读进内存 map）
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::eventListen()
{
    // 网络编程基础步骤
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // epoll 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    // 工具类：信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化 client_data 数据；创建定时器，设置回调函数和超时时间，绑定用户数据，加入链表
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟 3 个单位，并调整链表位置
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        // LT：accept 一次
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }
    else
    {
        // ET：循环 accept 直到 EAGAIN
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
                timeout = true;
                break;
            case SIGTERM:
                stop_server = true;
                break;
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 监测到读事件，把任务丢给线程池
        m_pool->append(users + sockfd, 0);

        // 忙等：等工作线程把 improv 置 1
        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor：主线程自己读
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            m_pool->append_p(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
```

全函数讲解：

- **构造函数**：`new http_conn[MAX_FD]` 和 `new client_data[MAX_FD]` 两个大数组；`getcwd` + `malloc` 拼出 `m_root` 绝对路径。这就是上一阶段"记得补两行 `new`"的正式归宿。
- **析构函数**：关 fd、`delete[]` 两个数组、`delete m_pool`。`m_root` 没 `free`（瑕疵，见思考题）。
- **`init`**：纯粹把构造参数存进成员，不做事。
- **`trig_mode`**：按 `-m` 拆出 listenfd/connfd 各自模式。
- **`log_write`**：按 `-l` 决定同步/异步；`-c 1` 则完全跳过。
- **`sql_pool`**：取单例、`init` 建连接、`users->initmysql_result` 加载用户表。
- **`thread_pool`**：`new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num)`。
- **`eventListen`**：socket → SO_LINGER → bind → listen → epoll → 注册 listenfd/pipefd → 装信号 → `alarm` → 把 pipefd/epollfd 存进 `Utils` 静态成员。
- **`timer` / `adjust_timer` / `deal_timer`**：定时器绑定连接、延迟、删除。
- **`dealclientdata`**：LT 单次 `accept`，ET 循环 `accept`；满了回 `Internal server busy`。
- **`dealwithsignal`**：从管道读信号，置 `timeout`/`stop_server`。
- **`dealwithread` / `dealwithwrite`**：Reactor/Proactor 分支（见理论学习）。
- **`eventLoop`**：`epoll_wait` 分发四类事件，末尾处理 `timeout` 触发定时器 `tick`。

### 第 3 步：`config.h` + `config.cpp` + `main.cpp`

`config.h`（**与仓库一致**）：

```cpp
#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"

using namespace std;

class Config
{
public:
    Config();
    ~Config(){};

    void parse_arg(int argc, char *argv[]);

    // 端口号
    int PORT;
    // 日志写入方式
    int LOGWrite;
    // 触发组合模式
    int TRIGMode;
    // listenfd 触发模式
    int LISTENTrigmode;
    // connfd 触发模式
    int CONNTrigmode;
    // 优雅关闭链接
    int OPT_LINGER;
    // 数据库连接池数量
    int sql_num;
    // 线程池内的线程数量
    int thread_num;
    // 是否关闭日志
    int close_log;
    // 并发模型选择
    int actor_model;
};

#endif
```

`config.cpp`（**与仓库一致**）：

```cpp
#include "config.h"

Config::Config()
{
    // 端口号，默认 9006
    PORT = 9006;
    // 日志写入方式，默认同步
    LOGWrite = 0;
    // 触发组合模式，默认 listenfd LT + connfd LT
    TRIGMode = 0;
    // listenfd 触发模式，默认 LT
    LISTENTrigmode = 0;
    // connfd 触发模式，默认 LT
    CONNTrigmode = 0;
    // 优雅关闭链接，默认不使用
    OPT_LINGER = 0;
    // 数据库连接池数量，默认 8
    sql_num = 8;
    // 线程池内的线程数量，默认 8
    thread_num = 8;
    // 关闭日志，默认不关闭
    close_log = 0;
    // 并发模型，默认 proactor
    actor_model = 0;
}

void Config::parse_arg(int argc, char *argv[])
{
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p': PORT = atoi(optarg); break;
        case 'l': LOGWrite = atoi(optarg); break;
        case 'm': TRIGMode = atoi(optarg); break;
        case 'o': OPT_LINGER = atoi(optarg); break;
        case 's': sql_num = atoi(optarg); break;
        case 't': thread_num = atoi(optarg); break;
        case 'c': close_log = atoi(optarg); break;
        case 'a': actor_model = atoi(optarg); break;
        default: break;
        }
    }
}
```

注意：`Config` 里的 `LISTENTrigmode` / `CONNTrigmode` 只是默认值占位，实际拆分逻辑在 `WebServer::trig_mode()` 里根据 `TRIGMode` 重新计算（`config.h` 里这两个成员其实没被用上，这也是个可留意的细节）。

`main.cpp`（**与仓库一致**，30 行逐行讲解）：

```cpp
#include "config.h"

int main(int argc, char *argv[])
{
    // 需要修改的数据库信息：登录名、密码、库名
    string user = "root";
    string passwd = "root";
    string databasename = "qgydb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode, config.sql_num, config.thread_num,
                config.close_log, config.actor_model);

    // 日志
    server.log_write();

    // 数据库
    server.sql_pool();

    // 线程池
    server.thread_pool();

    // 触发模式
    server.trig_mode();

    // 监听
    server.eventListen();

    // 运行
    server.eventLoop();

    return 0;
}
```

逐行讲解：

- 第 6~8 行：数据库账号密码库名，硬编码（改这里即可换库）。
- 第 11~12 行：构造 `Config` 并解析命令行，覆盖默认值。
- 第 14 行：构造 `WebServer`（此时 `new` 两个数组、算 `m_root`）。
- 第 17~19 行：`init` 把 11 个参数灌进成员。
- 第 23 行：`log_write` 初始化日志。
- 第 26 行：`sql_pool` 初始化连接池 + 加载用户表。
- 第 29 行：`thread_pool` 建线程池。
- 第 32 行：`trig_mode` 拆 LT/ET 组合。
- 第 35 行：`eventListen` 建 socket/epoll/信号。
- 第 38 行：`eventLoop` 进入主循环，永不返回（直到 SIGTERM）。

这就是仓库 README 里说的"7 步装配"：init → log_write → sql_pool → thread_pool → trig_mode → eventListen → eventLoop。

### 第 4 步：`makefile` + `build.sh`

`makefile`（**与仓库一致**）：

```makefile
CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp  ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp  webserver.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm  -r server
```

逐行讲：

- `CXX ?= g++`：定义编译器变量；`?=` 表示"如果环境没定义 CXX 就用 g++"。
- `DEBUG ?= 1`：默认调试构建。
- `ifeq ($(DEBUG), 1) ... else ... endif`：make 的条件分支，DEBUG=1 加 `-g`（调试符号），否则加 `-O2`（优化）。`$(DEBUG)` 是引用变量，`ifeq` 比较两个参数。
- `server: <依赖列表>`：目标 `server` 依赖 7 个 `.cpp`；任一 `.cpp` 变了就重新链接。
- 下一行（**Tab 开头**）是配方（recipe）：`$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient`。`$^` 展开成**所有依赖**，`$(CXXFLAGS)` 展开成编译选项；`-o server` 指定输出名。
- `clean:` 目标：`rm -r server` 删掉产物。
- 关键：目标文件里**没有 `http_conn.h` 等头文件**，因为它们被 include 进 `.cpp`（见知识点 1）。

`build.sh`（**与仓库一致**）：

```bash
#!/bin/bash

make server
```

一行说明：`build.sh` 只是 `make server` 的包装，方便 `sh ./build.sh` 一条命令编译。首次使用可能要 `chmod +x build.sh`，或直接 `sh ./build.sh`。

### 第 5 步：`root/` 资源

`root/` 是纯静态资源，**直接从仓库拷贝**即可（这些 HTML/图片/视频属于"内容"而非"你要手写的代码"，手写反而容易和 `do_request` 的跳转逻辑对不上）：

```bash
cp -r ~/projects/TinyWebServer/root ~/projects/my_tiny_webserver/
```

页面跳转关系（对应 `do_request` 的分支）：

| 页面 | 按钮/表单 | 提交目标 | `do_request` 分支 | 结果 |
|---|---|---|---|---|
| judge.html | 新用户 | `action="0"` | `*(p+1)=='0'` | 跳 register.html |
| judge.html | 已有账号 | `action="1"` | `*(p+1)=='1'` | 跳 log.html |
| register.html | 注册表单 | `action="3CGISQL.cgi"` | `*(p+1)=='3'` | INSERT → log.html / registerError.html |
| log.html | 登录表单 | `action="2CGISQL.cgi"` | `*(p+1)=='2'` | 比对 → welcome.html / logError.html |
| welcome.html | xxx.jpg / xxx.avi / 关注我 | `action="5"/"6"/"7"` | `*(p+1)=='5'/'6'/'7'` | 跳 picture/video/fans.html |

## 编译与运行

```bash
cd ~/projects/my_tiny_webserver
make
# 或
sh ./build.sh
```

预期输出：无报错，生成 `server`。

```bash
./server -p 9007 -l 1 -m 0 -o 1 -s 10 -t 10 -c 1 -a 1
```

参数含义：端口 9007、异步日志、LT+LT、优雅关闭、连接池 10 条、线程 10 条、关闭日志、Reactor 模型。

### 参数实验矩阵

至少跑这 4 组，记录观察点：

| 实验 | 命令（其余用默认） | 观察点 |
|---|---|---|
| 并发模型 | `-a 0` vs `-a 1` | Reactor（-a 1）下主线程忙等；用 `curl -v` 观察响应是否正常；日志里 `deal with the client` 出现时机 |
| 优雅关闭 | `-o 0` vs `-o 1` | `-o 1` 时 `close` 多等 1 秒尝试发完剩余数据 |
| 日志方式 | `-l 0` vs `-l 1` | `-l 1` 异步时写线程分离，压测下 CPU 与吞吐变化 |
| 关闭日志 | `-c 1` | 无日志 IO，压测 QPS 明显提升（见 WebBench） |
| 触发组合 | `-m 0` vs `-m 3` | ET+ET（-m 3）需循环读/循环 accept，连接异常时表现不同 |

## 验收清单

终极大验收，每条都要看到预期输出：

- [ ] `make`（或 `sh ./build.sh`）编译成功，生成 `server`，无 error。
- [ ] `./server` 启动后进程不退出、无报错；`curl -v http://127.0.0.1:9006/` 返回 `HTTP/1.1 200 OK`。
- [ ] 浏览器访问 `http://127.0.0.1:9006/` 显示 judge.html（"欢迎访问"）。
- [ ] 注册 `alice` → 跳 log.html；登录 `alice` → 跳 welcome.html；`mysql -uroot -p -e "SELECT * FROM qgydb.user;"` 能看到 `alice` 一行。
- [ ] 图片页：welcome.html 点 "xxx.jpg" → picture.html 能显示图片（`root/xxx.jpg`）。
- [ ] 视频页：welcome.html 点 "xxx.avi" → video.html 能播放视频（`root/xxx.mp4`）。
- [ ] `curl -v http://127.0.0.1:9006/xxx.jpg -o /dev/null` 检查响应头含 `Content-Length` 与 `HTTP/1.1 200`。
- [ ] 空闲连接 15 秒断开：`curl http://127.0.0.1:9006/` 后保持不关（或 `nc` 连上不发数据），15 秒左右日志出现 `timer tick` 且连接被关闭（默认 `3 * TIMESLOT = 15s`）。
- [ ] 日志文件生成：`ls ServerLog/` 能看到按日期命名的日志文件；`-l 1` 异步模式下写线程仍在写。
- [ ] `-m 0` 与 `-m 3` 各启动一次并 curl 访问正常；`-a 0` 与 `-a 1` 各启动一次并完成一次注册登录。
- [ ] `-c 1` 启动后无日志输出（控制台只有 `close` 等 `printf`），压测 QPS 更高。
- [ ] WebBench 压测：编译并运行（见下），记录 QPS 与成功数；关闭日志（`-c 1`）后再压一次，QPS 应明显更高。

### WebBench 压测

WebBench 在仓库 `test_pressure/webbench-1.5` 里（也可自行拷贝）。编译：

```bash
cd ~/projects/TinyWebServer/test_pressure/webbench-1.5
make
```

> **坑 1（旧可执行文件）**：仓库里可能已带旧的 `webbench` 可执行文件，若 `./webbench` 报"找不到命令"或段错误，先 `rm webbench` 再重新 `make`（README 原话）。`webbench.o` 也是仓库自带的、已编译好的目标文件，只要没动过 `webbench.c`，`make` 只做链接，不会重新编译。
>
> **坑 2（新系统重编译报 rpc/types.h 缺失）**：WebBench 是 2004 年的老代码，`webbench.c` 里 `#include <rpc/types.h>`，而现代 glibc 已移除 RPC 头文件。如果你 `touch` 过 `webbench.c`/`socket.c` 或执行过 `make clean`，重新编译会报 `fatal error: rpc/types.h: No such file or directory`。解决办法：`sudo apt install libtirpc-dev`，然后把 `Makefile` 里 `CFLAGS?=...` 改为 `CFLAGS?= -Wall -Wno-unused-parameter -ggdb -W -O -I/usr/include/tirpc`，`LDFLAGS?=` 改为 `LDFLAGS?= -ltirpc`，再 `make`。若嫌麻烦，直接保留仓库自带的 `webbench.o` 不动它即可。

压测（先启动服务器，另开终端）：

```bash
./server -p 9006 -c 1          # 关日志跑，先拿到基准
# 另开终端
./webbench -c 1000 -t 10 http://127.0.0.1:9006/
```

预期输出形如：

```text
Benchmarking: GET http://127.0.0.1:9006/
1000 clients, running 10 sec.
Speed=xxxx pages/min, xxxxxx bytes/sec.
Requests: xxxx susceed, 0 failed.
```

再测开日志对比：

```bash
./server -p 9006                # 开日志
./webbench -c 1000 -t 10 http://127.0.0.1:9006/
```

观察：关闭日志后的 QPS 显著高于开日志（日志是主要 IO 瓶颈）。仓库 README 里的参考量级是**数万 QPS**（LT+LT 约 93251 QPS 等），但那是作者当年的机器 + 全部优化后的结果；**你的实际数值取决于机器**，上万并发下能稳定运行、失败数为 0 就是合格，不必追平参考值。

> 注意：WebBench 默认可能带代理设置，压测本机请确保没走代理（`-p` 或环境变量）。若 `failed` 数很高，先检查 `ulimit -n` 是否够大（`ulimit -n 65535`）。

## 参考答案对照

本阶段所有文件**与仓库逐文件一致**：

| 你的文件 | 仓库文件 | 一致性 |
|---|---|---|
| `webserver.h` | `webserver.h` | 一致 |
| `webserver.cpp` | `webserver.cpp` | 一致 |
| `config.h` | `config.h` | 一致 |
| `config.cpp` | `config.cpp` | 一致 |
| `main.cpp` | `main.cpp` | 一致 |
| `makefile` | `makefile` | 一致 |
| `build.sh` | `build.sh` | 一致 |
| `lock/locker.h` | `lock/locker.h` | 一致 |
| `threadpool/threadpool.h` | `threadpool/threadpool.h` | 一致 |
| `http/http_conn.h/.cpp` | `http/http_conn.h/.cpp` | 一致 |
| `timer/lst_timer.h/.cpp` | `timer/lst_timer.h/.cpp` | 一致 |
| `log/block_queue.h/log.h/log.cpp` | `log/*` | 一致 |
| `CGImysql/sql_connection_pool.*` | `CGImysql/sql_connection_pool.*` | 一致 |
| `root/` | `root/` | 直接拷贝 |

对照时重点看：`main.cpp` 的 7 步顺序、`eventLoop` 的分发分支、`dealwithread/write` 的 actor_model 分支、`trig_mode` 的四组合、`dealclientdata` 的 LT/ET accept 差异。

## 常见问题

1. **`make` 报 `missing separator`**：makefile 的 recipe 行必须以 **Tab** 开头，复制时被编辑器替换成空格就会报这个。把第 12、15 行行首的空格改回 Tab。

2. **`undefined reference to http_conn::m_epollfd` 或 `Utils::u_epollfd`**：静态成员只在头文件声明了，没在 `.cpp` 定义。确认 `http/http_conn.cpp` 里有 `int http_conn::m_user_count = 0; int http_conn::m_epollfd = -1;`，`timer/lst_timer.cpp` 里有 `int *Utils::u_pipefd = 0; int Utils::u_epollfd = 0;`。

3. **启动报 `Assertion 'm_listenfd >= 0' failed`**：端口被占用或权限不足。换端口 `-p 9007`，或 `sudo lsof -i:9006` 找到占用进程杀掉。

4. **浏览器 404 / 图片视频加载不出**：`m_root` 是"当前工作目录 + /root"，必须在 `my_tiny_webserver/` 下启动 `./server`。用绝对路径或从别的目录启动会导致找不到 `root/`。

5. **`-a 1`（Reactor）下页面卡住不返回**：Reactor 依赖 `improv` 标志位 + 忙等。检查 `threadpool.h` 的 `run()` 里 Reactor 分支是否正确置了 `request->improv = 1`；`http_conn::init()` 是否把 `improv` 初始化为 0；`append(T*, int)` 是否写了 `m_state`。三者缺一都会让主线程死循环自旋。

6. **`-m 3`（ET+ET）下并发连接出错/丢事件**：ET 必须"读到 EAGAIN 才停"。确认 `read_once()` 的 ET 分支是 `while(1)` 循环，`dealclientdata()` 的 ET 分支是 `while(1)` 循环 `accept`。只读一次就返回会漏数据。

7. **WebBench 报 `webbench: command not found` 或段错误**：删掉旧的 `webbench` 可执行文件重新 `make`（README 提示）。

8. **压测 `failed` 数很高**：先 `ulimit -n 65535` 提高文件描述符上限；确认没走代理；`-c 1000` 已经很大，机器弱就降到 `-c 500`。

9. **`connection_pool` 初始化失败退出**：连接池 `init` 里 `mysql_real_connect` 失败会 `exit(1)`。确认 MySQL 已启动、`main.cpp` 里的账号密码库名正确、`qgydb` 已建。

## 思考题（面试级）

1. ET 模式为什么必须循环读到 `EAGAIN`？如果只 `recv` 一次，会漏掉什么？（提示：ET 只在"状态变化"时通知一次，内核缓冲区里剩余数据不会再触发新事件。）

2. 为什么 connfd 要挂 `EPOLLONESHOT`？去掉它会有什么并发风险？（提示：多个线程同时操作同一个 socket。）

3. Reactor 分支的 `improv` 忙等如何改进？给出至少两种方案并分析代价（条件变量 / 每连接一把锁 / 主线程不做等待改用回调）。

4. 定时器为什么要和连接 fd 绑定？（提示：谁活跃谁续命，谁沉默谁被踢。）

5. `writev` 为什么适合 HTTP 响应？（提示：响应 = 响应头 + 文件内容，两块内存一次发。）

6. `m_user_count` 的并发安全性：它是 `static int`，在多线程下 `++`/`--` 有没有数据竞争？本项目为什么"看似能跑"？怎么改才严格线程安全？

7. `WebServer` 析构函数漏 `free(m_root)`，这个泄漏发生在什么生命周期（进程结束才析构，影响多大）？`new/delete` 与 `malloc/free` 混用的正确姿势是什么？

> 更多面试自测题见 [LEARNING_GUIDE.md](../LEARNING_GUIDE.md) 的"面试和自测问题"一节，答案都能在本教程对应阶段找到。

## 恭喜完成

至此，`my_tiny_webserver/` 已经是一台和仓库源码逐文件对应的完整 TinyWebServer：线程池 + 非阻塞 socket + epoll(ET/LT) + Reactor/Proactor + HTTP 状态机 + 升序链表定时器 + 同步/异步日志 + MySQL 连接池与注册登录，还能用 WebBench 压到上万并发。

接下来建议：

1. 把 [LEARNING_GUIDE.md](../LEARNING_GUIDE.md) 的面试题全部过一遍，检验自己是否"知其所以然"。
2. 用 [附录 A：make 与 CMake](appendix-a-make-cmake.md) 把本项目 CMake 化，吃透构建系统。
3. 用 [附录 B：gdb 调试](appendix-b-gdb.md) 全程调试本项目（断点跟踪 HTTP 状态机、多线程调度、core dump 定位段错误）。

你已经从零手写了这台服务器，这是一个非常扎实的 C++ 项目里程碑。🎉
