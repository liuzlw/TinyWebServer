# TinyWebServer 从零复现教程（主索引）

> 面向 C++ 入门者：以复现本项目为主线，边做边学 C++ 语法、数据结构、项目组织，以及 make / CMake / gdb 工具链。
> 每个阶段都产出一个**能编译、能运行、能验收**的里程碑；走完全程，你就从零手写了这台 Web 服务器。

## 这套教程能帮你实现什么

| 目标 | 教程如何兑现 |
|---|---|
| **从零完整复现本项目，理论与实践都懂** | 10 个阶段（Stage 0 环境准备 + Stage 1～9 服务器主体），每阶段先讲理论（协议/操作系统原理），再给完整可编译代码，最后对照本仓库"参考答案"。最终产物与本仓库代码逐文件对应 |
| **借此熟悉 C++** | 每阶段开头列"本阶段 C++ 知识点"，语法点全部以项目真实代码为例讲解；全部知识点汇总见下方"阶段总表"与"名词速查" |
| **熟悉 cmake/make/gdb** | 工具链循序渐进：Stage 2 学会 g++ 与 make；附录 A 吃透 makefile 并完成 CMake 化；附录 B 用 gdb 全程调试本项目 |

## 使用前必读

### 1. 环境约定

本项目是 **Linux** 项目（epoll / pthread / MySQL 均为 Linux 生态）。

- Windows 用户请先在 WSL2 中安装 Ubuntu（步骤见 [Stage 0](stage-00-environment.md)）。
- Mac 用户请使用 Linux 虚拟机或云服务器（epoll 在 macOS 上不可用）。
- 所有命令默认在 Ubuntu 22.04 下验证。

### 2. 工作区约定：`my_tiny_webserver/`

本仓库是**参考答案**，动手过程中请勿修改它。请在本仓库**同级目录**（而不是仓库内部）创建自己的工作区：

```text
~/projects/
├── TinyWebServer/          # 本仓库 = 参考答案，只读
└── my_tiny_webserver/      # 你的工作区，所有代码写在这里
```

- 每个阶段的代码都写入 `my_tiny_webserver/`，目录结构逐步向仓库靠拢（演进规则见 Stage 2 / 3 / 5）。
- Stage 9 完成后，`my_tiny_webserver/` 的文件与本仓库源码**逐文件对应**。
- 教程中"参考答案对照"一节给出的路径均相对于本仓库根目录（如 `webserver.cpp`、`http/http_conn.cpp`）。

### 3. 学习方法：每个阶段的标准流程

1. **读理论**：先理解本阶段要解决的问题（协议/OS 原理），不求一次全懂；
2. **抄代码**：代码必须自己逐字敲一遍，敲完再编译（抄一遍 >> 看十遍）；
3. **编译运行**：按"编译与运行"一节的命令操作；
4. **过验收清单**：完成"验收清单"里每一行 `- [ ]`，每行都给出**具体命令和预期输出**，全部打勾才算过关；
5. **做思考题**：思考题没有标准答案，用来检验"知其所以然"；
6. **对照参考答案**：翻看仓库对应源码，找出你与参考实现的分歧并理解原因。

### 4. 时间预估（业余时间，每天 2 小时左右）

| 阶段 | 主题 | 预估 |
|---|---|---|
| Stage 0 | 环境准备 | 0.5～1 天 |
| Stage 1 | C++ 快速上手 | 1～2 天 |
| Stage 2 | 阻塞式 echo 服务器 | 1 天 |
| Stage 3 | 多线程与线程池 | 2 天 |
| Stage 4 | epoll 事件驱动 | 2～3 天 |
| Stage 5 | HTTP 服务器 | 3～4 天 |
| Stage 6 | 定时器 | 1～2 天 |
| Stage 7 | 日志系统 | 1～2 天 |
| Stage 8 | 数据库与注册登录 | 2 天 |
| Stage 9 | 整合收官 + 压测 | 2～3 天 |
| 附录 A | make 与 CMake | 0.5～1 天 |
| 附录 B | gdb 调试 | 0.5～1 天 |

**合计约 3～5 周。** 卡住超过半小时就去看该阶段的"常见问题"，再不行直接对照参考答案，不要死磕。

## 阶段总表

| 阶段 | 主题 | 产出的可运行程序 | 核心 C++ 知识点 | 工具链 | 验收要点 | 对应仓库代码 |
|---|---|---|---|---|---|---|
| [0](stage-00-environment.md) | 环境准备 | 第一个 hello world | 编译/链接流程 | g++、gdb 初体验、MySQL | `./a.out` 输出 hello world；gdb 单步一次 | — |
| [1](stage-01-cpp-basics.md) | C++ 快速上手 | 一批迷你练习 | 类、模板、RAII、const、引用、STL、异常 | g++ 多文件编译 | 全部练习编译通过并输出预期 | — |
| [2](stage-02-socket-echo.md) | 第一个服务器 | 阻塞式 echo 服务器 | 指针、struct、C 字符串、socket API | **make 入门** | `nc`/`telnet` 连接后回显输入 | `webserver.cpp` 的 socket 部分 |
| [3](stage-03-threadpool.md) | 多线程与线程池 | 线程池版 echo 服务器 | **类封装、模板类、静态成员、pthread、条件变量、信号量** | gdb 观察线程切换 | 多客户端并发回显互不阻塞 | `lock/locker.h`、`threadpool/threadpool.h` |
| [4](stage-04-epoll.md) | epoll 事件驱动 | epoll 版 echo 服务器 | 枚举、位运算、errno、非阻塞 IO | gdb 断点看事件循环 | ET/LT 两种模式都能正确回显；万级连接不崩溃 | `webserver.cpp` 事件循环骨架 |
| [5](stage-05-http.md) | HTTP 服务器 | 静态文件 HTTP 服务器 | **状态机、枚举、C 字符串函数族、变参函数、mmap、writev、map** | curl 验证、gdb 跟踪状态机 | 浏览器访问返回页面与图片 | `http/http_conn.cpp`、`http/http_conn.h` |
| [6](stage-06-timer.md) | 定时器 | 带超时断开功能的服务器 | **链表、函数指针、static 成员**、信号与管道 | 用 gdb 验证定时器回调 | 空闲连接 15 秒后被自动关闭 | `timer/lst_timer.cpp`、`timer/lst_timer.h` |
| [7](stage-07-log.md) | 日志系统 | 带日志的服务器 | **单例模式、宏、变参、模板类阻塞队列、生产者消费者** | 观察异步写线程 | 日志文件按天/按行切分；异步模式下写线程分离 | `log/log.cpp`、`log/log.h`、`log/block_queue.h` |
| [8](stage-08-mysql.md) | 数据库与注册登录 | 支持注册登录的服务器 | **RAII、单例、list、map、SQL 基础、MySQL C API** | MySQL 客户端验证数据 | 浏览器完成注册→登录全流程 | `CGImysql/sql_connection_pool.*` |
| [9](stage-09-integration.md) | 整合收官 | **完整 TinyWebServer** | 多文件项目组织、getopt、类组合 | **makefile 全解析 + WebBench 压测** | 完整功能验收 + 压测上万并发 | `webserver.*`、`config.*`、`main.cpp`、`makefile` |
| [附录 A](appendix-a-make-cmake.md) | make 与 CMake | 本项目 CMake 化 | 构建系统原理 | **make/CMake 深度** | 用 CMake 构建出同一份 server | `makefile`、`build.sh` |
| [附录 B](appendix-b-gdb.md) | gdb 调试 | 调试本项目 | 调试方法论 | **gdb 深度** | 断点跟踪 HTTP 状态机、多线程调度、core dump | — |

## 阶段依赖关系

```text
Stage 0 ─→ Stage 1 ─→ Stage 2 ─→ Stage 3 ─→ Stage 4 ─→ Stage 5 ─→ Stage 6 ─→ Stage 7 ─→ Stage 8 ─→ Stage 9
                                      (锁/线程池)   (epoll)    (HTTP)    (定时器)  (日志)    (MySQL)   (整合)
```

- Stage 6 / 7 / 8 之间没有强依赖（Stage 7 的日志被 6/8 使用，建议先 6 后 7 再 8，顺序如箭头所示）。
- 附录 A、B 可在 Stage 2 之后随时穿插学习：卡住调试不动时，先去附录 B 学对应章节。

## 名词速查

| 名词 | 一句话解释 | 详细讲解 |
|---|---|---|
| socket | 网络通信的"文件描述符"，读写它就像读写文件 | Stage 2 |
| 阻塞 / 非阻塞 IO | 没数据时 recv 是否"原地等" | Stage 2 / 4 |
| 线程池 | 预先创建一批线程，任务排队被线程领取 | Stage 3 |
| 互斥锁 / 信号量 / 条件变量 | 三种线程同步工具：锁数据、锁资源数量、锁"条件" | Stage 3 |
| RAII | 资源获取即初始化：用对象生命周期自动管理资源 | Stage 1 / 8 |
| epoll | Linux 高效的事件通知机制，一个线程管理成千上万个连接 | Stage 4 |
| ET / LT | 边沿触发 / 水平触发：内核"通知一次"还是"一直催" | Stage 4 |
| Reactor / Proactor | 谁负责读数据：主线程只分发事件 / 主线程替工作线程读好数据 | Stage 4 / 9 |
| HTTP 状态机 | 按"请求行→头部→正文"逐步解析请求 | Stage 5 |
| 长连接 keep-alive | 一次连接复用多次请求 | Stage 5 |
| 定时器 | 超时未活动的连接被服务器踢下线 | Stage 6 |
| 单例模式 | 全局只有一个实例（日志、连接池） | Stage 7 / 8 |
| 生产者-消费者 | 一方放数据、一方取数据，用阻塞队列解耦 | Stage 7 |
| 连接池 | 预先建好 N 条数据库连接，随取随还 | Stage 8 |
| CGI | 服务器执行"程序化逻辑"（本项目指注册/登录处理） | Stage 8 |
| 优雅关闭 (SO_LINGER) | close 时是否等内核把剩余数据发完 | Stage 9 |

## 我的进度打卡表

把这张表抄到自己的笔记里，每完成一个阶段的验收清单就勾掉一行：

```text
[ ] Stage 0  环境准备           —— 能编译运行 hello world、gdb 会单步
[ ] Stage 1  C++ 快速上手       —— 全部练习通过
[ ] Stage 2  echo 服务器        —— nc 回显成功
[ ] Stage 3  线程池             —— 并发回显不互相阻塞
[ ] Stage 4  epoll             —— ET/LT 都正确回显
[ ] Stage 5  HTTP 服务器        —— 浏览器看到页面和图片
[ ] Stage 6  定时器             —— 空闲连接被自动断开
[ ] Stage 7  日志               —— 日志文件出现且可切分
[ ] Stage 8  注册登录           —— 浏览器注册→登录全流程成功
[ ] Stage 9  整合收官           —— 完整功能 + WebBench 压测通过
[ ] 附录 A   make/CMake        —— CMake 构建出同一份 server
[ ] 附录 B   gdb               —— 能独立调试本项目任意问题
```

## 文档导航

- **想快速看整体**：先读本仓库 [LEARNING_GUIDE.md](../LEARNING_GUIDE.md)（模块视角导读 + 面试自测题），再回来按阶段动手。
- **跑通后想精读**：LEARNING_GUIDE 中的"阅读顺序"与面试题是很好的复习材料。
- **环境卡住**：先看 Stage 0 的"常见问题"。
- **调试卡住**：直接翻 [附录 B](appendix-b-gdb.md)。

---

## 致新手的三句话

1. **不要跳阶段**。每个阶段都依赖前面阶段的理解，特别是 Stage 2→3→4 的演进（阻塞 → 线程池 → epoll）是本项目并发模型的主线。
2. **验收清单是硬指标**。"能编译"不等于"学会了"；每一条 `- [ ]` 都勾上才算数。
3. **卡住是正常的**。本项目是很多人的第一个 C++ 项目，遇到段错误、死锁、协议解析错误，正是学习 gdb 和排查方法的最好机会——附录 B 就是为这些时刻准备的。

现在，从 [Stage 0：环境准备](stage-00-environment.md) 开始吧。🚀
