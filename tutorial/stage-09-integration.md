# Stage 9 整合与压力测试

> 🏆 **收官阶段**。把前面所有模块用原版完整代码组装成真正的 TinyWebServer,支持命令行参数,并用 webbench 压测。做完这一步,你的 `my_tiny_webserver/` 就是一台完整的 Web 服务器。

## 1. 本阶段目标

- [ ] 用原版完整代码替换各模块的简化版
- [ ] 理解 `main → init → log_write → sql_pool → thread_pool → trig_mode → eventListen → eventLoop` 装配流程
- [ ] 理解命令行参数(`-p -l -m -o -s -t -c -a`)
- [ ] 用 webbench 压测服务器
- [ ] 全功能回归:静态页面、keep-alive、注册登录、定时器、日志全部正常

**最终效果:** `./server -p 9006 -m 3 -a 0` 这样启动,webbench 压出上万 QPS。

## 2. 前置知识

- S1~S8 全部内容。本阶段不做新功能,只做**组装**和**验证**。

## 3. 整合蓝图:用原版替换简化版

前 8 个阶段,为了"每阶段能独立编译运行",我们把一些模块做了简化。现在用**原版完整代码**替换它们(这些简化版本来就是原版的子集,你早就看懂了):

| 文件 | 阶段 | 现在的动作 |
|---|---|---|
| `main.cpp` | S1~S8 逐阶段演化 | **换成原版** `main.cpp`(装配 WebServer) |
| `config.h` + `config.cpp` | 新增 | **复制原版**(命令行参数解析) |
| `webserver.h` + `webserver.cpp` | 新增 | **复制原版**(核心装配 + 事件循环) |
| `http/http_conn.h/.cpp` | S5 简化 → S8 补 MySQL | **换成原版**(补回 LOG 调用等) |
| `threadpool/threadpool.h` | S3 简化 | **换成原版**(补回 actor 模型 + 连接池成员) |
| `timer/lst_timer.h/.cpp` | S6 简化 | **换成原版**(补回 log.h include) |
| `lock/locker.h` | S3→S7 | 已经是原版,不动 |
| `log/*`、`CGImysql/*` | S7/S8 | 已经是原版,不动 |
| `root/` | S2 | 复制原版完整静态资源 |

**复制命令**(在 `my_tiny_webserver/` 下,原仓库就在上一级):

```bash
# 主文件(注意会覆盖你写的 main.cpp)
cp ../main.cpp ../config.h ../config.cpp ../webserver.h ../webserver.cpp .
# 各模块(覆盖简化版)
cp ../http/http_conn.h ../http/http_conn.cpp http/
cp ../threadpool/threadpool.h threadpool/
cp ../timer/lst_timer.h ../timer/lst_timer.cpp timer/
# 静态资源(补全图片/视频等)
cp -r ../root/* root/
```

> 复制前建议先把 `my_tiny_webserver/` 打一个 git 提交,留个"简化版"里程碑:
> ```bash
> git add -A && git commit -m "简化版完成, 准备整合为原版"
> ```

## 4. main.cpp:装配入口

**替换 `my_tiny_webserver/main.cpp`** 为原版(40 行):

```cpp
#include "config.h"

int main(int argc, char *argv[])
{
    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "qgydb";

    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite, 
                config.OPT_LINGER, config.TRIGMode,  config.sql_num,  config.thread_num, 
                config.close_log, config.actor_model);
    
    //日志
    server.log_write();

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    return 0;
}
```

**这就是整个项目的装配顺序**,一行一个模块,每个函数干一件事:

```text
Config::parse_arg    解析命令行参数(端口/日志/触发模式/线程数...)
WebServer::init      把参数存进成员变量
log_write()          初始化日志
sql_pool()           初始化数据库连接池,加载 user 表
thread_pool()        创建线程池
trig_mode()          根据 -m 参数算出 listenfd/connfd 的 LT/ET 组合
eventListen()        socket/bind/listen + epoll + 信号管道
eventLoop()          主循环:epoll_wait → 分发处理
```

> 对比 S1~S8 的 `main.cpp`:你现在知道每个函数内部是什么了。`eventListen` 就是 S4/S6 学过的那套(socket + epoll + socketpair 信号),`sql_pool` 是 S8 的 `init_sql`,`eventLoop` 是 S6/S7 的事件循环加上了 `dealwithread/write` 的线程池分发。

## 5. config:命令行参数

`config.h` + `config.cpp`(原版)定义了 8 个启动参数:

| 参数 | 含义 | 默认 |
|---|---|---|
| `-p` | 端口 | 9006 |
| `-l` | 日志方式:`0` 同步,`1` 异步 | 0 |
| `-m` | 触发模式:`0` LT+LT,`1` LT+ET,`2` ET+LT,`3` ET+ET | 0 |
| `-o` | 优雅关闭:`0` 关,`1` 开 | 0 |
| `-s` | 数据库连接池连接数 | 8 |
| `-t` | 线程池线程数 | 8 |
| `-c` | 关闭日志:`0` 开,`1` 关 | 0 |
| `-a` | 并发模型:`0` Proactor,`1` Reactor | 0 |

实现是标准 `getopt` 循环,`optarg` 是参数值字符串,`atoi` 转成整数:

```cpp
void Config::parse_arg(int argc, char*argv[]){
    int opt;
    const char *str = "p:l:m:o:s:t:c:a:";   // 冒号表示该选项后跟参数
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p': { PORT = atoi(optarg); break; }
        case 'l': { LOGWrite = atoi(optarg); break; }
        // ... 其余类似
        }
    }
}
```

**trig_mode() 把 `-m` 拆成两个:** `m_LISTENTrigmode`(listenfd 用)和 `m_CONNTrigmode`(连接用):

```text
-m 0:  LT + LT      -m 1:  LT + ET
-m 2:  ET + LT      -m 3:  ET + ET
```

## 6. webserver.cpp:核心装配

`webserver.h` + `webserver.cpp`(原版,共约 500 行)——把 S1~S8 的所有东西拧成一台机器。**复制后对照原版逐段看**,重点看懂三处:

### ① eventLoop:最终版事件循环

```cpp
void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
            break;

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)            dealclientdata();   // 新连接
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                                                 deal_timer(...);    // 关闭+删定时器
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
                                                 dealwithsignal(timeout, stop_server);  // 信号
            else if (events[i].events & EPOLLIN) dealwithread(sockfd);
            else if (events[i].events & EPOLLOUT) dealwithwrite(sockfd);
        }
        if (timeout)    // 收到 SIGALRM
        {
            utils.timer_handler();      // tick() + alarm()
            timeout = false;
        }
    }
}
```

这就是 S4~S6 事件循环的"完全体":监听 / 关闭 / 信号 / 读 / 写五种事件,全部分发到对应的 `deal*` 函数。

### ② dealwithread:Reactor 与 Proactor 在这里分流

```cpp
void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    if (1 == m_actormodel)     // Reactor:工作线程自己读
    {
        if (timer) adjust_timer(timer);
        m_pool->append(users + sockfd, 0);   // 任务丢给线程池,工作线程读+处理
        // 忙等 improv,等线程池处理完
        while (true) { ... }
    }
    else                       // Proactor:主线程读好再交给线程池
    {
        if (users[sockfd].read_once())      // 主线程读
        {
            m_pool->append_p(users + sockfd);  // 读好的请求交给线程池处理
            if (timer) adjust_timer(timer);
        }
        else
            deal_timer(timer, sockfd);
    }
}
```

对比 S4 学过的两种模型——现在看真实代码,一目了然:
- **Proactor(`-a 0`)**:主线程 `read_once()` 读完,`append_p` 把"已读好的 http_conn"丢给线程池
- **Reactor(`-a 1`)**:主线程只 `append`,工作线程自己 `read_once()` + 处理

### ③ thread_pool:原版线程池(actor 分支)

S3 学的是简化版,原版多了 `m_actor_model` 分支。工作线程 `run()` 里:

```cpp
if (1 == m_actor_model)        // Reactor:工作线程自己读写
{
    if (0 == request->m_state)  // 读事件
        if (request->read_once()) { ...; request->process(); }
    else                        // 写事件
        if (request->write()) ...
}
else                           // Proactor:主线程已读好,工作线程只处理
{
    connectionRAII mysqlcon(&request->mysql, m_connPool);
    request->process();
}
```

**`improv` / `timer_flag` 的配合**:Reactor 模式里主线程把任务丢给线程池后,要等线程池处理完才知道"连接是否还该留"。工作线程处理完置 `improv=1`;如果读失败置 `timer_flag=1`。主线程 `while(true)` 忙等 `improv`,看到 `timer_flag` 就关连接。这是多线程下"主线程等待工作线程结论"的典型做法。

## 7. 完整 CMakeLists.txt(已验证可构建)

**替换 `my_tiny_webserver/CMakeLists.txt`**:

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)
target_link_libraries(server pthread mysqlclient)
```

> 到这一步,`CMakeLists.txt` 里把所有模块的 `.cpp` 都列上了。它做的事和原项目 `makefile` 一模一样(附录 A1 详解)。**头文件不用列**(`#include` 自己带),模板实现本来就在 `.h` 里。

## 8. 编译与运行

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
```

**启动(常用几种):**

```bash
./build/server -p 9006                  # 最简:默认 LT+LT, Proactor
./build/server -m 3                     # ET + ET
./build/server -m 1 -a 0                # LT+ET, Proactor(原项目默认推荐)
./build/server -l 1 -c 0                # 异步日志
./build/server -t 16 -s 16              # 16 线程 + 16 个数据库连接
```

**预期输出:**

```text
数据库连接池初始化完成(若 MySQL 就绪)
```

> 压测前建议 `-c 1` 关闭日志(写日志会拖慢 QPS)。

## 9. webbench 压力测试

webbench 是原项目作者推荐的压测工具。原仓库 `test_pressure/webbench-1.5/` 就带着完整源码(webbench.c + Makefile),可以直接就地编译;想用官方 GitHub 仓库重新编译也行。**推荐就地编译**(不需要联网):

```bash
# 方案 A(推荐):用原仓库自带的 webbench-1.5 编译
cd ~/TinyWebServer/test_pressure/webbench-1.5
make
sudo cp webbench /usr/local/bin/

# 方案 B:从 GitHub 拉源码编译(和 A 等价)
cd ~
git clone https://github.com/EZLippi/WebBench.git
cd WebBench
make
sudo cp webbench /usr/local/bin/
```

**压测:**

```bash
# 先启动服务器(关闭日志, ET+ET, 高连接数限制)
./build/server -m 3 -c 1 -t 16 &

# 另开终端压测:1000 个并发客户端, 测 5 秒
webbench -c 1000 -t 5 http://127.0.0.1:9006/

# 若报 "Too many open files",调大 fd 上限
ulimit -n 65535
```

**预期输出格式(QPS 数值因机器而异):**

```text
Webbench - Simple Web Benchmark 1.5
Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.

Benchmarking: GET http://127.0.0.1:9006/
1000 clients, running 5 sec.

Speed=360000 pages/min, 6000 pages/sec,
Requests: 30000 susceed, 0 failed.
```

- `Requests: ... susceed, 0 failed.` ← **0 failed 是重点**,证明没有连接被丢
- 上面的数字是"能自洽"的示例,不是你的机器能跑到的值:**`pages/sec × 60 = pages/min`、`pages/sec × 秒数 = susceed 数`**(6000×5=30000)。如果你跑出来对不上,说明压测中途有连接被断,值得查。
- 你的机器跑出的 QPS 不设硬指标——项目 README 里作者在专用机器上跑到 9 万+ QPS,普通笔记本几千到几万都正常

> 如果压测时服务器崩溃或大量 failed,通常是因为连接数超了 `ulimit -n` 或 ET 模式有 bug。先 `ulimit -n 65535` 再压。

## 10. 验收清单(全功能回归 + 压测)

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `./build/server` 启动 | 能启动,日志正常 | ☐ |
| 2 | `curl http://127.0.0.1:9006/welcome.html` | 200(S2 功能) | ☐ |
| 3 | 浏览器 `/0` 注册、`/1` 登录、`/5` 图片、`/6` 视频 | 全部正常(S8 + 静态功能) | ☐ |
| 4 | `mysql ... SELECT * FROM user` | 能看到注册记录(持久化) | ☐ |
| 5 | `nc` 空闲连接 20 秒 | 被定时器关闭,日志有 tick(S6) | ☐ |
| 6 | `tail -f *ServerLog` | 请求/连接日志滚动(S7) | ☐ |
| 7 | 并发两个 curl | 同时响应(线程池) | ☐ |
| 8 | `./build/server -m 0` 与 `-m 3` 对比 | 都能正常服务(LT/ET 都通) | ☐ |
| 9 | `./build/server -a 0` 与 `-a 1` 对比 | 都能正常服务(Proactor/Reactor 都通) | ☐ |
| 10 | `webbench -c 1000 -t 5 http://127.0.0.1:9006/` | 跑完输出 QPS,`0 failed` | ☐ |
| 11 | 压测后服务器仍响应 | 没有崩溃、没有 fd 泄漏 | ☐ |

> 第 8、9 条是"四种触发模式 × 两种并发模型"的组合测试,对应原项目 README 里说的所有配置都能跑。

## 11. 调试技巧

### gdb 观察事件循环

```bash
gdb ./build/server
```

```text
(gdb) break webserver.cpp:396     ← 断在 eventLoop 的事件分发
(gdb) run -m 3 -a 0
(gdb) next                        ← 另开终端 curl 后,单步看走哪个分支
(gdb) print events[i].events      ← 看事件类型(EPOLLIN/EPOLLOUT)
```

### 压测时 attach 到运行中的服务器

```bash
gdb -p $(pgrep -f "build/server")
```

```text
(gdb) bt                          ← 看它当时卡在哪
(gdb) print http_conn::m_user_count   ← 当前连接数
```

## 12. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 压测报 `Too many open files` | fd 上限太低 | `ulimit -n 65535` |
| 压测大量 failed | 连接被拒/超时 | 确认 `ulimit`,日志看报错 |
| `mysql: Access denied` | root 密码不对 | 重新设 root 密码(Stage 0) |
| `-m 3` 下页面偶发白屏 | ET 读取 bug | 对比 `-m 0`;ET 必须循环读到 EAGAIN |
| 编译报 `m_pool not declared` | webserver.h 没复制全 | 检查 `webserver.h` 里 `threadpool<http_conn> *m_pool;` |
| 启动就崩(assert 失败) | bind/连接池失败 | 看是哪行 assert,通常端口被占或 MySQL 没起 |
| 压测完服务器内存涨 | 可能有连接泄漏 | 定时器没清干净,对照 S6 |

## 13. 与原项目对照

**本阶段完成后,`my_tiny_webserver/` 应该和原项目基本一致。** diff 验证:

```bash
cd ~/TinyWebServer                  # Stage 0 建过软链接,等价于 /mnt/c/.../TinyWebServer
diff my_tiny_webserver/webserver.cpp webserver.cpp          # 应无差异
diff my_tiny_webserver/http/http_conn.cpp http/http_conn.cpp # 应无差异
diff my_tiny_webserver/main.cpp main.cpp                     # 应无差异
```

**差异只剩合理的两处:**
1. `my_tiny_webserver` 用 `CMakeLists.txt` 构建(原项目用 makefile)——附录 A1 讲清了二者对应关系
2. 你可能给 `cb_func` 加了调试 printf(教程建议的观察日志),原版没有

## 14. 结语 🎉

到这里,你从零复现了 TinyWebServer。回头看这条学习线:

| 阶段 | 你掌握了 |
|---|---|
| C1~C6 | C++ 语法、类、RAII、模板、线程、现代特性 |
| S1 | socket 编程全流程 |
| S2 | HTTP 协议、静态文件服务 |
| S3 | 锁封装、半同步半反应堆线程池 |
| S4 | epoll、LT/ET、Reactor/Proactor 概念 |
| S5 | HTTP 状态机、mmap、writev |
| S6 | 升序链表定时器、信号→socketpair |
| S7 | 阻塞队列、同步/异步日志、单例 |
| S8 | 数据库连接池、RAII 归还、注册登录 |
| S9 | 装配、命令行参数、压测 |

**进阶建议**(原项目作者推荐):
- 把每个 `deal*` 函数、`http_conn` 每个成员变量都过一遍,能不看代码说出它们的作用
- 面试前重读附录 A3 对照表 + [gdb 速查表](annex-gdb-cheatsheet.md)
- 深度进阶读《Unix环境高级编程》《Unix网络编程》的对应章节(附录 A3 有索引)

**你现在的 `my_tiny_webserver/` 是一台能注册、能登录、能发图发视频、能扛住压测的服务器——这就是你亲手敲出来的 TinyWebServer。**
