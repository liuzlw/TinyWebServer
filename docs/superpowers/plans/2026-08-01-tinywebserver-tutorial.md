# TinyWebServer C++ 入门教程写作计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 C++ 初学者写出一份从零复现 TinyWebServer 的分阶段教程,每阶段含完整可编译代码、逐步讲解与可验证的验收清单。

**Architecture:** 教程分三部分(环境 / 前置 C++ 基础 6 章 / 服务器 9 阶段)+ 3 篇附录,按规格中的 3 批交付。每篇服务器阶段文档遵循统一模板,代码贴近原仓库(参考答案),构建用 CMake。复现代码随阶段累积在 `my_tiny_webserver/`。

**Tech Stack:** Markdown 文档;C++11;CMake(3.28);g++(13.3);gdb;MySQL 8;webbench(压测,Stage 9 使用);环境为 WSL2 Ubuntu 24.04。

**规格文件:** `docs/superpowers/specs/2026-08-01-tinywebserver-tutorial-design.md`

## Global Constraints

- **全中文写作**,面向"入门 C++、零网络编程经验"的读者;教程文档统一放 `tutorial/`
- **每篇文档必须有"验收清单"小节**:可操作的验证命令 + 预期输出,不能只描述概念
- **代码 C++11**,命名风格与原项目一致;所有文档中的代码必须能在 WSL2 直接编译运行
- **复现代码放 `my_tiny_webserver/`**,随阶段累积;每阶段更新 `my_tiny_webserver/CMakeLists.txt` 指向当前入口
- **端口约定 9006**;MySQL 库 `qgydb`、用户 `root/root`、表 `user`
- **原仓库代码是参考答案**,照抄模块时来源路径必须写进文档的"与原项目对照"小节
- 每篇服务器阶段文档遵循统一模板:目标 → 前置知识回链 → 完整代码 → 编译与运行 → 验收清单 → 常见坑 → 与原项目对照
- 每个任务结束打 git 提交

---

## 批次 1:README + Stage 0 + C1 + C2

### Task 1: 教程导航 README

**Files:**
- Create: `tutorial/README.md`

**文档结构(必须包含):**
1. **教程是什么**——一句话 + 三个学习目标(复现项目 / 熟悉 C++ / 熟悉 cmake·make·gdb)
2. **学习路线图**——ASCII 图,展示:环境 → C++ 基础(6 章)→ 服务器(9 阶段,标出"第 2 阶段即可在浏览器看到网页"这个里程碑)→ 附录
3. **目录**——三部分 + 附录的表格,每项一行简介
4. **如何使用本教程**——阅读 + 在 `my_tiny_webserver/` 手敲代码 + 每阶段跑验收清单 + git 提交里程碑
5. **分 3 批的推荐节奏**——每批结束时可验证的成果(照抄规格"交付方式"表格)
6. **环境要求**——WSL2、g++/cmake/gdb/mysql 版本说明

- [ ] **Step 1: 创建文件** `tutorial/README.md`,按上述结构写完整内容
- [ ] **Step 2: 自查**——每个目录条目都能对应到后续任务将要创建的文件
- [ ] **Step 3: 提交**

```bash
git add tutorial/README.md
git commit -m "教程:添加导航 README 与学习路线图"
```

---

### Task 2: Stage 0 环境准备

**Files:**
- Create: `tutorial/stage-00-environment.md`
- Create (示例, 读者照做): `my_tiny_webserver/hello/CMakeLists.txt`、`my_tiny_webserver/hello/main.cpp` —— 这是给读者的演示工程,写入文档的代码块中,不要求实际创建

**文档结构(必须包含):**
1. **目标**——环境全部跑通:能编译调试第一个程序,MySQL 可用
2. **前置知识**——无(这是起点)
3. **WSL2 与工具链检查**——逐条命令 + 预期版本输出(要求 >= g++ 11、cmake 3.20、gdb、mysql --version;若版本不符给出安装命令)
4. **第一个 CMake 工程**——完整贴出 `hello/main.cpp` 与 `hello/CMakeLists.txt`,逐步讲解 CMake 最小写法(cmake_minimum_required / project / add_executable),然后给编译运行命令(期望输出 `Hello TinyWebServer!`)
5. **gdb 入门**——用 hello 程序演示:断点、run、next、print 变量、quit;每条 gdb 交互给"输入→预期输出"
6. **MySQL 安装与建库建表**——apt 安装 + 启动服务 + 创建库 `qgydb`、表 `user`(表结构与原仓库 `sql_connection_pool.cpp` 中查询的字段一致:username、passwd,见原仓库 `CGImysql/README.md`),给出 mysql 命令与预期输出
7. **验收清单**——5 条:hello 编译运行成功 / gdb 能打断点看变量 / mysql 客户端能连上 / 建库成功 / 建表成功(mysql 查询表结构)
8. **常见坑**——WSL2 中文路径问题、`/mnt/c` 文件权限、mysql 服务未启动

**嵌入代码:** hello 程序的 `main.cpp` 与 `CMakeLists.txt`(完整代码,约 20 行)。

- [ ] **Step 1: 创建** `tutorial/stage-00-environment.md`
- [ ] **Step 2: 自查**——每条验收命令都有明确预期输出;MySQL 建表语句与 S8 用到的字段一致
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-00-environment.md
git commit -m "教程:Stage 0 环境准备(工具链/CMake hello/gdb/MySQL)"
```

---

### Task 3: C++ 基础 C1 语法速览

**Files:**
- Create: `tutorial/cpp-01-syntax.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——明确"这是给全部服务器阶段的语法地基"
2. **变量与类型**——基本类型、const、auto
3. **控制流**——if/for/while/switch
4. **函数**——传值 vs 传引用 vs 传指针,默认参数
5. **指针与引用**——区别、空指针、nullptr
6. **示例程序**——一个综合小程序(结构体 + 函数 + 指针/引用参数),完整代码 + 预期输出
7. **练习**——2 个练习题(带答案)
8. **验收清单**——示例程序编译运行输出正确;练习答案能编译

**嵌入代码:** 示例程序完整代码(约 40 行)、练习代码与答案(约 20 行)。

- [ ] **Step 1: 创建** `tutorial/cpp-01-syntax.md`
- [ ] **Step 2: 自查**——示例程序可以在 WSL2 用 g++ 直接编译;验收有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-01-syntax.md
git commit -m "教程:C++ 基础 C1 语法速览"
```

---

### Task 4: C++ 基础 C2 类与对象

**Files:**
- Create: `tutorial/cpp-02-class.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——"项目里几乎所有模块都是类:config/webserver/http_conn/log..."
2. **类的定义与封装**——public/private、成员函数
3. **构造函数与析构函数**——默认构造、带参构造、初始化列表
4. **this 指针、static 成员、const 成员函数**——每个概念配 2~3 行代码示例
5. **std::string 与 std::vector**——常用操作(string 拼接/查找、vector push_back/遍历)
6. **示例程序**——一个 `Student`/`Counter` 类小程序,展示构造/析构/static 计数/string 与 vector,完整代码 + 预期输出
7. **练习**——2 个练习题(带答案)
8. **验收清单**——示例程序运行输出正确;练习编译通过

**嵌入代码:** 示例程序完整代码(约 60 行)、练习代码与答案。

- [ ] **Step 1: 创建** `tutorial/cpp-02-class.md`
- [ ] **Step 2: 自查**——示例程序编译运行有预期输出;代码里用到的语法在 C1 讲过
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-02-class.md
git commit -m "教程:C++ 基础 C2 类与对象"
```

**批次 1 完成标记**:README + Stage 0 + C1 + C2 四篇就绪,读者验收通过后进入批次 2。

---

## 批次 2:C3~C6 + S1~S3

### Task 5: C++ 基础 C3 RAII

**Files:**
- Create: `tutorial/cpp-03-raii.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——"RAII 是项目里 locker.h(锁封装)和 SQL 连接池的核心思想"
2. **栈对象生命周期**——作用域结束时析构自动执行
3. **析构函数与资源释放**——new/delete 与类封装
4. **智能指针**——std::unique_ptr / std::shared_ptr,`#include <memory>`
5. **示例程序**——一个 `FileGuard`/`Resource` 类演示析构自动释放,再加 shared_ptr 引用计数示例,完整代码 + 预期输出
6. **练习**——2 个练习题(带答案)
7. **验收清单**——示例程序输出显示析构按顺序触发;练习编译通过

**嵌入代码:** 示例程序完整代码(约 50 行)、练习代码与答案。

- [ ] **Step 1: 创建** `tutorial/cpp-03-raii.md`
- [ ] **Step 2: 自查**——析构顺序的预期输出与代码实际行为一致
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-03-raii.md
git commit -m "教程:C++ 基础 C3 RAII 资源管理"
```

---

### Task 6: C++ 基础 C4 模板

**Files:**
- Create: `tutorial/cpp-04-template.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——"threadpool.h 和 block_queue.h 都是类模板,这是下一批服务器阶段的核心"
2. **函数模板**——`template<typename T>` 写法、实例化
3. **类模板**——成员函数实现、模板与 .h 文件(为什么模板实现放头文件)
4. **示例程序**——一个 `MyQueue<T>` 迷你模板类(push/pop/empty),完整代码 + 预期输出
5. **练习**——1 个练习:为模板类写一个 `max` 函数模板(带答案)
6. **验收清单**——示例程序运行输出正确;练习编译通过

**嵌入代码:** 模板示例完整代码(约 50 行)、练习与答案。

- [ ] **Step 1: 创建** `tutorial/cpp-04-template.md`
- [ ] **Step 2: 自查**——`MyQueue` 用 int 和 string 两个类型实例化都能运行
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-04-template.md
git commit -m "教程:C++ 基础 C4 模板与泛型"
```

---

### Task 7: C++ 基础 C5 线程与同步

**Files:**
- Create: `tutorial/cpp-05-thread.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——"线程池、日志系统、信号处理都会用到;项目自己封装了 locker.h,理解底层再学封装"
2. **std::thread**——创建线程、join、传参
3. **数据竞争与 std::mutex**——`lock_guard`、临界区
4. **std::condition_variable**——wait/notify 模式(生产者消费者雏形)
5. **信号量**——POSIX `sem_t`(sem_init/sem_wait/sem_post),这是原项目 locker.h 的信号量封装用的接口
6. **示例程序**——两个程序:① 两个线程用 mutex 累加计数器,展示正确输出;② 生产者消费者用 condition_variable,完整代码 + 预期输出
7. **练习**——1 个练习(带答案)
8. **验收清单**——两个示例程序输出正确(计数器=预期值、消费者取完所有元素);练习编译通过

**嵌入代码:** 两个示例程序完整代码(共约 100 行)、练习与答案。

- [ ] **Step 1: 创建** `tutorial/cpp-05-thread.md`
- [ ] **Step 2: 自查**——多线程示例加了 `-lpthread` 编译选项说明;输出可预期
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-05-thread.md
git commit -m "教程:C++ 基础 C5 线程与同步"
```

---

### Task 8: C++ 基础 C6 现代特性

**Files:**
- Create: `tutorial/cpp-06-modern.md`

**文档结构(必须包含):**
1. **本课目标 + 铺路说明**——"项目代码里到处是这些 C++11 写法"
2. **nullptr / auto / 范围 for**
3. **lambda 与 std::function**
4. **右值引用与移动(简述)**——只讲 std::move 的"是什么、什么时候用",不深挖
5. **示例程序**——综合小程序用上上述特性,完整代码 + 预期输出
6. **练习**——1 个练习(带答案)
7. **验收清单**——示例程序运行输出正确;练习编译通过

**嵌入代码:** 示例程序完整代码(约 50 行)、练习与答案。

- [ ] **Step 1: 创建** `tutorial/cpp-06-modern.md`
- [ ] **Step 2: 自查**——示例程序能在 C++11 标准下编译(不需要更高标准)
- [ ] **Step 3: 提交**

```bash
git add tutorial/cpp-06-modern.md
git commit -m "教程:C++ 基础 C6 现代特性"
```

---

### Task 9: Stage 1 单线程 TCP echo 服务器

**Files:**
- Create: `tutorial/stage-01-tcp-echo.md`

**文档结构(必须包含):**
1. **本阶段目标**——写出第一个网络程序:单线程 echo 服务器,客户端发什么回什么
2. **前置知识回链**——C1(指针)、C5 无;新增:Linux 网络编程基础概念
3. **socket 编程全流程讲解**——socket / bind / listen / accept / read / write / close 逐个 API 讲参数与返回,配一张连接流程图(ASCII)
4. **完整代码**——单文件 `main.cpp`(约 80 行),逐段讲解
5. **编译与运行**——先展示原始 g++ 一行命令(`g++ -o server main.cpp -lpthread`)并解释 `-lpthread`,然后建立 `my_tiny_webserver/CMakeLists.txt` 用 CMake 构建(本阶段结束的目录结构:根目录有 `main.cpp`、`CMakeLists.txt`、`root/`),两条路径都给命令 + 预期输出
6. **验收清单**——① `nc 127.0.0.1 9006` 或 telnet 连接后输入文字立即回显;② 按 Ctrl+C 服务端退出
7. **调试技巧**——用 gdb 在 `accept` 处打断点,观察阻塞;给出 gdb 交互"输入→预期输出"
8. **常见坑**——端口被占用(bind 失败)、telnet/nc 未安装、服务端地址填错
9. **与原项目对照**——本阶段是最简版,原项目用 epoll+线程池做了同样的事,完整对照表在附录 A3;说明这阶段的代码会在后续阶段演化

**嵌入代码:** echo 服务器 `main.cpp` 完整代码(约 80 行)。

- [ ] **Step 1: 创建** `tutorial/stage-01-tcp-echo.md`
- [ ] **Step 2: 自查**——echo 代码在 WSL2 用 `g++ -o server main.cpp -lpthread` 能编译;验收命令有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-01-tcp-echo.md
git commit -m "教程:Stage 1 单线程 TCP echo 服务器"
```

---

### Task 10: Stage 2 单线程 HTTP 静态服务器

**Files:**
- Create: `tutorial/stage-02-http-static.md`

**文档结构(必须包含):**
1. **本阶段目标**——在浏览器看到网页(这是本教程第一个大里程碑)
2. **前置知识回链**——C2(string)、S1(socket);新增:HTTP 协议基础(请求行/请求头/响应格式,给出一个 curl -v 示例报文)
3. **完整代码**——单文件 `main.cpp`(约 130 行):解析请求行与 Host 头、按 URL 读 `root/` 下的静态文件、拼 HTTP 响应(状态行+头+body),逐段讲解
4. **编译与运行**——更新 `my_tiny_webserver/CMakeLists.txt`(替换 main.cpp 源),CMake 构建 + `./server` 启动,说明工作目录要含 `root/`
5. **验收清单**——① `curl -v http://127.0.0.1:9006/` 看到 `HTTP/1.1 200 OK` 与 `root/index.html`(可先用仓库现成的 `root/welcome.html` 演示)内容;② 浏览器访问 `http://127.0.0.1:9006/welcome.html` 看到页面;③ 访问不存在的文件返回 404
6. **调试技巧**——用 gdb 在解析请求行的函数处打断点,print 读到的字符串
7. **常见坑**——没设工作目录找不到 root 文件、多行请求粘包(单线程阻塞处理已能覆盖大部分场景)、二进制文件(jpg)乱码问题说明(本阶段可只支持文本,二进制留到 S5 writev)
8. **与原项目对照**——原项目 `http/http_conn.cpp` 的 `do_request` 做了同样的 URL→文件映射;本阶段最简实现,完整状态机在 S5

**嵌入代码:** HTTP 静态服务器 `main.cpp` 完整代码(约 130 行)。

- [ ] **Step 1: 创建** `tutorial/stage-02-http-static.md`
- [ ] **Step 2: 自查**——HTTP 响应格式符合 HTTP/1.1 规范(状态行/头/空行/body);验收命令有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-02-http-static.md
git commit -m "教程:Stage 2 单线程 HTTP 静态服务器"
```

---

### Task 11: Stage 3 锁与线程池

**Files:**
- Create: `tutorial/stage-03-threadpool.md`

**文档结构(必须包含):**
1. **本阶段目标**——支持多连接并发处理;引入线程池
2. **前置知识回链**——C4(模板)、C5(线程/同步);说明为什么 S2 单线程只能服务一个连接
3. **lock/locker.h**——完整代码(照抄原仓库 `lock/locker.h`,它是 C5 学的 mutex/cond/sem 的 RAII 封装),逐段讲解
4. **threadpool/threadpool.h**——完整代码(照抄原仓库 `threadpool/threadpool.h`,类模板,半同步半反应堆模型),重点讲:模板参数、任务队列、工作线程循环、`append()` 入队
5. **改造 S2 用线程池**——给出新的 `main.cpp`(约 120 行):accept 后把连接封装成任务交给线程池;说明线程池负责 read/解析/应答
6. **编译与运行**——更新 `CMakeLists.txt` 加入 `lock/` 与 `threadpool/` 源文件,CMake 构建 + 启动
7. **验收清单**——① 两个终端同时 `nc` 连接,两边都能独立收发(并行而非排队);② 进程内线程数符合线程池大小(用 `ps -T` 或日志观察)
8. **调试技巧**——gdb 在 worker 线程的函数打断点,`info threads` 查看线程,`thread N` 切换
9. **常见坑**——任务对象生命周期(连接 fd 复用)、线程池析构时线程退出、锁死锁
10. **与原项目对照**——`lock/locker.h`、`threadpool/threadpool.h` 直接对应原仓库同名文件,给出 diff 对比命令(`diff my_tiny_webserver/threadpool/threadpool.h threadpool/threadpool.h`)

**嵌入代码:** `locker.h`、`threadpool.h`、改造后 `main.cpp` 完整代码。

- [ ] **Step 1: 创建** `tutorial/stage-03-threadpool.md`
- [ ] **Step 2: 自查**——threadpool.h 与 `diff` 命令指向的原仓库文件一致;验收的"并行而非排队"描述准确
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-03-threadpool.md
git commit -m "教程:Stage 3 锁与线程池"
```

**批次 2 完成标记**:C3~C6 + S1~S3 就绪,读者拿到 echo 服务器与并发线程池,验收通过后进入批次 3。

---

## 批次 3:S4~S9 + 附录

### Task 12: Stage 4 epoll 事件循环

**Files:**
- Create: `tutorial/stage-04-epoll.md`

**文档结构(必须包含):**
1. **本阶段目标**——从"阻塞 + 每连接一线程"升级为"事件驱动 + 非阻塞";这是高并发的关键
2. **前置知识回链**——S3(线程池)、C5;新增:非阻塞 socket、I/O 多路复用概念、LT vs ET
3. **epoll 三件套**——epoll_create / epoll_ctl / epoll_wait 逐个 API 讲解,配状态图(ASCI: 连接进来→注册→等待事件→分发)
4. **完整代码**——单文件 `main.cpp`(约 150 行):listenfd 非阻塞、accept 新连接注册 EPOLLIN、就绪连接由工作线程(沿用 S3 线程池)处理,读取循环到 `EAGAIN`(ET)或用 LT;逐段讲解
5. **Reactor 与 Proactor**——用代码 + 流程图说明本项目两种模型的差异(谁负责读:主线程读 = 模拟 Proactor,工作线程读 = Reactor)
6. **编译与运行**——更新 `CMakeLists.txt`,CMake 构建 + 启动
7. **验收清单**——① 浏览器/curl 仍能访问(功能回归);② 两个终端并发 curl,能同时响应;③ 用 gdb 在 `epoll_wait` 返回处打断点,观察返回的事件数
8. **调试技巧**——`strace -f -e epoll_wait` 观察系统调用(可选);gdb 观察 EPOLLIN/EPOLLOUT
9. **常见坑**——忘记非阻塞导致 ET 卡死、漏注册 EPOLLONESHOT(说明本项目会用到,详述在 S5)、读不完整
10. **与原项目对照**——原仓库 `webserver.cpp` 的 `eventListen`(epoll 初始化)、`eventLoop`、`dealwithread`/`dealwithwrite`;给出对应文件路径

**嵌入代码:** epoll 版 `main.cpp` 完整代码(约 150 行)。

- [ ] **Step 1: 创建** `tutorial/stage-04-epoll.md`
- [ ] **Step 2: 自查**——ET 分支的循环读取逻辑正确(while read 到 EAGAIN);验收命令有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-04-epoll.md
git commit -m "教程:Stage 4 epoll 事件循环"
```

---

### Task 13: Stage 5 完整 HTTP 状态机

**Files:**
- Create: `tutorial/stage-05-http-state-machine.md`

**文档结构(必须包含):**
1. **本阶段目标**——实现健壮的 HTTP 解析(状态机)与高效响应(writev),支持 GET/POST、keep-alive
2. **前置知识回链**——S2(HTTP 基础)、S4(epoll);说明 S2 的"一次性读+简单解析"在长连接下会出错
3. **状态机设计**——主状态机(解析哪部分)与从状态机(读行),配 ASCII 状态转移图
4. **http/http_conn.h + http_conn.cpp**——完整代码(照抄原仓库 `http/http_conn.h`、`http/http_conn.cpp`),逐段讲解:枚举 HTTP_CODE、`parse_*` 函数、`process_read`/`process_write`、`read_once`(LT/ET 两种)、`write` 用 `writev` 发头+文件、`EPOLLONESHOT`
5. **接入 epoll 循环**——给出现有 `main.cpp` 需要怎么改(连接对象数组、状态迁移),给出关键改动代码(约 60 行)
6. **编译与运行**——更新 `CMakeLists.txt`(加入 `http/` 源文件),CMake 构建 + 启动
7. **验收清单**——① `curl -v http://127.0.0.1:9006/` 报文头正确;② POST 一个表单数据(给 curl 命令),服务器能解析 body(可用日志/打印验证);③ keep-alive:连续两次 curl 到同一连接(`curl --keepalive` 或脚本)不报错;④ 请求不存在的文件返回 404
8. **调试技巧**——gdb 在 `parse_line` 打断点单步,观察状态机状态变量(主状态从 0 走到 3)
9. **常见坑**——读半包(ET 未读完)、请求头超长、writev 与散列表、文件大小超过 mmap 能力
10. **与原项目对照**——直接对应原仓库 `http/` 目录,给出 `diff` 命令

**嵌入代码:** `http_conn.h`、`http_conn.cpp`、接入改动代码(照抄自原仓库)。

- [ ] **Step 1: 创建** `tutorial/stage-05-http-state-machine.md`
- [ ] **Step 2: 自查**——照抄代码与原仓库 `http/` 一致(diff 只应有注释/命名微调);验收命令有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-05-http-state-machine.md
git commit -m "教程:Stage 5 完整 HTTP 状态机"
```

---

### Task 14: Stage 6 定时器

**Files:**
- Create: `tutorial/stage-06-timer.md`

**文档结构(必须包含):**
1. **本阶段目标**——定期清理非活动连接,防止连接泄漏
2. **前置知识回链**——S4(SIGALRM 信号)、C5;新增:信号处理、`setitimer`/`alarm`、管道通知机制
3. **升序链表定时器**——`timer/lst_timer.h + lst_timer.cpp` 完整代码(照抄原仓库),讲:定时器结点/链表 add/adjust/del/tick
4. **信号转 IO 事件**——为什么不用信号回调而用 `sigaction` + 管道,把 SIGALRM 变成 `read` 可读的普通事件
5. **接入主循环**——`main.cpp` 关键改动代码(约 60 行):注册 SIGALRM、每 3 秒一次 `tick` 检查、连接关闭时调整定时器
6. **编译与运行**——更新 `CMakeLists.txt`(加入 `timer/` 源文件),CMake 构建 + 启动
7. **验收清单**——① 连接后不发送任何数据,等待超过超时时间(用日志/打印观察 tick 打印),连接被关闭(客户端 read 返回 0 或 reset);② 正常收发数据的连接不被误关
8. **调试技巧**——gdb 在 `tick` 打断点,`print` 链表头结点超时值
9. **常见坑**——信号与主循环竞争、定时器结点与连接生命周期绑定、alarm 覆盖
10. **与原项目对照**——原仓库 `timer/` 目录,给出 `diff` 命令

**嵌入代码:** `lst_timer.h`、`lst_timer.cpp`、接入改动代码(照抄自原仓库)。

- [ ] **Step 1: 创建** `tutorial/stage-06-timer.md`
- [ ] **Step 2: 自查**——照抄代码与原仓库 `timer/` 一致;验收的"空闲连接被关闭"步骤可操作
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-06-timer.md
git commit -m "教程:Stage 6 定时器清理非活动连接"
```

---

### Task 15: Stage 7 日志系统

**Files:**
- Create: `tutorial/stage-07-log.md`

**文档结构(必须包含):**
1. **本阶段目标**——把服务器运行状态写进日志,支持同步/异步两种模式
2. **前置知识回链**——C5(生产者消费者)、C4(模板);新增:阻塞队列概念
3. **log/block_queue.h**——完整代码(照抄原仓库),讲:模板环形队列、push/pop 的锁与条件变量
4. **log/log.h + log.cpp**——完整代码(照抄原仓库),讲:单例、日志分级、同步直写 vs 异步(后台线程从队列取)、刷新时机
5. **接入主循环**——`main.cpp` 关键改动代码(约 30 行):初始化日志、在关键路径打日志
6. **编译与运行**——更新 `CMakeLists.txt`(加入 `log/` 源文件),CMake 构建 + 启动
7. **验收清单**——① 启动后 `ServerLog`/`log/` 目录生成日志文件(给路径);② `tail -f 日志文件` 实时看到访问日志;③ 对比同步/异步模式(启动参数或宏),功能一致
8. **调试技巧**——gdb 在 `write_log` 打断点,观察格式;异步模式在写线程打断点
9. **常见坑**——日志文件打不开、目录不存在、异步队列满阻塞、单例初始化顺序
10. **与原项目对照**——原仓库 `log/` 目录,给出 `diff` 命令

**嵌入代码:** `block_queue.h`、`log.h`、`log.cpp`、接入改动代码(照抄自原仓库)。

- [ ] **Step 1: 创建** `tutorial/stage-07-log.md`
- [ ] **Step 2: 自查**——照抄代码与原仓库 `log/` 一致;验收的日志路径与代码里的路径常量一致
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-07-log.md
git commit -m "教程:Stage 7 同步/异步日志系统"
```

---

### Task 16: Stage 8 MySQL 连接池与注册登录

**Files:**
- Create: `tutorial/stage-08-mysql.md`

**文档结构(必须包含):**
1. **本阶段目标**——数据库连接池 + 实现注册/登录功能(项目"完整功能"的最后一块)
2. **前置知识回链**——C3(RAII)、Stage 0(建库建表)、C5(锁);新增:MySQL C API(mysql_init/mysql_real_connect/mysql_query)
3. **CGImysql/sql_connection_pool.h + .cpp**——完整代码(照抄原仓库),讲:单例、连接池取/还、RAII 包装自动归还(连接守护类)
4. **注册/登录逻辑**——在 http_conn 的 `do_request` 里接上数据库(照抄原仓库 `http_conn.cpp` 的注册登录分支),讲:GET 注册页/POST 提交/查表校验
5. **接入**——`main.cpp` 关键改动代码(约 30 行):初始化连接池;编译加 `-lmysqlclient`
6. **编译与运行**——更新 `CMakeLists.txt`(加入 `CGImysql/` 源文件并 `target_link_libraries` 链接 `mysqlclient`),CMake 构建 + 启动
7. **验收清单**——① 浏览器访问注册页,提交新用户名密码,页面提示注册成功;② `mysql -u root -proot qgydb -e "SELECT * FROM user"` 查到刚注册的记录;③ 用该账号登录成功跳转;④ 重复注册同名用户报错
8. **调试技巧**——gdb 在 `do_request` 的注册分支打断点,`print` 解析出的用户名密码
9. **常见坑**——连接池初始化失败(库名/密码错)、mysql 头文件/库未装、URL 编码问题
10. **与原项目对照**——原仓库 `CGImysql/` 与 `http_conn.cpp` 注册登录部分,给出 `diff` 命令

**嵌入代码:** `sql_connection_pool.h`、`sql_connection_pool.cpp`、`http_conn.cpp` 注册登录改动、接入代码(照抄自原仓库)。

- [ ] **Step 1: 创建** `tutorial/stage-08-mysql.md`
- [ ] **Step 2: 自查**——照抄代码与原仓库 `CGImysql/` 一致;验收的 mysql 查询命令与表名/字段一致
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-08-mysql.md
git commit -m "教程:Stage 8 MySQL 连接池与注册登录"
```

---

### Task 17: Stage 9 整合与压力测试

**Files:**
- Create: `tutorial/stage-09-integration.md`

**文档结构(必须包含):**
1. **本阶段目标**——把各模块组装成完整项目(对齐原仓库),并能做压力测试
2. **前置知识回链**——全部 S1~S8;新增:命令行参数解析、`config` 配置类
3. **整合装配**——`main.cpp`(约 40 行)、`config.h + config.cpp`(约 130 行)、`webserver.h + webserver.cpp`(约 500 行),完整代码(照抄原仓库),讲清 `init → log_write → sql_pool → thread_pool → trig_mode → eventListen → eventLoop` 装配顺序
4. **完整 CMake 工程**——完整 `CMakeLists.txt`(替代 makefile):收集各模块 .cpp、链接 `pthread` 与 `mysqlclient`;同时给出原仓库 makefile 的等价对照
5. **启动与参数**——`./server` 常用参数说明(端口/日志开关/触发模式/线程数/actor 模型),对应原仓库 `-p -l -m -s -t -a`
6. **webbench 压测**——安装/编译 webbench(仓库 `test_pressure/` 里没有源码,给出 `git clone` + 编译命令或说明),压测命令 + 预期输出格式(QPS 行)
7. **验收清单**——① 全功能回归:S2 静态页面、S5 POST/keep-alive、S8 注册登录逐一复测(给出命令表);② `webbench -c 1000 -t 5 http://127.0.0.1:9006/` 能跑完输出 QPS(数值不设硬指标,只要跑完不崩溃);③ gdb 能 attach 到运行中的 server 打断点
8. **常见坑**——参数忘传、mysqlclient 链接顺序、webbench 在 WSL2 的兼容性、fd 数上限(`ulimit -n` 调大)
9. **与原项目对照**——本阶段代码应与原仓库主目录文件基本一致,给出 `diff` 命令

**嵌入代码:** `main.cpp`、`config.h/cpp`、`webserver.h/cpp`、完整 `CMakeLists.txt`(照抄自原仓库 + CMake 版本)。

- [ ] **Step 1: 创建** `tutorial/stage-09-integration.md`
- [ ] **Step 2: 自查**——CMakeLists 链接顺序(源文件后 `-lpthread -lmysqlclient`)正确;验收命令有预期输出
- [ ] **Step 3: 提交**

```bash
git add tutorial/stage-09-integration.md
git commit -m "教程:Stage 9 整合与压力测试"
```

---

### Task 18: 附录 A1 makefile 解读 + A2 gdb 速查

**Files:**
- Create: `tutorial/annex-makefile.md`
- Create: `tutorial/annex-gdb-cheatsheet.md`

**annex-makefile.md 必须包含:**
1. 原仓库 `makefile`(15 行)逐行注释解读:变量 `CXX`/`DEBUG`/`CXXFLAGS`、目标依赖、`-g` vs `-O2`、`$^` 自动变量、`-lpthread -lmysqlclient`
2. make 基础:目标/依赖/伪目标、增量编译原理(为什么 makefile 只写一次、Makefile 与时间戳)
3. CMake 与 make 的关系:本教程用 CMake 生成构建,最终对比原项目 makefile 干的事

**annex-gdb-cheatsheet.md 必须包含:**
1. 表格:常用命令(break/run/next/step/print/backtrace/info threads/thread/attach/list/finish/continue/watch/finish/quit)
2. 三个实战小例子(对应 S1 accept、S4 epoll、S9 压测时 attach)
3. 常见报错(gdb 提示 no symbol table → 编译忘加 `-g`)

- [ ] **Step 1: 创建两个附录文件**
- [ ] **Step 2: 自查**——makefile 注释基于仓库真实文件逐行;gdb 命令在 WSL2 有效
- [ ] **Step 3: 提交**

```bash
git add tutorial/annex-makefile.md tutorial/annex-gdb-cheatsheet.md
git commit -m "教程:附录 makefile 解读与 gdb 速查"
```

---

### Task 19: 附录 A3 对照表

**Files:**
- Create: `tutorial/annex-reference-map.md`

**文档结构(必须包含):**
1. **对照总表**——每阶段 ↔ 原仓库文件 ↔ 本教程成果文件 三列大表
2. **学习路径回溯**——每阶段在整条学习线上的位置与依赖
3. **进阶阅读**——《Unix环境高级编程》《Unix网络编程》对应章节提示(与 README 原项目作者推荐一致)

- [ ] **Step 1: 创建** `tutorial/annex-reference-map.md`
- [ ] **Step 2: 自查**——表中每个原仓库路径真实存在(对照仓库目录逐一核对)
- [ ] **Step 3: 提交**

```bash
git add tutorial/annex-reference-map.md
git commit -m "教程:附录 阶段与原项目文件对照表"
```

**批次 3 完成标记**:全部 19 篇文档就绪,教程交付完成。

---

## 整体自查(写完全部任务后)

1. **规格覆盖**——规格里每项决策都有对应任务:完整代码+讲解(T9~T17 全部)、前置C++章(T3~T8)、MySQL全程必需(T2 建库 + T16 功能)、CMake为主+make附录(T17/T18)、gdb前置+融入(T2 入门 + 各阶段调试技巧 + A2)、混合骨架(T9~T10 快速跑通 + T11~T17 模块递增)
2. **占位符扫描**——所有任务都有具体代码来源(新代码直接嵌入或注明"照抄原仓库 X 文件");无"待定/以后再说"
3. **类型/命名一致性**——S9 的 CMakeLists 源文件列表与 T2 约定的 `my_tiny_webserver/` 结构一致;MySQL 库/表名在 T2、T16、T17 全文一致(`qgydb`/`user`/`root`/`root`);端口 9006 全文一致
