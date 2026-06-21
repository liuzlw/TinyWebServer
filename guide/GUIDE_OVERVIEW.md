# TinyWebServer 从零实现 —— 教学总览

> 📖 **阅读时间：** 约 20 分钟（概述）+ 每阶段 2-6 小时（动手实践）
> 🎯 **适合人群：** 有基本 C++ 语法基础，想通过实战项目深入理解 Linux 网络编程的开发者
> 💻 **运行环境：** Linux（WSL/虚拟机/云服务器）+ MySQL 5.7+

---

## 这是什么

一套**分阶段、可验证**的教学材料，带你从零开始，逐步实现一个完整的 Linux C++ Web 服务器。

每完成一个阶段，你都会得到一个**看得见、跑得通**的结果——而不是一堆不知对错的代码片段。

## 你会学到什么

| 维度 | 覆盖内容 |
|------|---------|
| **C++ 编程** | 类封装、RAII、模板、单例模式、状态机、友元、静态成员 |
| **Linux 系统编程** | socket / bind / listen / accept、epoll（LT+ET）、fcntl、mmap、writev、信号处理、alarm |
| **多线程编程** | pthread、互斥锁、信号量、条件变量、线程池（半同步/半反应堆） |
| **网络协议** | HTTP/1.1 请求/响应格式、GET/POST、Content-Length、Connection、状态码 |
| **数据库** | MySQL C API、连接池、RAII 自动归还 |
| **工程工具** | g++ 编译链 → Makefile → CMake、gdb 调试（断点/单步/watch/条件断点/attach/thread）、strace 系统调用追踪、WebBench 压力测试 |

---

## 核心理论：Web 服务器的本质

### 一个 Web 服务器究竟在做什么？

```
浏览器                          服务器
  │                               │
  │ ─── TCP 三次握手 ──────────▶  │  内核完成
  │                               │
  │ ─── GET /index.html HTTP/1.1 ▶│  服务器读取
  │     Host: example.com         │
  │                               │      ↓ 解析请求
  │                               │      ↓ 找到 /root/index.html
  │                               │      ↓ 打开文件、读入内存
  │                               │      ↓ 拼 HTTP 响应
  │                               │
  │ ◀── HTTP/1.1 200 OK ────────  │  服务器发送
  │     Content-Length: 1234      │
  │     <html>...</html>          │
  │                               │
  │ ─── TCP 四次挥手 ──────────▶  │  连接关闭
```

**本质上是三件事：**
1. **接收**字节流（TCP）
2. **理解**字节流的含义（HTTP 协议解析）
3. **响应**正确的字节流（文件读取 + 业务逻辑 + HTTP 响应构造）

**难点在于**：一台服务器要同时处理成千上万个这样的对话，不能因为某个人网速慢就卡住所有人。

---

## 架构蓝图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         WebServer (主控类)                           │
│  main.cpp → Config::parse_arg → WebServer::init → ... → eventLoop   │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
        ┌──────────────────────┼──────────────────────┐
        │                      │                      │
   ┌────▼─────┐         ┌──────▼──────┐        ┌──────▼──────┐
   │ 监听层    │         │  事件驱动层  │        │  业务处理层  │
   │ socket()  │  ────▶  │  epoll      │ ────▶  │  threadpool  │
   │ bind()    │  accept │  epoll_wait │ 投递   │  工作线程    │
   │ listen()  │         │  5类事件分发│        │  处理请求    │
   └───────────┘         └──────┬──────┘        └──────┬──────┘
                                │                      │
                    ┌───────────┼───────────┐          │
                    │           │           │          │
              ┌─────▼────┐ ┌───▼────┐ ┌───▼────┐     │
              │ 信号管道  │ │ 可读   │ │ 可写   │     │
              │ pipefd   │ │ EPOLLIN│ │EPOLLOUT│     │
              │(SIGALRM) │ │        │ │        │     │
              └─────┬────┘ └───┬────┘ └───┬────┘     │
                    │          │           │          │
              ┌─────▼────┐ ┌───▼────┐ ┌───▼────┐     │
              │ 定时器    │ │ HTTP   │ │ writev  │     │
              │ tick()   │ │ 解析   │ │ 发送    │◄────┘
              │ 清理超时  │ │ process│ │ 响应    │
              └──────────┘ └───┬────┘ └─────────┘
                               │
                        ┌──────▼──────┐
                        │  支撑模块    │
                        ├─────────────┤
                        │ Log (Phase2)│  ← 贯穿：记录每步操作
                        │ locker.h    │  ← 贯穿：并发安全
                        │ MySQL Pool  │  ← 按需：注册/登录时获取连接
                        └─────────────┘
```

### 模块间数据流

```
[浏览器] ──TCP──▶ [listenfd]
                      │ accept
                      ▼
                 [connfd] ──epoll注册──▶ [m_epollfd]
                      │
                      │ EPOLLIN 事件
                      ▼
                 [read_once()]  ◀── 读缓冲区 m_read_buf[2048]
                      │
                      │ 原始 HTTP 文本
                      ▼
                 [process_read()]  ◀── 有限状态机
                      │                 请求行 → 请求头 → 请求体
                      │ 解析结果:
                      │  - m_url, m_method (GET/POST)
                      │  - m_content_length
                      │  - m_string (POST 的 body)
                      ▼
                 [do_request()]
                      │
         ┌────────────┼────────────┐
         │            │            │
    静态文件      CGI: 登录      CGI: 注册
    mmap 映射    查 users map   查重 → INSERT
         │            │            │
         └────────────┼────────────┘
                      │
                      ▼
                 [process_write()]
                      │
                      │ m_write_buf[1024] (响应头)
                      │ + m_file_address (mmap 的文件内容)
                      ▼
                 [m_iv[2]]: iovec 数组
                      │ iv[0] = 响应头
                      │ iv[1] = 文件内容 (mmap)
                      ▼
                 [writev(connfd, m_iv, 2)]
                      │ 一次系统调用，发送两块不连续内存
                      ▼
                 [浏览器收到 HTTP 响应]
```

---

## 10 个阶段一览

```
Phase 0    Phase 1    Phase 2    Phase 3    Phase 4    Phase 5
 环境搭建 ─→ 线程同步 ─→ 日志系统 ─→ 连接池  ─→ 线程池  ─→ 定时器
 Hello      原语      阻塞队列    MySQL      任务调度    超时清理
 世界

Phase 6    Phase 7    Phase 8    Phase 9
 HTTP解析 ─→ Socket  ─→ 服务集成 ─→ 压测调优
 状态机     epoll     完整Server   性能分析
```

### 依赖关系

```
Phase 0 ─── 环境搭建 (独立)
   │
Phase 1 ─── 线程同步原语 (独立)
   │
   ├──▶ Phase 2 ─── 阻塞队列 + 日志系统 (依赖 Phase 1)
   │
   ├──▶ Phase 3 ─── MySQL 连接池 (依赖 Phase 1)
   │
   ├──▶ Phase 4 ─── 线程池 (依赖 Phase 1, Phase 3)
   │
   ├──▶ Phase 5 ─── 定时器 (依赖 Phase 1)
   │
   ├──▶ Phase 6 ─── HTTP 解析器 (独立，理论层面)
   │
   ├──▶ Phase 7 ─── Socket + epoll (独立，最底层的网络部分)
   │
   └──▶ Phase 8 ─── 服务集成 (依赖 Phase 1-7 全部)
         │
         └──▶ Phase 9 ─── 压测调优 (依赖 Phase 8)
```

### 各阶段产出

| 阶段 | 核心产出 | 代码量 | 预计耗时 |
|------|---------|--------|---------|
| Phase 0 | 环境搭建 + Hello World | ~30 行 | 2-4 小时 |
| Phase 1 | locker.h（三种同步原语） | ~120 行 | 3-5 小时 |
| Phase 2 | block_queue.h + log.h/log.cpp | ~270 行 | 4-6 小时 |
| Phase 3 | sql_connection_pool.h/.cpp | ~150 行 | 3-5 小时 |
| Phase 4 | threadpool.h | ~150 行 | 4-6 小时 |
| Phase 5 | lst_timer.h/.cpp | ~220 行 | 3-5 小时 |
| Phase 6 | http_conn.h/.cpp（最复杂） | ~700 行 | 6-10 小时 |
| Phase 7 | echo_server（epoll + 非阻塞） | ~200 行 | 4-6 小时 |
| Phase 8 | WebServer 主类 + 集成 | ~650 行 | 6-10 小时 |
| Phase 9 | WebBench 压测 + 性能分析 | ~50 行 | 3-5 小时 |
| **总计** | | **~2500 行** | **38-62 小时** |

---

## 工具教学穿插表

每个阶段在实现模块的同时，还教授 1-2 个工具技巧：

| 阶段 | cmake 新技能 | gdb 新技能 | 其他工具 |
|------|-------------|-----------|---------|
| Phase 0 | `project`、`add_executable`、`cmake -B build` | 断点、单步、`print`、`backtrace` | g++、make |
| Phase 1 | `include_directories`、多文件编译 | `thread apply all bt`、线程切换 | ThreadSanitizer |
| Phase 2 | `add_library`、`target_link_libraries`（静态库） | 条件断点（`break if`）、`watchpoint` | — |
| Phase 3 | `find_package` / `find_library`（链接外部库） | `attach` 到运行中的进程 | mysql 命令行 |
| Phase 4 | `add_subdirectory`（多目录项目） | 死锁检测、mutex 状态查看 | — |
| Phase 5 | — | `handle SIGALRM nostop noprint pass` | — |
| Phase 6 | `CMAKE_BUILD_TYPE`（Debug/Release） | 条件断点在状态机中 | — |
| Phase 7 | — | errno 检查 | strace |
| Phase 8 | 最终项目 CMakeLists.txt 整合 | — | curl、telnet |
| Phase 9 | Release 构建（`-O2`/`-O3`） | core dump 分析 | WebBench、perf |

---

## 如何使用这套材料

### 黄金规则

1. **按顺序来，不要跳。** Phase 0-9 的设计有知识依赖关系。跳过某个阶段会导致后续阶段看不懂。
2. **先读后写。** 读完 README 后自己手打代码，不要复制粘贴。**手打代码的过程就是学习和记忆的过程。**
3. **每阶段都跑通验证。** 每个 Phase 末尾有明确的验证清单，全部通过才能进入下一阶段。
4. **遇到 bug 先 gdb。** 这是培养调试能力的最好机会。每个阶段的"踩坑记录"列出该模块的典型错误——先看那里。
5. **和项目源码对照。** 教你写的代码以本项目（`../` 目录下的源码）为蓝本。如果卡住了，可以回来看对应文件——但尽量先自己写。
6. **理解 > 背诵。** 不要背诵代码。理解"为什么这样设计"比记住"这行写的是什么"重要 100 倍。

### 推荐的学习节奏

- **每天 1 个 Phase**（约 3-6 小时）
- **每个 Phase 至少敲一遍**，不要求一次写对
- **周末集中攻克 Phase 6 和 Phase 8**（这两个最复杂、代码最多）

---

## 与项目源码的对应关系

| 阶段 | 对应项目源码 | 教学重点 |
|------|-------------|---------|
| Phase 0 | — | g++/make/cmake/gdb 工具链 |
| Phase 1 | `lock/locker.h` | sem、locker、cond 三种同步原语 |
| Phase 2 | `log/block_queue.h`、`log/log.h`、`log/log.cpp` | 循环数组阻塞队列 + 单例日志 |
| Phase 3 | `CGImysql/sql_connection_pool.h/.cpp` | 单例连接池 + RAII |
| Phase 4 | `threadpool/threadpool.h` | 模板线程池 + Proactor/Reactor |
| Phase 5 | `timer/lst_timer.h/.cpp` | 升序链表定时器 + 信号驱动 |
| Phase 6 | `http/http_conn.h`、`http/http_conn.cpp` | HTTP 状态机 + mmap + writev |
| Phase 7 | `webserver.cpp` 中 socket/epoll 部分 | 非阻塞 + epoll + LT/ET |
| Phase 8 | `webserver.h`、`webserver.cpp`、`main.cpp`、`config.h/.cpp` | 集成所有模块 |
| Phase 9 | `test_pressure/webbench-1.5/` | 性能测试与分析 |

---

## 前置条件

- **C++ 基本语法：** 类、模板、指针、引用、`new`/`delete`、STL 容器（list / map / string）
- **Linux 基本操作：** 会用终端敲命令（cd、ls、mkdir、cp、mv）
- **愿意在 Linux 环境下工作：** WSL、虚拟机、或云服务器
- **对"网络"有概念性了解：** 知道 IP 地址和端口是什么

如果你还不会这些，建议先花 1-2 天时间学习 C++ 基础语法（类、指针、STL 容器），然后再开始本教程。

---

## 面试和自测问题

完成 Phase 8 后，你应该能回答以下问题：

**网络编程：**
- 为什么 socket 要设置非阻塞？
- ET 模式和 LT 模式的本质区别是什么？ET 为什么必须循环读到 EAGAIN？
- 为什么连接 fd 使用 EPOLLONESHOT？
- `epoll` 为什么比 `select` 和 `poll` 快？
- `listen` 的 backlog 参数是什么意思？实际最大连接数由什么决定？

**并发模型：**
- Reactor 和 Proactor 在这个项目中的区别是什么？主线程和工作线程的职责有何不同？
- `writev` 为什么适合发送 HTTP 响应？它比两次 `write` 好在哪里？
- 线程池为什么使用"半同步/半反应堆"模式？

**资源管理：**
- 定时器为什么和连接 fd 绑定？如果定时器触发时连接还在使用会怎样？
- MySQL 连接池为什么用 RAII 包装？信号量在连接池中起什么作用？
- `mmap` 和普通 `read` 的区别是什么？为什么发送静态文件用 `mmap` 而不是 `read`？

**日志系统：**
- 异步日志的阻塞队列解决了什么问题？同步日志和异步日志各有什么适用场景？
- 单例模式为什么用局部静态变量而不是 `new` + 锁？

**HTTP 协议：**
- HTTP 请求的报文格式是什么？（请求行 + 请求头 + 空行 + 请求体）
- POST 和 GET 的主要区别是什么？
- 状态机解析 HTTP 报文有什么好处？

---

**准备好了吗？从 [Phase 0 — 环境搭建与工具入门](./phase_0/README.md) 开始吧！**
