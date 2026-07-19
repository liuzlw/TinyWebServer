# TinyWebServer 从零复现教程

> 本教程以仓库中的 TinyWebServer（Linux 下 C++ 轻量级 Web 服务器）为蓝本，
> 带你**从零开始、分阶段复现**整个项目，并在过程中系统学习 C++、网络编程与工程工具链。

## 一、教程目标

| 目标 | 说明 |
|------|------|
| 目标 1 | 从零完整复现本项目，理论与实践都能讲清楚 |
| 目标 2 | 以项目为载体熟悉 C++：语法、常用数据结构、项目组织方式 |
| 目标 3 | 熟练使用构建工具（make / CMake）与调试工具（gdb） |

## 二、你需要的前置知识

- C 语言基础（指针、结构体、函数）
- 会用 Linux 基本命令（cd、ls、vim/vscode）
- 不需要任何网络编程经验 —— 教程从 socket 讲起

如果 C++ 语法不熟，没有关系：每个阶段都会列出「本阶段用到的 C++ 知识点」，
遇到问题随时查阅 [附录 C：C++ 知识点索引](appendix-cpp-map.md)。

## 三、学习环境

本项目依赖 Linux 专有 API（epoll、pthread、mysqlclient），**不能在 Windows 原生环境编译**。
你的电脑已经具备完整环境：

- **WSL2 + Ubuntu 24.04**（已安装 g++ 13.3 / make / CMake 3.28 / gdb）
- 所有教程命令都在 **WSL 的 Ubuntu 终端**中执行
- 项目代码在 WSL 中通过 `/mnt/c/Users/liuzl/Documents/projects/TinyWebServer` 访问

> 💡 建议用 VS Code 安装 **WSL 扩展**，在 WSL 里打开项目目录，获得代码补全与调试支持。

Stage 0 会带你逐一验证环境。

## 四、成品项目长什么样

复现完成后，你的服务器将具备：

- 线程池 + 非阻塞 socket + epoll（LT/ET 均实现）+ Reactor/Proactor 两种事件处理模式
- 状态机解析 HTTP 请求，支持 GET / POST
- MySQL 数据库连接池，实现 Web 端注册、登录
- 同步 / 异步日志系统
- 升序链表定时器，自动踢掉非活跃连接
- 经 Webbench 压测可支撑上万并发连接

## 五、学习路线图

教程共 **10 个阶段（Stage 0-9）+ 3 个附录**。每个阶段都有：

```
🎯 本阶段目标      —— 做完后你拥有什么
📚 理论铺垫        —— 必须懂的背景知识（配最小示例）
💻 C++ 知识点      —— 本阶段新出现的语法/数据结构
🔨 动手实现        —— 一步一步写代码（写进 my_tiny_webserver/）
✅ 验证            —— 明确的、可检查的验收标准
🐛 常见问题        —— 这个阶段最容易踩的坑
🤔 思考与练习      —— 巩固与拓展
```

### 阶段总览

| 阶段 | 主题 | 产出（可验证结果） | 对应源码模块 |
|------|------|--------------------|--------------|
| [Stage 0](stage-00-environment.md) | 环境与工具链 | g++/make/CMake/gdb/MySQL 全部可用，编译运行第一个 CMake 工程 | — |
| [Stage 1](stage-01-tcp-echo.md) | socket 与 TCP | 一个 TCP echo 服务器，telnet/nc 连上能回显 | — |
| [Stage 2](stage-02-simple-http.md) | HTTP 协议 | 单线程 HTTP 服务器，浏览器能打开静态页面 | `http/`（雏形） |
| [Stage 3](stage-03-threadpool.md) | 线程同步与线程池 | 多线程服务器，可同时服务多个客户端 | `lock/`、`threadpool/` |
| [Stage 4](stage-04-epoll.md) | I/O 多路复用 | epoll 事件驱动服务器，理解 LT/ET、Reactor | `webserver.cpp`（雏形） |
| [Stage 5](stage-05-http-parser.md) | HTTP 状态机 | 完整 HTTP 解析 + mmap 发文件 + writev + keep-alive | `http/http_conn.*` |
| [Stage 6](stage-06-timer.md) | 定时器 | 非活跃连接 15 秒无操作被自动断开 | `timer/` |
| [Stage 7](stage-07-log.md) | 日志系统 | 同步/异步日志，运行状态写入日志文件 | `log/` |
| [Stage 8](stage-08-mysql.md) | 数据库连接池 | 浏览器完成注册、登录、访问图片视频 | `CGImysql/` |
| [Stage 9](stage-09-integration.md) | 整合与压测 | 完整复现！命令行参数齐全，webbench 压测上万并发 | `config.*`、`webserver.*`、`main.cpp` |

### 为什么是这个顺序？

整体原则是：**每次只引入一个新概念，每一步都能独立运行验证**。

1. **Stage 1-2（网络基础）**：先用最简单的方式跑通「网络收发数据」和「浏览器访问」，
   建立信心，也理解后面所有优化到底在解决什么问题。
2. **Stage 3-4（并发核心）**：引入线程池和 epoll —— 这是高性能服务器的两条腿。
3. **Stage 5（协议解析）**：把 Stage 2 的玩具 HTTP 处理升级为工业级状态机解析。
4. **Stage 6-8（周边系统）**：定时器、日志、数据库是「锦上添花」的支撑模块，
   逐个挂载到主干上。
5. **Stage 9（收官）**：用 Config + WebServer 把所有模块装配成最终形态，压测验收。

### 两个目录的分工

```
TinyWebServer/            ← 原始完整代码（参考答案，卡住了就看）
├── http/  lock/  log/  timer/  threadpool/  CGImysql/
├── webserver.cpp  config.cpp  main.cpp  makefile
└── my_tiny_webserver/    ← 你的战场！教程所有代码写在这里
    ├── http/  lock/  log/  timer/  threadpool/  CGImysql/  root/
    └── CMakeLists.txt    ← 从 Stage 0 开始逐步完善
```

> ⚠️ **重要建议**：每个阶段先自己写，写不出来再看参考答案。
> 「看懂」和「能写出来」之间隔着的就是真正的学习。
> 原始代码就是你的标准答案 —— 复现完成后，用 `diff` 对比你的版本和原版，是很好的复盘。

## 六、附录（随时查阅）

- [附录 A：Makefile 与 CMake 详解](appendix-cmake-make.md) —— 构建工具从入门到够用
- [附录 B：GDB 调试实战](appendix-gdb.md) —— 断点、多线程调试、core dump 分析
- [附录 C：C++ 知识点索引](appendix-cpp-map.md) —— 项目里每处语法/数据结构出现在哪、去哪学

## 七、学习节奏建议

- 每个 Stage 预计 2-6 小时，不必一天一个，**理解透了再往下走**
- 每个 Stage 结尾的「🤔 思考与练习」尽量做，那是面试常问的点
- 卡住超过 30 分钟：看参考答案对应文件 → 用 gdb 跟一遍 → 再看理论
- 学完全部阶段后，建议再读一遍仓库根目录的 [LEARNING_GUIDE.md](../LEARNING_GUIDE.md) 做总复盘

准备好了吗？从 [Stage 0：环境与工具链](stage-00-environment.md) 开始。
