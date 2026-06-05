# Phase 9 —— 性能压测与工程实践

## 目标

对 TinyWebServer 进行系统性的性能分析，包括：
1. 用 WebBench 压测工具测试 QPS
2. 对比 ET/LT 四种组合的性能差异
3. 对比 Reactor/Proactor 的性能差异
4. 分析 `mmap` 的文件传输优势
5. 学会 core dump 分析崩溃问题

**可见结果：** 每种模式下 WebBench 的输出报告，QPS 对比表格，以及对性能瓶颈的分析。

---

## 前置知识

- Phase 0-8 全部模块
- 知道 QPS（每秒查询数）的基本概念

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | Release 构建、`-O2`/`-O3` 优化选项 |
| **gdb** | `core dump` 分析：`ulimit -c`、`bt full` |
| **WebBench** | 编译、压测、解读输出 |
| **perf** | 火焰图采样、CPU 热点分析 |

---

## 分步实现

### Step 1：编译 WebBench

WebBench 是一个简单的 HTTP 压测工具。本项目的 `test_pressure/webbench-1.5/` 目录里有源码。

```bash
cd test_pressure/webbench-1.5
make
# 生成可执行文件 webbench

# 如果报错，编辑 socket.c，将报错行注释或修复
# （常见：getsockname 函数缺少头文件或用宏替代）
```

### Step 2：Release 构建

Release 模式开启编译器优化，性能远高于 Debug：

```cmake
# CMakeLists.txt
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
# -O2：开启大部分优化（内联、循环展开、死代码消除等）
# -DNDEBUG：禁用 assert 宏
```

```bash
cmake -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release -j$(nproc)
```

**为什么 Release 比 Debug 快？**

| 因素 | Debug | Release |
|------|-------|---------|
| 优化级别 | -O0（无优化） | -O2/-O3（激进优化） |
| 内联 | 函数调用开销原样保留 | 小函数直接展开 |
| 变量 | 每个变量在栈上 | 很多变量优化到寄存器 |
| assert | 生效 | 被 DNDEBUG 禁用 |

### Step 3：关闭日志压测

日志写入会严重影响性能。压测时用 `-c 1` 关闭日志：

```bash
# 启动服务器（关闭日志）
./build_release/server -c 1

# 另一个终端运行 WebBench
./test_pressure/webbench-1.5/webbench -c 10500 -t 5 http://127.0.0.1:9006/
```

**参数说明：**

- `-c 10500`：并发 10500 个连接
- `-t 5`：持续 5 秒
- 结果中的 `Speed` 就是 QPS

### Step 4：ET/LT 四种组合对比

用 `-m` 参数切换触发模式：

| `-m` | listenfd | connfd | 命令 |
|------|----------|--------|------|
| 0 | LT | LT | `./server -m 0 -c 1` |
| 1 | LT | ET | `./server -m 1 -c 1` |
| 2 | ET | LT | `./server -m 2 -c 1` |
| 3 | ET | ET | `./server -m 3 -c 1` |

对每种模式跑 3 次，取平均值，填入表格：

| 模式 | 第一次 QPS | 第二次 QPS | 第三次 QPS | 平均 QPS |
|------|-----------|-----------|-----------|----------|
| LT+LT | | | | |
| LT+ET | | | | |
| ET+LT | | | | |
| ET+ET | | | | |

**预期结果（本项目在 Ubuntu 16.04, 关闭日志, Release 构建下的数据）：**

| 模式 | QPS |
|------|------|
| Proactor, LT + LT | ~93251 |
| Proactor, LT + ET | ~97459 |
| Proactor, ET + LT | ~80498 |
| Proactor, ET + ET | ~92167 |
| Reactor, LT + ET | ~69175 |

> ⚠️ **数据说明：** 以上数据来自 Release 构建（`-O2`）+ 关闭日志（`-c 1`）+ 10500 并发连接压测 5 秒。Debug 模式（`-O0 -g`）+ 开启日志时 QPS 会大幅下降（约 1/3 ~ 1/7），属于正常现象。建议压测时统一用 Release 构建。

**分析：** ET 模式比 LT 快，因为内核不会重复通知已就绪的 fd，减少了 `epoll_wait` 的唤醒次数和上下文切换。connfd 用 ET 的收益比 listenfd 大，因为 connfd 数量远多于 listenfd。

### Step 5：Reactor vs Proactor 对比

用 `-a` 参数切换模型：

```bash
# Proactor
./server -m 3 -c 1 -a 0
# Reactor
./server -m 3 -c 1 -a 1
```

**预期结果：** 在当前实现中 Proactor 比 Reactor 快。原因：
- Proactor：主线程完成 IO，工作线程只处理业务。IO 操作集中执行，减少了锁竞争。
- Reactor：每个工作线程自己执行 IO，需要更多的同步开销。
- 本项目的 Reactor 实现中 `dealwithread/write` 有 `while (improv != 1)` 自旋等待，浪费 CPU。

### Step 6：为什么 mmap 比 read 快

传统文件发送需要两次拷贝：

```
磁盘 → (DMA) → 内核缓冲区 → (CPU copy) → 用户缓冲区 → (CPU copy) → socket 缓冲区 → (DMA) → 网卡
```

mmap 将文件直接映射到进程地址空间，减少一次从内核到用户的拷贝：

```
磁盘 → (DMA) → 内核缓冲区 [同时也是用户地址空间的映射] → (CPU copy) → socket 缓冲区 → (DMA) → 网卡
```

如果内核支持 `sendfile`（Linux 2.6.33+），可以实现真正的零拷贝（数据不经过用户态）：

```
磁盘 → (DMA) → 内核缓冲区 → (DMA) → 网卡
```

本项目中 `mmap` + `writev` 的组合也比 `read` + `write` 快，因为：
1. `mmap` 省一次拷贝
2. `writev` 聚集写一次发送（响应头 + 文件内容），减少系统调用

### Step 7：core dump 分析

当程序崩溃（段错误等）时，Linux 可以生成 core dump 文件保存崩溃时的内存快照。

```bash
# 允许生成 core 文件
ulimit -c unlimited

# 设置 core 文件的保存路径
echo "/tmp/core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern

# 运行服务器直到崩溃（如果有 bug 的话）
./build_debug/server

# 用 gdb 分析 core 文件
gdb ./build_debug/server /tmp/core.server.12345

# 在 gdb 内：
(gdb) bt full
# 查看崩溃时的完整调用栈 + 每个栈帧的局部变量

(gdb) info registers
# 查看寄存器状态

(gdb) frame 3
# 跳到第 3 帧

(gdb) print *this
# 打印当前对象的成员
```

**常见崩溃原因：**

| 崩溃信号 | 含义 | 常见原因 |
|---------|------|---------|
| SIGSEGV | 段错误 | 空指针解引用、野指针、数组越界 |
| SIGABRT | abort | assert 失败 |
| SIGFPE | 浮点异常 | 除零 |
| SIGBUS | 总线错误 | 未对齐的内存访问、mmap 失败后访问 |

### Step 8：`strace` 系统调用追踪

```bash
# 统计系统调用次数
strace -c ./build_release/server -c 1 &> /dev/null &
# 用 webbench 压测 5 秒后 kill server

# 输出类似：
# % time     seconds  usecs/call     calls    errors syscall
#  45.23    0.123456         123      5000           epoll_wait
#  30.12    0.082345          82      5000           recvfrom
#  15.34    0.041800          83       500           writev
```

这可以帮助你发现哪些系统调用是瓶颈。如果 `write` / `writev` 占比很高，说明响应发送是瓶颈。

### Step 9：火焰图（CPU 热点分析）

`strace -c` 只能看到系统调用级别的统计，而 **perf** 工具可以采样 CPU 正在执行的代码路径，生成火焰图——直观展示 CPU 时间"烧"在哪里。

```bash
# 安装 perf（Ubuntu）
sudo apt install linux-tools-common linux-tools-generic

# 启动服务器
./build_release/server -c 1

# 另一个终端：采样 10 秒
perf record -p $(pgrep server) -g -- sleep 10
perf report                    # 交互式查看热点
```

**如何看 perf report 输出：**

```
# Samples: 10K of event 'cycles'
# Overhead  Command      Shared Object        Symbol
# ........  ...........  ...................   ....................
    45.23%  server       server                [.] http_conn::process_read
    30.12%  server       [kernel]              [k] epoll_wait
    12.34%  server       server                [.] http_conn::write
     5.67%  server       libc-2.31.so          [.] __memcpy_avx_unaligned
```

**火焰图可视化（更直观）：**

```bash
# 安装 FlameGraph 工具集
git clone https://github.com/brendangregg/FlameGraph
cd FlameGraph

# 生成火焰图 SVG
perf script -i /path/to/perf.data | ./stackcollapse-perf.pl | ./flamegraph.pl > server_flame.svg
# 用浏览器打开 server_flame.svg
```

火焰图中的每个矩形代表一个函数调用栈帧，宽度表示 CPU 采样占比。**越宽的函数越值得优化。** 典型瓶颈模式：

| 火焰图特征 | 含义 | 优化方向 |
|-----------|------|---------|
| `epoll_wait` 顶很宽 | IO 等待型，CPU 空转等待网络 | 增加并发连接数 |
| `process_read` 很宽 | HTTP 解析是瓶颈 | 优化字符串操作、减少拷贝 |
| `__memcpy` 很宽 | 内存拷贝消耗大 | 检查是否用了 mmap/writev |
| 锁相关函数很宽 | 锁竞争激烈 | 减少临界区、改用读写锁 |

### Step 10：性能优化清单

| 优化项 | 效果 | 复杂度 |
|--------|------|--------|
| ET 替代 LT | 减少 epoll 唤醒（~2x QPS） | 低 |
| 关闭日志 | 消除磁盘 IO（~5x QPS） | 低 |
| `-O2` 优化 | 编译器内联、循环优化（~3x QPS） | 零 |
| `mmap` 替代 `read` | 减少内存拷贝 | 低 |
| `writev` 替代多次 `write` | 减少系统调用 | 低 |
| 增加线程池线程数 | 提高并行度（受 CPU 核数限制） | 低 |
| 增加连接池连接数 | 减少等待连接的阻塞 | 低 |
| 避免 `dealwithread` 自旋 | Proactor 模式消除忙等 | 中 |

---

## 压测注意事项

1. **在本地压测本地。** 客户端和服务端在同一台机器，网络延迟可忽略。如果要模拟真实场景，用另一台机器压测。

2. **并发数不要超过文件描述符上限。** `ulimit -n` 查看。默认 1024，需要调大：`ulimit -n 65535`。

3. **端口范围。** 客户端发起连接时也需要临时端口，范围是 `/proc/sys/net/ipv4/ip_local_port_range`。默认 ~28000 个，超过后 `connect` 返回 `EADDRNOTAVAIL`。

4. **WebBench 自身的性能。** 单进程 WebBench 在并发数很高时自身也快成为瓶颈。可以用多进程或多实例。

---

## 验证方法

- [ ] 完成 Release 构建，确认 `-O2` 生效
- [ ] 用 WebBench 测试四种触发组合，记录 QPS 数据
- [ ] 对比 Reactor 和 Proactor 的 QPS
- [ ] 在有/无日志下分别压测，量化日志的性能损耗
- [ ] 学会用 `gdb` 分析 core dump 文件

---

---

## 踩坑记录

1. **WebBench 自身瓶颈。** 单进程 WebBench 在并发 > 20000 时自身 CPU 可能先跑满。如果需要更高并发，可以开多个 WebBench 实例。

2. **端口号耗尽。** 压测时客户端大量 `connect` 会消耗临时端口。查看范围：`cat /proc/sys/net/ipv4/ip_local_port_range`。如果耗尽，`connect` 返回 `EADDRNOTAVAIL`。解决方案：调大范围或启用 `tcp_tw_reuse`。

3. **文件描述符上限。** `ulimit -n` 默认 1024，压测前需调高：`ulimit -n 65535`。否则 `accept` 返回 `EMFILE`。

4. **`TIME_WAIT` 堆积。** 压测结束后大量连接处于 `TIME_WAIT` 状态（`netstat -an | grep TIME_WAIT | wc -l`）。这是正常的 TCP 四次挥手行为，`SO_REUSEADDR` 可以缓解。

## 推荐的面试自测问题

学完整个项目后，你应该能回答以下问题：

1. 为什么 socket 要设置非阻塞？
2. ET 模式下为什么必须循环读到 `EAGAIN`？
3. 连接 fd 为什么用 `EPOLLONESHOT`？
4. Reactor 和 Proactor 在本项目里的区别是什么？
5. `writev` 为什么适合发送 HTTP 响应？
6. 定时器为什么要和连接 fd 绑定？
7. MySQL 连接池为什么用信号量而不是条件变量？
8. 异步日志的阻塞队列解决了什么问题？

---

## 附录：现代 C++ (C++11/14/17) 替代方案

本教程使用 POSIX pthread 系列 API 教学，因为它们在 Linux 上是"底层事实标准"，适合理解操作系统原理。但在现代 C++ 项目中，标准库提供了更安全、更简洁的替代品。

### 对照表

| 本教程方案 | C++ 标准替代 | 优势 |
|-----------|-------------|------|
| `pthread_mutex_t` + `locker` 封装 | `std::mutex` + `std::lock_guard` | 不用手动 unlock，异常安全 |
| `sem_t` | `std::counting_semaphore` (C++20) | 类型安全，RAII 友好 |
| `pthread_cond_t` + `cond` 封装 | `std::condition_variable` | 与 `std::unique_lock` 配合更自然 |
| `pthread_create` + `pthread_detach` | `std::thread` + `detach()` | 面向对象，无需 C 函数指针适配 |
| 手动线程池 (`pthread_t` 数组 + 任务队列) | `std::async` / `std::thread_pool` (C++26 TS) | 返回值通过 `std::future` 获取 |
| `pthread_once` 或手动 double-check | `std::call_once` + `std::once_flag` | 保证只执行一次，无竞态 |
| `volatile` 标志 | `std::atomic<T>` | 正确的内存序语义，防止编译器重排 |

### 迁移示例：locker.h → C++11

**原 pthread 版本（`locker.h`）：**
```cpp
class locker {
    pthread_mutex_t m_mutex;
public:
    locker() { pthread_mutex_init(&m_mutex, NULL); }
    ~locker() { pthread_mutex_destroy(&m_mutex); }
    bool lock() { return pthread_mutex_lock(&m_mutex) == 0; }
    bool unlock() { return pthread_mutex_unlock(&m_mutex) == 0; }
};
```

**C++11 版本：**
```cpp
#include <mutex>
class locker {
    std::mutex m_mutex;
public:
    void lock() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }
    std::mutex& get() { return m_mutex; }
};
// 使用 lock_guard 自动管理锁生命周期
// std::lock_guard<std::mutex> guard(mtx);
```

### 迁移示例：线程池

核心变化：`pthread_create` 传入静态函数指针的黑魔法 → `std::thread` 直接绑定成员函数。

```cpp
// C++11 线程池核心部分
std::vector<std::thread> m_threads;

for (int i = 0; i < thread_number; ++i) {
    m_threads.emplace_back([this]() { this->run(); });
    m_threads.back().detach();
}
```

> **练习建议：** 在理解本教程的 pthread 实现后，尝试用 C++11 标准库重写 `locker.h`、`threadpool.h` 和 `log.cpp`。这会加深你对两种方案的理解。

---

## 阶段小结

**这是最后一个阶段。** 你完成了从环境搭建到性能调优的完整 C++ Web 服务器之旅。

你现在掌握的技能栈：
- C++11 编程（类、模板、RAII、单例）
- Linux 网络编程（socket、epoll、非阻塞 IO、ET/LT）
- 多线程（pthread、互斥锁、信号量、条件变量、线程池）
- HTTP 协议（请求解析、响应构造、状态机）
- 数据库（MySQL C API、连接池）
- 工程工具（cmake、gdb、strace、WebBench）
- 性能分析（QPS、瓶颈定位、编译器优化）

**下一步建议：**
- 阅读《Unix 网络编程》（Stevens）深入理解网络协议栈
- 尝试用 C++11 标准库重构本项目的同步原语和线程池（见附录）
- 将定时器从链表升级为时间轮（timer wheel）
- 支持 HTTPS（用 OpenSSL 或 GnuTLS）

