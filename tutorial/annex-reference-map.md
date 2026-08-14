# 附录 A3:阶段 ↔ 原项目文件对照表

> 学完全部阶段后,这张表帮你把"教程的每个阶段"映射到"原项目真实代码",作为复习和面试准备的索引。

## 1. 对照总表

| 教程阶段 | 你学到 / 复现了什么 | 原项目对应文件 | 对照方法 |
|---|---|---|---|
| C1~C6 | C++ 语法、类、RAII、模板、线程、现代特性 | 无(基础) | — |
| **S1** 单线程 echo | socket/bind/listen/accept/read/write 全流程 | `webserver.cpp` 的 `eventListen()`(建 socket)、`dealclientdata()`(accept) | `grep "eventListen" webserver.cpp` |
| **S2** 单线程 HTTP | HTTP 报文、请求行解析、静态文件映射 | `http/http_conn.cpp` 的 `parse_request_line()`、`do_request()` | 对比你的 S2 main 与 `do_request` 的 URL→文件逻辑 |
| **S3** 锁 + 线程池 | locker 封装、半同步半反应堆线程池 | `lock/locker.h`、`threadpool/threadpool.h` | `diff my_tiny_webserver/lock/locker.h lock/locker.h`(应一致) |
| **S4** epoll | epoll 三件套、LT/ET、事件循环 | `webserver.cpp` 的 `eventLoop()`、`dealwithread/write()` | 你的 S4 main 与 `eventLoop` 的分发逻辑对照 |
| **S5** HTTP 状态机 | 主/从状态机、mmap、writev、keep-alive | `http/http_conn.cpp` 的 `process_read/process_write/write/do_request` | `diff my_tiny_webserver/http/http_conn.cpp http/http_conn.cpp`(S9 后应一致) |
| **S6** 定时器 | 升序链表、信号→socketpair | `timer/lst_timer.h/.cpp`、`webserver.cpp` 的 `timer()/adjust_timer()/deal_timer()` | `diff my_tiny_webserver/timer/lst_timer.cpp timer/lst_timer.cpp` |
| **S7** 日志 | 阻塞队列、同步/异步、单例 | `log/block_queue.h`、`log/log.h/.cpp` | `diff my_tiny_webserver/log/log.cpp log/log.cpp`(应一致) |
| **S8** MySQL | 连接池、RAII 归还、注册登录 | `CGImysql/sql_connection_pool.h/.cpp`、`http_conn.cpp` 的 cgi 分支 | `diff my_tiny_webserver/CGImysql/sql_connection_pool.cpp CGImysql/sql_connection_pool.cpp` |
| **S9** 整合 | 装配、命令行参数、压测 | `main.cpp`、`config.h/.cpp`、`webserver.h/.cpp` | `diff my_tiny_webserver/webserver.cpp webserver.cpp`(应一致) |
| 附录 A1 | makefile 解读 | `makefile`(15 行) | 逐行看 |
| 附录 A2 | gdb 速查 | — | — |

## 2. 原项目文件全景

```text
TinyWebServer/
├── main.cpp                      # 装配入口(Stage 9)
├── config.h / config.cpp         # 命令行参数(Stage 9)
├── webserver.h / webserver.cpp   # 核心装配 + 事件循环(Stage 4/6/9)
├── http/
│   ├── http_conn.h / .cpp        # 连接类:状态机 + 响应(Stage 5/8)
│   └── README.md
├── threadpool/
│   └── threadpool.h              # 线程池模板(Stage 3)
├── timer/
│   ├── lst_timer.h / .cpp        # 升序链表定时器(Stage 6)
│   └── README.md
├── log/
│   ├── block_queue.h             # 阻塞队列(Stage 7)
│   ├── log.h / log.cpp           # 日志(Stage 7)
│   └── README.md
├── lock/
│   └── locker.h                  # 锁封装(Stage 3/7)
├── CGImysql/
│   ├── sql_connection_pool.h / .cpp  # 连接池(Stage 8)
│   └── README.md
├── root/                         # 静态资源(Stage 2 起)
├── test_pressure/
│   ├── README.md                 # 压测说明
│   └── webbench-1.5/             # webbench 源码(Stage 9 就地编译)
├── makefile                      # 原项目构建(附录 A1)
└── build.sh                      # 构建脚本(make server)
```

## 3. 依赖关系图(哪个模块依赖谁)

```text
                        ┌────────────────────┐
                        │   webserver.cpp     │  ← 顶层:组装一切
                        └───┬────────┬────────┘
                            │        │
              ┌─────────────▼──┐  ┌──▼──────────────┐
              │ threadpool<T>  │  │   http_conn       │
              └─────┬──────────┘  └──┬──────┬──────┬──┘
                    │                │      │      │
          ┌─────────▼───┐   ┌────────▼──┐ ┌─▼──┐ ┌─▼─────────┐
          │ lock/locker │   │ timer      │ │ log │ │ CGImysql   │
          └─────────────┘   │ lst_timer  │ │     │ │ connection │
                            └────────────┘ │     │ │   pool     │
                                           └─────┘ └────────────┘
    几乎所有模块都依赖 lock/locker.h(互斥锁/信号量/条件变量)
    log 还依赖 block_queue;http_conn 依赖 timer+log+连接池
```

**面试常问的"模块间关系"**:线程池处理 `http_conn` 类型的任务;`http_conn` 处理时要数据库连接(连接池借)、要记录日志(日志单例);定时器管 `http_conn` 的生死;它们之间的锁都来自 `locker.h`。

## 4. 学习路径回溯:每个阶段在整条线上的位置

```text
环境(Stage 0) → C++ 基础(C1~C6)
    → 能编译调试写类 → 
S1 socket 全流程 ─┐
S2 HTTP 静态     ─┤→ "看得见的服务器"
S3 线程池        ─┤→ 并发
S4 epoll        ──┤→ 事件驱动(本项目的灵魂)
S5 状态机        ─┤→ 健壮 HTTP
S6 定时器        ─┤→ 防泄漏
S7 日志          ─┤→ 可观测
S8 MySQL         ─┤→ 完整功能
S9 整合 + 压测    ─┘→ 完整 TinyWebServer
```

## 5. 进阶阅读(原项目作者推荐)

| 主题 | 推荐书籍章节 |
|---|---|
| socket 编程细节 | 《Unix网络编程》卷1:第 3 章(socket 基础)、第 6 章(I/O 复用:select/poll/epoll) |
| 进程/线程/信号 | 《Unix环境高级编程》:第 7~11 章(进程环境/控制/线程)、第 10 章(信号) |
| 定时器与事件驱动 | 《Unix网络编程》第 6 章 + Redis 源码的 ae 事件循环(进阶) |
| HTTP 协议 | RFC 7230(HTTP/1.1),或《HTTP 权威指南》 |
| 高并发架构 | 《Linux 高性能服务器编程》(游双,本项目很多思路源于此) |

**面试自测问题**(能不看代码答出,就算真懂了):
1. 为什么 socket 要设非阻塞?(S4)
2. ET 模式为什么必须循环读到 EAGAIN?(S4)
3. 为什么连接 fd 用 EPOLLONESHOT?(S5)
4. Reactor 和 Proactor 在本项目的区别?(S9)
5. writev 为什么适合发 HTTP 响应?(S5)
6. 定时器为什么用升序链表?(S6)
7. 信号为什么要转成 socketpair 事件?(S6)
8. 异步日志的阻塞队列解决什么问题?(S7)
9. 数据库连接池为什么用 RAII 包装?(S8)
10. 整个请求从进来到返回,经历了哪几个模块?(S9)
