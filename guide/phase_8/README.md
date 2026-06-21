# Phase 8 —— 服务集成：组装完整 Web 服务器

## 本阶段目标

将 Phase 1-7 的所有模块串联成一个完整的 Web 服务器。

**可见结果：** 浏览器访问 `http://服务器IP:9006/`，看到完整的 Web 服务：

1. 首页是判断页面（judge.html），可以选择注册或登录
2. 注册 → 用户名密码写入 MySQL → 跳转登录页
3. 登录 → 验证用户名密码 → 跳转欢迎页
4. 可以请求图片和视频文件（通过静态文件服务）

**验收标准：**

- [ ] 浏览器访问首页，显示 judge.html
- [ ] 注册新用户成功，数据库 user 表中有新记录
- [ ] 用刚注册的用户登录成功，跳转 welcome.html
- [ ] 访问 `/picture.html` 能看到图片
- [ ] 访问 `/video.html` 能播放视频
- [ ] 用不存在的用户登录，显示错误页面
- [ ] 所有参数可以通过命令行配置（端口、线程数、日志模式等）

---

## 理论与机制

### 1. 整体架构

```
                          ┌──────────────────────────┐
                          │      WebServer (主控)      │
                          │   main.cpp → webserver.cpp │
                          └──────────┬───────────────┘
                                     │
          ┌──────────────────────────┼──────────────────────────┐
          │                          │                          │
    ┌─────▼─────┐            ┌──────▼──────┐            ┌──────▼──────┐
    │  监听线程  │            │  epoll 循环   │            │   线程池     │
    │ (eventListen)│          │ (eventLoop)  │            │ (threadpool) │
    └───────────┘            └──────┬───────┘            └──────┬──────┘
                                    │                           │
                    ┌───────────────┼───────────────┐          │
                    │               │               │          │
              ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐    │
              │ 新连接     │  │ 可读事件  │  │ 可写事件  │    │
              │ accept    │  │ EPOLLIN   │  │ EPOLLOUT  │    │
              └─────┬─────┘  └─────┬─────┘  └─────┬─────┘    │
                    │              │               │          │
              ┌─────▼─────┐  ┌─────▼─────┐  ┌─────▼─────┐    │
              │ 创建定时器 │  │ read_once │  │  write()  │    │
              │ timer()   │  │ + process │  │ 写响应    │    │
              └───────────┘  └─────┬─────┘  └───────────┘    │
                                   │                          │
                            ┌──────▼──────┐            ┌──────▼──────┐
                            │ HTTP 状态机  │            │  工作线程    │
                            │ parse/proc  │◄───────────│  run()      │
                            └──────┬──────┘            └──────┬──────┘
                                   │                          │
                            ┌──────▼──────┐                   │
                            │ MySQL 连接池 │◄──────────────────┘
                            │ (RAII 获取) │
                            └─────────────┘
```

### 2. 请求处理全链路

一次完整的 HTTP 请求在系统中的生命周期：

```
1. 浏览器发起 TCP 连接
   ↓ TCP 三次握手 (内核完成)
2. listenfd 变为可读 → epoll_wait 返回
   ↓
3. dealclientdata() → accept() → 创建 connfd → 注册 epoll
   ↓ 同时创建定时器(3×TIMESLOT 秒超时)
4. 浏览器发送 HTTP 请求
   ↓
5. connfd 变为可读 → epoll_wait 返回
   ↓
6. dealwithread(sockfd)
   ↓ 
7. 根据 actor_model 选择路径:
   
   【Proactor 路径】(默认)
   7a. 主线程: read_once() 读全部数据
   7b. 主线程: m_pool->append_p() 投递到线程池
   7c. 工作线程: process() → process_read() → do_request() → process_write()
   7d. 工作线程: 完成后 modfd(EPOLLOUT)
   
   【Reactor 路径】
   7a. 主线程: m_pool->append(users+sockfd, 0) 投递到线程池
   7b. 工作线程: read_once() 读数据
   7c. 工作线程: process() 解析 + 生成响应
   7d. 主线程: 等待 improv 标志 → modfd(EPOLLOUT)

8. connfd 变为可写 → epoll_wait 返回
   ↓
9. dealwithwrite(sockfd) → write() → writev 发送响应头+文件
   ↓
10. 如果 keep-alive: modfd(EPOLLIN) 重新读 → 回到步骤 4
    如果 close: close_conn() → removefd → 连接关闭
```

### 3. Proactor vs Reactor：核心区别

这是本项目最常被问到的设计抉择：

| | Proactor (默认) | Reactor |
|---|---|---|
| **谁读数据** | 主线程读（IO 操作在主线程） | 工作线程读（IO 操作在工作线程） |
| **主线程职责** | IO 操作 + 事件分发 | 只做事件分发 |
| **工作线程职责** | 纯粹的业务逻辑处理 | IO 操作 + 业务逻辑 |
| **优点** | IO 集中，易优化；工作线程纯粹 | 主线程轻量，不会被 IO 阻塞 |
| **缺点** | 主线程负担重 | 工作线程会被 IO 阻塞 |
| **线程池入口** | `append_p()` — 数据已读好 | `append()` — 需要自己读 |

**主循环中的体现（`dealwithread`）：**

```cpp
// Proactor: 主线程读 + 投递
if (users[sockfd].read_once()) {
    m_pool->append_p(users + sockfd);  // 数据已读好
}

// Reactor: 只投递，工作线程自己读
m_pool->append(users + sockfd, 0);  // 0 = 读状态
```

---

## 实现指南

### Step 1：WebServer 主类

```cpp
class WebServer {
public:
    void init(...);           // 初始化所有参数
    void eventListen();       // socket/bind/listen/epoll 创建
    void eventLoop();         // 主事件循环（epoll_wait + 分发）
    void dealclientdata();    // 处理新连接（accept）
    void dealwithread(int);   // 处理可读事件
    void dealwithwrite(int);  // 处理可写事件
    void dealwithsignal();    // 处理信号（SIGALRM → tick）
    void timer(int, sockaddr_in); // 为新连接创建定时器

    // 模块初始化
    void log_write();         // 初始化日志
    void sql_pool();          // 初始化数据库连接池
    void thread_pool();       // 初始化线程池
    void trig_mode();         // 设置 LT/ET 模式
    
    // 核心成员
    int m_listenfd;           // 监听 socket
    int m_epollfd;            // epoll 实例
    int m_pipefd[2];          // 信号管道（socketpair）
    http_conn *users;         // 所有连接的数组（预分配 MAX_FD 个）
    client_data *users_timer; // 所有连接的定时器数据
    threadpool<http_conn> *m_pool; // 线程池
    connection_pool *m_connPool;   // 数据库连接池
    Utils utils;              // 工具类（定时器链表、fd 操作）
};
```

### Step 2：main.cpp 中的 6 步启动流程

```cpp
int main(int argc, char *argv[]) {
    // 1. 解析命令行参数
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    // 2. 初始化参数
    server.init(config.PORT, user, passwd, databasename,
                config.LOGWrite, config.OPT_LINGER, config.TRIGMode,
                config.sql_num, config.thread_num,
                config.close_log, config.actor_model);

    // 3. 启动日志系统（Phase 2）
    server.log_write();

    // 4. 初始化数据库连接池（Phase 3）
    server.sql_pool();

    // 5. 创建线程池（Phase 4）
    server.thread_pool();

    // 6. 设置触发模式（Phase 7）
    server.trig_mode();

    // 7. 创建监听 socket + epoll（Phase 7）
    server.eventListen();

    // 8. 进入主事件循环（Phase 7）
    server.eventLoop();
}
```

**⚠️ 注意初始化的顺序！**
- `log_write` 必须在最前面——后续的 `sql_pool` 和 `thread_pool` 初始化中可能会 LOG_ERROR
- `sql_pool` 必须在 `thread_pool` 之前——线程池构造时需要传入数据库连接池指针
- `eventListen` 必须在最后——它设置了信号处理，应该在所有初始化完成后

### Step 3：事件循环的 5 类事件

`eventLoop` 用 `epoll_wait` 等待事件，然后按 fd 类型分发：

```cpp
void WebServer::eventLoop() {
    while (!stop_server) {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        
        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            if (sockfd == m_listenfd) {
                // 事件1: 新连接 → accept
                dealclientdata();
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                // 事件2: 连接异常 → 清理定时器 + 关闭
                deal_timer(timer, sockfd);
            }
            else if (sockfd == m_pipefd[0] && (events[i].events & EPOLLIN)) {
                // 事件3: 信号（SIGALRM/SIGTERM）→ timer_handler / 退出
                dealwithsignal(timeout, stop_server);
            }
            else if (events[i].events & EPOLLIN) {
                // 事件4: 客户端数据到达 → 读 + 处理
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT) {
                // 事件5: 可以发送响应 → 写
                dealwithwrite(sockfd);
            }
        }
        
        // 定时器心跳
        if (timeout) {
            utils.timer_handler();  // tick() + alarm()
            timeout = false;
        }
    }
}
```

**事件优先级**（代码中的判断顺序）：
1. 新连接优先（listenfd 有新连接必须尽快 accept，否则 backlog 满了会丢连接）
2. 异常连接（尽快清理，释放资源）
3. 信号处理（定时器 tick 和终止信号需要及时响应）
4. 读事件
5. 写事件

### Step 4：命令行参数解析

```cpp
// config.cpp
void Config::parse_arg(int argc, char* argv[]) {
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:";
    while ((opt = getopt(argc, argv, str)) != -1) {
        switch (opt) {
            case 'p': PORT = atoi(optarg); break;       // 端口
            case 'l': LOGWrite = atoi(optarg); break;    // 日志模式
            case 'm': TRIGMode = atoi(optarg); break;    // LT/ET 组合
            case 'o': OPT_LINGER = atoi(optarg); break;  // 优雅关闭
            case 's': sql_num = atoi(optarg); break;     // 数据库连接数
            case 't': thread_num = atoi(optarg); break;  // 线程数
            case 'c': close_log = atoi(optarg); break;   // 关闭日志
            case 'a': actor_model = atoi(optarg); break; // Proactor/Reactor
        }
    }
}
```

**⚠️ 注意 `getopt` 的冒号规则：**
- `"p:"` — 冒号表示 `-p` 后面**必须**跟一个参数
- `"p"` — 没有冒号表示 `-p` 是一个开关（不需要参数）

### Step 5：数据库相关的安全问题

**⚠️ SQL 注入风险：** 本项目为了保持教学简洁，直接在 `do_request()` 中拼接 SQL 字符串：

```cpp
// ⚠️ 教学代码，不安全！生产环境不能这样写
char *sql_insert = (char *)malloc(sizeof(char) * 200);
strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
strcat(sql_insert, "'");
strcat(sql_insert, name);  // 直接拼接用户输入！
strcat(sql_insert, "', '");
strcat(sql_insert, password);
strcat(sql_insert, "')");
mysql_query(mysql, sql_insert);
```

**正确的做法（生产环境）：**
1. 使用**参数化查询（Prepared Statement）**：`mysql_stmt_prepare` + `mysql_stmt_bind_param`
2. 对密码进行**哈希（hash）**，不要存明文：`SHA256(password + salt)`
3. 输入验证：用户名长度限制、不允许特殊字符

---

## 验证用例与预期结果

### 测试 1：编译

```bash
cd guide/phase_8/src
mkdir -p build && cd build
cmake ..
make
```

### 测试 2：启动服务器

```bash
./server -p 9006 -l 0 -m 0 -s 4 -t 4 -c 0 -a 0
```

**预期输出：**
- 无错误信息
- 日志文件 `ServerLog` 被创建

### 测试 3：浏览器访问静态页面

```
打开浏览器 → http://服务器IP:9006/
```

**预期：** 显示判断页面（judge.html），包含两个链接：注册和登录。

### 测试 4：注册

1. 点击"注册"链接 → 进入 register.html
2. 输入用户名和密码 → 提交
3. **预期：** 跳转到登录页面（log.html）

```bash
# 验证数据库中有新记录
mysql -u root -p -e "SELECT * FROM yourdb.user;"
```

### 测试 5：登录

1. 在登录页面输入刚注册的用户名和密码
2. **预期：** 跳转到欢迎页面（welcome.html）

### 测试 6：登录失败

1. 输入不存在的用户名或错误密码
2. **预期：** 显示错误页面（logError.html）

### 测试 7：curl 验证

```bash
# GET 请求
curl -v http://127.0.0.1:9006/

# 预期响应头包含：
# HTTP/1.1 200 OK
# Content-Type: text/html
# Connection: close

# POST 登录请求（模拟表单）
curl -v -X POST -d "user=test&passwd=123" http://127.0.0.1:9006/2CGISQL.cgi
```

### 测试 8：定时器验证

```bash
# 启动服务器
./server -p 9006

# 用 telnet 连接但不发送数据
telnet 127.0.0.1 9006

# 等待 15 秒（3 × TIMESLOT = 3 × 5 = 15 秒）
# 预期：telnet 连接自动断开
# 日志中出现 "close fd N"
```

### 测试 9：不同模式对比

```bash
# Proactor + LT+LT
./server -p 9006 -a 0 -m 0

# Proactor + ET+ET
./server -p 9007 -a 0 -m 3

# Reactor + LT+ET
./server -p 9008 -a 1 -m 1
```

### 失败排查

| 症状 | 可能原因 |
|------|---------|
| 启动时报 "Address already in use" | 上次运行的进程还在占用端口。`lsof -i :9006` 找到并 `kill` |
| 浏览器连接拒绝 | 防火墙。`sudo ufw allow 9006` |
| 注册后数据库没记录 | MySQL 服务没启动：`sudo systemctl start mysql` |
| 注册/登录后 500 错误 | 数据库表不存在。先用 README.md 中的 SQL 建表 |
| 图片/视频不显示 | root 目录路径错误。确保 `m_root` 路径正确指向 `root/` |
| 偶尔连接失败 | 文件描述符数达到上限。`ulimit -n 65536` |

---

## C++ 语法速查（本阶段涉及）

| 语法 | 示例 | 说明 |
|------|------|------|
| `new[]` / `delete[]` | `users = new http_conn[MAX_FD]` | 动态分配数组 |
| `getopt` | `getopt(argc, argv, "p:l:")` | POSIX 命令行解析（来自 C） |
| `atoi` | `atoi(optarg)` | ASCII → 整数（C 函数） |
| `static` 成员变量 | `static int m_user_count` | 类级别共享（所有实例同一份） |
| `friend` class | `friend class Utils` 在 http_conn 中 | 友元声明：允许其他类访问私有成员 |

---

## 阶段小结

你完成了整个项目的集成！最终的 Web 服务器包含：

- ✅ TCP 监听 + epoll 事件驱动（Phase 7）
- ✅ HTTP 请求解析 + 响应生成（Phase 6）
- ✅ 线程池并发处理（Phase 4）
- ✅ MySQL 连接池 + 注册/登录（Phase 3）
- ✅ 定时器清理超时连接（Phase 5）
- ✅ 同步/异步日志（Phase 2）
- ✅ Proactor / Reactor 双模式（Phase 4 + Phase 7）
- ✅ 命令行参数配置

**后续工作：** Phase 9 — 性能压测与工程实践。

---

## 🔒 安全注意事项

本项目的教学代码存在以下安全隐患（**不要直接用于生产环境**）：

1. **SQL 注入**：直接拼接用户输入到 SQL 语句（见 Step 5）
2. **密码明文存储**：`INSERT INTO user VALUES('name', 'passwd')` 直接存明文
3. **路径穿越**：`do_request()` 中直接将 URL 拼接到文件路径。攻击者可以用 `/../../../etc/passwd` 尝试读取系统文件
4. **内存泄露**：`do_request()` 中多处 `malloc`，异常路径可能泄露
5. **缓冲区溢出**：`strcpy`/`strcat` 的使用有潜在的溢出风险
