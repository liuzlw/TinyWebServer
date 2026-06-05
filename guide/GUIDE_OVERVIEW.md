# TinyWebServer 从零实现 —— 教学总览

## 这是什么

一套分阶段教学材料，带你从 **零** 开始，逐步实现一个完整的 Linux C++ Web 服务器。

每完成一个阶段，你都会得到一个**看得见、跑得通**的结果，而不仅仅是一堆代码片段。

## 你会学到什么

| 维度 | 覆盖内容 |
|------|---------|
| **C++ 编程** | 类封装、RAII、模板、单例模式、状态机 |
| **Linux 系统编程** | socket、epoll、fcntl、mmap、writev、信号处理 |
| **多线程编程** | pthread、互斥锁、信号量、条件变量、线程池 |
| **网络协议** | HTTP/1.1 请求/响应格式、GET/POST |
| **数据库** | MySQL C API、连接池、注册/登录逻辑 |
| **工程工具** | cmake（构建系统）、gdb（调试器）、WebBench（压测） |

## 10 个阶段一览

```
Phase 0    Phase 1    Phase 2    Phase 3    Phase 4    Phase 5    Phase 6    Phase 7    Phase 8    Phase 9
 环境搭建 ─→ 线程同步 ─→ 日志系统 ─→ 连接池  ─→ 线程池  ─→ 定时器  ─→ HTTP解析 ─→ Socket ─→ 服务集成 ─→ 压测调优
  Hello     原语      阻塞队列    MySQL      任务调度    超时清理    状态机     epoll     完整Server  性能分析
```

每个阶段的详细内容和文件链接见下面。

---

## 阶段导航

### [Phase 0 — 环境搭建与工具入门](./phase_0/README.md)

- 搭建 Linux C++ 开发环境
- 对比 g++、make、cmake
- 第一个 CMake 项目
- gdb 断点调试入门

**产出：** 一个编译运行正确的 `hello` 程序。

---

### [Phase 1 — 线程同步原语](./phase_1/README.md)

- pthread 多线程编程
- 封装 mutex、semaphore、condition_variable
- 生产者-消费者模型测试

**产出：** 正确的多线程同步程序。

---

### [Phase 2 — 阻塞队列与日志系统](./phase_2/README.md)

- 循环数组阻塞队列
- 单例模式日志类
- 同步/异步写入、按天/按行分文件

**产出：** 实时写入的日志文件系统。

---

### [Phase 3 — MySQL 连接池](./phase_3/README.md)

- MySQL C API 操作
- 连接池设计（单例 + 信号量）
- RAII 自动归还连接

**产出：** 多线程安全地获取、使用、归还数据库连接。

---

### [Phase 4 — 线程池](./phase_4/README.md)

- 通用模板线程池
- 信号量 + 互斥锁的任务队列
- Reactor / Proactor 两种模型对比

**产出：** 多线程并行执行提交的任务。

---

### [Phase 5 — 定时器](./phase_5/README.md)

- 升序双向链表定时器
- SIGALRM + socketpair 统一信号到 epoll
- tick 机制扫除超时连接

**产出：** 超时连接自动断开、资源清理。

---

### [Phase 6 — HTTP 解析器](./phase_6/README.md)

- HTTP/1.1 协议格式
- 有限状态机：请求行 → 请求头 → 请求体
- GET / POST 处理
- 响应行、响应头、Body 构造

**产出：** 给定原始 HTTP 文本，输出正确的解析结果和响应。

---

### [Phase 7 — 非阻塞 Socket + epoll](./phase_7/README.md)

- socket / bind / listen / accept
- fcntl 非阻塞、setsockopt
- epoll 事件驱动（LT / ET / EPOLLONESHOT）

**产出：** 单线程 echo 服务器，telnet 交互正确回显。

---

### [Phase 8 — 服务集成](./phase_8/README.md)

- WebServer 主类串联所有模块
- 命令行参数解析
- 注册/登录 + 静态文件服务
- 🔒 安全注意事项：SQL 注入、路径穿越防护

**产出：** 浏览器访问完整 Web 服务。

---

### [Phase 9 — 性能压测与工程实践](./phase_9/README.md)

- WebBench 编译与使用
- ET/LT 四种组合性能对比
- Reactor / Proactor 模式对比
- core dump 分析
- perf 火焰图 CPU 热点分析
- 📖 附录：C++11 现代替代方案（std::thread、std::mutex 等对照）

**产出：** 上万 QPS 并发连接 + 性能分析报告。

---

## 前置条件

- 基本 C++ 语法：类、模板、指针、引用、STL 容器（vector / list / map）
- 愿意在 Linux 环境下工作（WSL / 虚拟机 / 云服务器）
- 对"网络"有概念性了解即可（知道 IP、端口是什么）

## 工具教学穿插表

每个阶段在实现模块的同时，还教 1-2 个工具技巧：

| 阶段 | cmake 新技能 | gdb 新技能 |
|------|-------------|-----------|
| Phase 0 | `project`、`add_executable`、`cmake -B build` | 断点、单步、`print`、`backtrace` |
| Phase 1 | `include_directories`、多文件编译 | `thread apply all bt`、线程切换 |
| Phase 2 | `add_library`、`target_link_libraries`（静态库） | 条件断点、`watchpoint` |
| Phase 3 | `find_package` / `find_library`（链接外部库） | — |
| Phase 4 | `add_subdirectory`（多目录项目） | 死锁检测、mutex 状态查看 |
| Phase 5 | — | `handle` 信号调试 |
| Phase 6 | `CMAKE_BUILD_TYPE`（Debug/Release） | — |
| Phase 7 | — | — |
| Phase 8 | 最终项目 CMakeLists.txt 整合 | — |
| Phase 9 | Release 构建、`-O2`/`-O3` 优化 | `core dump` 分析 |

## 如何使用这套材料

1. **按顺序来。** 每个阶段依赖前一个阶段的知识，不要跳。
2. **先读后写。** 读完 README 后自己手打代码，不要复制粘贴。
3. **每阶段都跑通验证。** 验证不通过不进入下一阶段。
4. **遇到 bug 先 gdb。** 每个阶段的"踩坑记录"列出该模块典型错误，先看那里。
5. **和本项目的源码对照。** 整套材料以本项目源码为蓝本。如果你卡住了，可以回来看 `../http/http_conn.cpp` 等对应文件——但尽量先自己写。

## 与项目源码的对应关系

| 阶段 | 对应项目源码 |
|------|-------------|
| Phase 1 | `lock/locker.h` |
| Phase 2 | `log/block_queue.h`、`log/log.h`、`log/log.cpp` |
| Phase 3 | `CGImysql/sql_connection_pool.h`、`CGImysql/sql_connection_pool.cpp` |
| Phase 4 | `threadpool/threadpool.h` |
| Phase 5 | `timer/lst_timer.h`、`timer/lst_timer.cpp` |
| Phase 6 | `http/http_conn.h`、`http/http_conn.cpp` |
| Phase 7 | `webserver.cpp` 中 socket/epoll 部分 |
| Phase 8 | `webserver.h`、`webserver.cpp`、`main.cpp`、`config.h`、`config.cpp` |
| Phase 9 | `test_pressure/webbench-1.5/`、压测参数对比 |
