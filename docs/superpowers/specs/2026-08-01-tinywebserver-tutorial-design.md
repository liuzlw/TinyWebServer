# TinyWebServer C++ 入门教程设计规格

日期：2026-08-01
状态：已获用户批准

## 背景与目标

用户以本仓库（qinguoyi/TinyWebServer）作为入门 C++ 的学习项目，三个目标：

1. **从零完整复现项目**——理论与实践上都能理解
2. **以项目为基础熟悉 C++**——语法、数据结构、项目组织结构
3. **熟悉构建工具与调试**——cmake、make、gdb

用户环境：Windows 11 + WSL2 Ubuntu 24.04（g++ 13.3 / cmake 3.28 / gdb 已装，项目经 `/mnt/c/...` 访问）。复现代码写在 `my_tiny_webserver/`（目前为空骨架），原仓库代码作为"参考答案"。

## 失败教训（为什么前两次 Kimi 教程被删除）

用户明确反馈四个失败原因，本教程必须逐一规避：

1. **太泛泛、不可执行** → 每阶段给出可直接照做的步骤
2. **步骤没有可验证结果** → 每阶段有明确的验收清单（操作 + 预期输出）
3. **没有贴真实代码** → 每阶段有完整可编译的真实代码
4. **组织不合理** → 统一的文档模板 + 清晰的阶段路线

## 已确认的设计决策

| 决策点 | 选择 | 含义 |
|---|---|---|
| 代码形式 | 完整代码 + 讲解 | 每阶段贴完整可编译代码，用户理解后在 `my_tiny_webserver/` 自己敲一遍实现"复现" |
| C++ 讲解 | 前置 C++ 基础章 | Part 1 集中 6 章，只讲项目用得上的概念，每章标注"给第 X 阶段铺路" |
| MySQL | 全程必需 | Stage 0 就装好 MySQL 并建库建表；S8 实现注册登录 |
| 构建工具 | CMake 为主 + make 附录 | 复现代码用 CMake 从零上手；附录 A1 解读原项目 makefile |
| gdb | 前置入门 + 自然融入 | 环境阶段教 gdb 基本操作，之后每阶段"调试技巧"自然使用 |
| 整体骨架 | 混合式 | S1~S2 快速用单线程跑通"浏览器看到网页"，之后按原项目模块顺序递增 |

## 教程结构

```
tutorial/
  README.md                 # 导航、学习路线图、如何使用本教程
  stage-00-environment.md   # Part 0 环境准备
  cpp-01-syntax.md          # Part 1 C++ 基础（6 章）
  cpp-02-class.md
  cpp-03-raii.md
  cpp-04-template.md
  cpp-05-thread.md
  cpp-06-modern.md
  stage-01-tcp-echo.md      # Part 2 服务器阶段（9 阶段）
  stage-02-http-static.md
  stage-03-threadpool.md
  stage-04-epoll.md
  stage-05-http-state-machine.md
  stage-06-timer.md
  stage-07-log.md
  stage-08-mysql.md
  stage-09-integration.md
  annex-makefile.md         # 附录 3 篇
  annex-gdb-cheatsheet.md
  annex-reference-map.md
```

## 阶段清单

### Part 0 环境

**Stage 0 环境准备**
- WSL2 环境检查（g++/cmake/gdb/mysql/git 版本确认）
- 用 CMake 构建第一个 hello 程序，理解构建流程
- gdb 入门：断点、单步、看变量（前置入门）
- MySQL 安装 + 建库建表（数据库 `qgydb`、表 `user`）
- 验收：hello 可运行、gdb 可调试、`mysql -u root -p` 能查到建好的表

### Part 1 C++ 基础（6 章）

每章结构：概念讲解 → 可运行小程序 → 练习 → 验收，标注铺路目标。

| 章节 | 主题 | 铺路给 |
|---|---|---|
| C1 语法速览 | 变量/类型/控制流/函数/指针/引用/const | 全部 |
| C2 类与对象 | 构造/析构/this/static/const 成员、std::string、std::vector | 全部类 |
| C3 RAII | 栈对象/析构/智能指针 | locker、SQL 连接池 |
| C4 模板 | 函数模板/类模板 | threadpool、block_queue |
| C5 线程与同步 | thread/mutex/condition_variable/信号量 | 并发部分 |
| C6 C++11 特性 | nullptr/auto/range-for/lambda/function | 全项目 |

### Part 2 服务器（9 阶段）

| 阶段 | 主题 | 关键成果 | 验收要点 |
|---|---|---|---|
| S1 | 单线程 TCP echo | socket/bind/listen/accept 全流程 | telnet 发消息回显 |
| S2 | 单线程 HTTP 静态 | 解析请求、返回静态文件 | 浏览器/curl 看到页面 |
| S3 | 锁 + 线程池 | locker.h + threadpool.h | 并发连接、观察线程行为 |
| S4 | epoll 事件循环 | 非阻塞 + epoll、LT/ET、Reactor/Proactor | gdb 观察 eventLoop、多连接并发 |
| S5 | 完整 HTTP 状态机 | http_conn：主从状态机、writev、GET/POST、keep-alive | curl POST、keep-alive、报文观察 |
| S6 | 定时器 | 升序链表清理非活动连接 | 空闲连接被关闭（日志/gdb 观察） |
| S7 | 日志系统 | 同步/异步、生产者消费者 | tail -f 实时日志 |
| S8 | MySQL + 注册登录 | sql_connection_pool（RAII+单例）、CGI | mysql 查询到注册记录、登录跳转 |
| S9 | 整合 + 压测 | main/config/webserver 装配、webbench | 压测 QPS、全功能回归 |

## 每篇文档统一模板

```
1. 本阶段目标（一句话 + 最终效果）
2. 前置知识回链（用到 C 章节/API 在哪）
3. 完整代码（带行号，逐段讲解）
4. 编译与运行（CMake 命令 + 本阶段结束的目录结构）
5. 验收清单（可操作的验证步骤 + 预期输出）
6. 常见坑（编译/运行报错对照）
7. 与原项目对照（参考答案在哪、怎么 diff）
```

## 验收策略总则

- 每个阶段（含 C++ 章节）都必须有"可操作验证步骤 + 预期输出"
- 验证手段优先使用环境已有工具：浏览器、curl、telnet/nc、gdb、tail -f、mysql CLI、webbench
- S9 做全功能回归，覆盖前面所有阶段的验收点

## 交付方式（3 批）

| 批次 | 内容 | 交付后可验证 |
|---|---|---|
| 第 1 批 | README + Stage 0 + C1 + C2 | 环境跑通、hello 调试、基础类/string/vector 练习通过 |
| 第 2 批 | C3~C6 + S1~S3 | 模板/线程/RAII 练习 + echo 服务器 + 线程池并发 |
| 第 3 批 | S4~S9 + 附录 | 完整 TinyWebServer + 压测 + makefile 解读 |

## 写作用例约定

- 全中文写作
- 代码风格与项目保持一致（C++11，贴近原项目命名）
- 复现代码统一放 `my_tiny_webserver/`，随阶段累积增长，最终结构与原项目一致（与已建的空骨架目录对齐：lock/threadpool/http/timer/log/CGImysql/root）
- 每阶段结束时当前工程必须能独立编译运行：各阶段更新 `my_tiny_webserver/CMakeLists.txt` 指向当前入口（这也是 CMake 的教学点），验收不依赖后续阶段
- 每个阶段结束打一个 git 提交作为里程碑，方便回退对照；S1/S2 单文件阶段直接放工程根目录，后续阶段按原项目结构落位
