# Phase 8 —— 服务集成

## 目标

将 Phase 1-7 的所有模块串联成一个完整的 Web 服务器，支持：
- 命令行参数配置（端口、日志模式、触发模式等）
- 静态文件服务（HTML、图片、视频）
- 用户注册 / 登录（POST → MySQL）
- Reactor / Proactor 两种并发模型
- LT / ET 四种触发组合

**可见结果：** 浏览器访问 `http://ip:port/`，看到一个完整的 Web 服务——注册页面 → 注册 → 登录 → 欢迎页 → 浏览图片/视频。

---

## 前置知识

- Phase 1-7 所有模块
- Phase 0 的 cmake 基础

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | 最终项目 CMakeLists.txt 全局整合：include_directories、ind_library、所有源文件归并到一个 target |
| **strace** | 追踪完整请求链路中的系统调用序列 |

---

## 分步实现

### Step 1：`WebServer` 主类设计

```cpp
// webserver.h
class WebServer {
public:
    WebServer();
    ~WebServer();

    // 初始化：设置参数
    void init(int port, string user, string passwd, string dbname,
              int log_write, int opt_linger, int trigmode,
              int sql_num, int thread_num, int close_log, int actor_model);

    // 六大初始化步骤（对应 main 中的调用顺序）
    void log_write();       // 1. 日志
    void sql_pool();        // 2. 数据库连接池
    void thread_pool();     // 3. 线程池
    void trig_mode();       // 4. 触发模式
    void eventListen();     // 5. 监听：socket + epoll + 信号
    void eventLoop();       // 6. 主循环：epoll_wait → 事件分发

    // 事件处理
    void timer(int connfd, sockaddr_in client_addr);
    void adjust_timer(util_timer* timer);
    void deal_timer(util_timer* timer, int sockfd);
    bool dealclientdata();                          // 处理新连接
    bool dealwithsignal(bool& timeout, bool& stop); // 处理信号
    void dealwithread(int sockfd);                  // 处理读事件
    void dealwithwrite(int sockfd);                 // 处理写事件

    // 成员变量（略，见 web_conn_detail.h）
};
```

### Step 2：`Config` 命令行参数解析

```cpp
// config.h
class Config {
public:
    Config();
    void parse_arg(int argc, char* argv[]);

    int PORT;           // 默认 9006
    int LOGWrite;       // 0=同步, 1=异步
    int TRIGMode;       // 0=LT+LT, 1=LT+ET, 2=ET+LT, 3=ET+ET
    int OPT_LINGER;     // 0=否, 1=是
    int sql_num;        // 默认 8
    int thread_num;     // 默认 8
    int close_log;      // 0=开, 1=关
    int actor_model;    // 0=Proactor, 1=Reactor
};
```

```cpp
// config.cpp
void Config::parse_arg(int argc, char* argv[]) {
    int opt;
    const char* str = "p:l:m:o:s:t:c:a:";
    while ((opt = getopt(argc, argv, str)) != -1) {
        switch (opt) {
        case 'p': PORT       = atoi(optarg); break;
        case 'l': LOGWrite   = atoi(optarg); break;
        case 'm': TRIGMode   = atoi(optarg); break;
        case 'o': OPT_LINGER = atoi(optarg); break;
        case 's': sql_num    = atoi(optarg); break;
        case 't': thread_num = atoi(optarg); break;
        case 'c': close_log  = atoi(optarg); break;
        case 'a': actor_model= atoi(optarg); break;
        default:  break;
        }
    }
}
```

**用法：**

```bash
./server -p 9007 -l 1 -m 3 -s 10 -t 10 -c 0 -a 0
# 端口 9007、异步日志、ET+ET、10 连接、10 线程、开日志、Proactor
```

### Step 3：`main` 函数

```cpp
// main.cpp
#include "config.h"

int main(int argc, char* argv[]) {
    // --- 数据库配置（按你的实际环境修改）---
    string user         = "root";
    string passwd       = "your_password";
    string databasename = "qgydb";

    // --- 命令行解析 ---
    Config config;
    config.parse_arg(argc, argv);

    // --- 初始化服务器 ---
    WebServer server;
    server.init(config.PORT, user, passwd, databasename,
                config.LOGWrite, config.OPT_LINGER,
                config.TRIGMode, config.sql_num,
                config.thread_num, config.close_log,
                config.actor_model);

    // --- 六大启动步骤 ---
    server.log_write();     // 日志系统
    server.sql_pool();      // 数据库连接池
    server.thread_pool();   // 线程池
    server.trig_mode();     // LT/ET 配置
    server.eventListen();   // socket + epoll + 信号
    server.eventLoop();     // 主事件循环

    return 0;
}
```

### Step 4：`eventListen` —— 网络编程启动

```cpp
void WebServer::eventListen() {
    // 1. socket
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // 2. SO_LINGER
    if (0 == m_OPT_LINGER) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 3. bind + listen
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    listen(m_listenfd, 5);

    // 4. epoll
    utils.init(TIMESLOT);
    m_epollfd = epoll_create(5);
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    http_conn::m_epollfd = m_epollfd;

    // 5. 信号管道
    socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // 6. 注册信号
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 7. 启动定时器
    alarm(TIMESLOT);

    // 8. 设置全局静态变量
    Utils::u_pipefd  = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}
```

### Step 5：`eventLoop` —— 主事件循环

```cpp
void WebServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server) {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("epoll failure");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // --- 分支 1：新连接 ---
            if (sockfd == m_listenfd) {
                bool flag = dealclientdata();
                if (!flag) continue;
            }
            // --- 分支 2：连接异常 ---
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer* timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // --- 分支 3：信号 ---
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
                bool flag = dealwithsignal(timeout, stop_server);
                if (!flag) LOG_ERROR("signal handling error");
            }
            // --- 分支 4：读事件 ---
            else if (events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            }
            // --- 分支 5：写事件 ---
            else if (events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }

        // 定时器 tick
        if (timeout) {
            utils.timer_handler();
            timeout = false;
        }
    }
}
```

**事件分发的五个分支：**

| 条件 | 含义 | 处理函数 |
|------|------|---------|
| `fd == listenfd` | 新客户端连接 | `dealclientdata` |
| `EPOLLRDHUP \| EPOLLHUP \| EPOLLERR` | 对端关闭或错误 | `deal_timer`（清理） |
| `fd == pipefd && EPOLLIN` | 信号到达 | `dealwithsignal` |
| `EPOLLIN` | 数据可读 | `dealwithread` |
| `EPOLLOUT` | 可写（缓冲区空） | `dealwithwrite` |

### Step 6：读写分发（Proactor vs Reactor）

```cpp
void WebServer::dealwithread(int sockfd) {
    util_timer* timer = users_timer[sockfd].timer;

    if (1 == m_actormodel) {
        // --- Reactor：主线程只投递，工作线程自己去读 ---
        if (timer) adjust_timer(timer);
        m_pool->append(users + sockfd, 0);  // state=0 表示读任务

        // 自旋等待工作线程处理完成
        while (true) {
            if (1 == users[sockfd].improv) {
                if (1 == users[sockfd].timer_flag)
                    deal_timer(timer, sockfd);
                users[sockfd].improv = 0;
                break;
            }
        }
    } else {
        // --- Proactor：主线程先读，再投递给工作线程 ---
        if (users[sockfd].read_once()) {
            m_pool->append_p(users + sockfd);
            if (timer) adjust_timer(timer);
        } else {
            deal_timer(timer, sockfd);
        }
    }
}
```

### Step 7：数据库准备

```sql
-- 在 MySQL 中执行
CREATE DATABASE qgydb;
USE qgydb;

CREATE TABLE user (
    username CHAR(50) NULL,
    passwd   CHAR(50) NULL
) ENGINE=InnoDB;

-- 可选：插入一个测试用户
INSERT INTO user(username, passwd) VALUES('admin', '123456');
```

### Step 8：静态文件准备

在项目目录下放一个 `root` 文件夹，包含这些 HTML 页面（本项目自带）：

```
root/
├── judge.html          # 首页/导航
├── register.html       # 注册页
├── log.html            # 登录页
├── welcome.html        # 欢迎页
├── registerError.html  # 注册失败
├── logError.html       # 登录失败
├── picture.html        # 图片展示
├── video.html          # 视频展示
├── fans.html           # 粉丝页
├── favicon.ico
└── *.jpg, *.gif, *.mp4
```

### Step 9：最终 cmake 整合

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 整个项目的头文件都能找到彼此
include_directories(${CMAKE_SOURCE_DIR})

# 查找 MySQL 客户端库
find_library(MYSQL_LIB mysqlclient
    PATHS /usr/lib/x86_64-linux-gnu
          /usr/lib64/mysql
          /usr/local/mysql/lib)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    log/log.cpp
    timer/lst_timer.cpp
    CGImysql/sql_connection_pool.cpp
)

target_link_libraries(server
    pthread
    ${MYSQL_LIB}
)
```

### Step 10：编译运行

```bash
# 创建数据库和表（见 Step 7）
# 修改 main.cpp 中的数据库密码

cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 启动（使用默认参数）
./build/server

# 或自定义参数
./build/server -p 9007 -l 1 -m 3 -s 10 -t 10
```

浏览器打开 `http://<你的IP>:9006`，应该看到导航页。

---

## 完整请求流程追溯

以一次"注册"请求为例，完整跟踪代码路径：

```
1. 浏览器 → TCP SYN → listenfd
2. epoll_wait 返回 → 进入 dealclientdata
3. accept → connfd → timer(connfd): 创建定时器
4. addfd: connfd 注册到 epoll (EPOLLIN | EPOLLET | EPOLLONESHOT)

5. 浏览器 → POST /3CGI HTTP/1.1 + body
6. epoll_wait 返回 connfd EPOLLIN → dealwithread
7. read_once(): 循环 recv 读完整请求
8. Proactor: append_p(users+connfd) → 投递到线程池
9. 工作线程: read_once() → process()
10. process_read(): 状态机 → CHECK_STATE_CONTENT → parse_content → GET_REQUEST
11. do_request(): 解析 name/password → INSERT INTO user → m_url="/log.html"
12. process_write(): 构造 200 OK + log.html 内容
13. modfd(EPOLLOUT) → 下次 epoll_wait 返回 EPOLLOUT
14. dealwithwrite → write(): writev 发送响应头 + 文件 → unmap
15. keep-alive: modfd(EPOLLIN) → 等待下一个请求
```

---

## 验证方法

- [ ] `./build/server` 正常启动，无报错
- [ ] 浏览器访问 `http://ip:9006/` → 显示导航页
- [ ] 点击注册 → 输入用户名密码 → 注册成功跳转登录页
- [ ] 登录 → 输入密码 → 登录成功显示欢迎页
- [ ] 点击图片页 → 正确显示图片
- [ ] 用不同的 `-m` 参数测试 LT/ET 组合
- [ ] 用不同的 `-a` 参数测试 Reactor/Proactor

---

## 安全注意事项（进阶阅读）

本项目的定位是教学演示，以下是生产环境部署前必须关注的安全问题：

### SQL 注入

`do_request` 中直接将用户输入拼接到 SQL 语句：

```cpp
char sql_insert[200];
sprintf(sql_insert, "INSERT INTO user(username, passwd) VALUES('%s', '%s')", name, password);
mysql_query(mysql, sql_insert);
```

如果用户输入 `admin' OR '1'='1`，SQL 语义就被篡改了。**防护：** 使用 `mysql_real_escape_string` 转义特殊字符，或使用 MySQL 预处理语句（Prepared Statement）。

### 路径穿越

URL 中的 `..` 可能让攻击者访问服务器上任意文件：

```
GET /../../../etc/passwd HTTP/1.1
```

**防护：** 在 `do_request` 中拒绝包含 `..` 的 URL，或用 `realpath()` 规范化后检查前缀是否为 `doc_root`。

### 缓冲区溢出

多处使用 `strcpy`、`sprintf` 没有边界检查。**防护：** 全部替换为 `strncpy`、`snprintf` 并检查返回值。

### 其他常见问题

- **无 HTTPS：** 密码在网络上明文传输。生产环境应使用 TLS/SSL。
- **无速率限制：** 攻击者可暴力破解登录。应增加 IP 级别的失败计数和限流。
- **无输入长度限制：** 用户名/密码无最大长度限制，可导致服务资源耗尽。

---

## 踩坑记录

1. **`getcwd` 获取工作目录。** `m_root` 是 `getcwd() + "/root"`，确保你从项目根目录启动服务器（`./build/server`，不是 `cd build && ./server`）。

2. **MySQL 连接失败。** 检查：MySQL 是否运行、用户名密码是否正确、数据库 `qgydb` 是否存在、表 `user` 是否存在。

3. **端口被占用。** `Address already in use` → 等 60 秒（TIME_WAIT）或设置 `SO_REUSEADDR`。

4. **`server_path` 数组太小。** 路径太长会截断，确保 `char server_path[200]` 足够容纳完整路径。

5. **Proactor 模式下的 `improv` 自旋。** `dealwithread` 中的 `while (users[sockfd].improv != 1)` 是忙等（busy-wait），会浪费 CPU。这是简化实现，生产环境应该用条件变量或回调替代：

   **方案一（条件变量）：** 主线程在 `improv` 上 `cond.wait()`，工作线程处理完后 `cond.signal()`。

   **方案二（回调+事件驱动）：** 工作线程处理完后通过 `eventfd` 通知主线程，主线程在 epoll 中监听 eventfd，无需自旋。

   ```cpp
   // 方案一的伪代码示例
   // 主线程 dealwithread 中：
   m_mutex.lock();
   if (users[sockfd].improv != 1)
       m_cond.wait(m_mutex.get());  // 休眠，不占 CPU
   m_mutex.unlock();

   // 工作线程 run() 中处理完后：
   request->improv = 1;
   m_cond.signal();  // 唤醒主线程
   ```

---

## 阶段小结

你完成了一个功能完整的 C++ Web 服务器！它包含：
- HTTP 解析（状态机）
- 静态文件服务（mmap + writev）
- 用户注册/登录（MySQL + 连接池）
- 多种并发模型（Reactor / Proactor）
- 多种触发模式（LT / ET 组合）
- 定时器清理死连接
- 同步/异步日志

最后一阶段：**性能压测与工程实践**——用 WebBench 压测，对比各种模式的 QPS，分析瓶颈。
