# Phase 9 —— 性能压测与工程实践

## 本阶段目标

对 TinyWebServer 进行系统性的性能分析，包括 WebBench 压力测试、ET/LT 组合对比、Reactor/Proactor 对比、以及生产环境的工程实践建议。

**可见结果：** 服务器在 WebBench 压测下达到上万的 QPS，并生成性能分析报告。

**验收标准：**

- [ ] WebBench 编译成功，能对服务器发起压力测试
- [ ] 关闭日志后，LT+LT 模式下 QPS > 50000
- [ ] ET+ET 模式下 QPS 与 LT+LT 对比有可见差异
- [ ] 能解释为什么 ET 模式在高并发下通常更快

---

## 理论与机制

### 1. WebBench 工作原理

WebBench 是最简单的 HTTP 压力测试工具——它模拟大量并发客户端向服务器发送请求：

```
WebBench 主进程
  │
  ├── fork() → 子进程 1 → 创建 socket → 连接服务器 → 发送 GET /
  ├── fork() → 子进程 2 → 创建 socket → 连接服务器 → 发送 GET /
  ├── fork() → 子进程 3 → ...
  └── ...

每个子进程持续发送请求，统计成功/失败数、速度（bytes/sec）、QPS
```

**关键指标：**
- **QPS（Queries Per Second）**：每秒处理的请求数
- **Speed**：每秒传输的字节数
- **Failed**：失败的请求数（应为 0）
- **并发连接数**：同时活跃的客户端数量

### 2. LT vs ET 的性能差异根源

**为什么 ET 通常更快？**

```
LT 模式：
  epoll_wait → 就绪 fd1 → 读 1024 字节 → 还有数据 → epoll_wait 再次返回 fd1
  → 再读 1024 字节 → ... → epoll_wait 被多次唤醒

ET 模式：
  epoll_wait → 就绪 fd1 → 循环读直到 EAGAIN → epoll_wait 返回一次
  → epoll_wait 唤醒次数更少
```

**epoll_wait 是有开销的**（用户态↔内核态切换）。ET 减少了这种切换次数 → 更高吞吐。

**但 ET 的代价是代码复杂度更高**（必须循环读/写，必须非阻塞，必须处理 EAGAIN）。

### 3. Proactor vs Reactor 性能差异

**Proactor（默认）** 在 QPS 上通常更高：

```
Proactor:  主线程 IO + 工作线程计算 → IO 集中 → 更好的 CPU 缓存局部性
Reactor:   工作线程 IO + 计算 → IO 分散 → 缓存抖动
```

**但 Reactor 在特定场景有优势：**
- 当业务逻辑很简单时（如 echo），主线程 IO 成为瓶颈
- Reactor 让工作线程分担 IO，减少了主线程的负担

---

## 实现指南

### Step 1：编译 WebBench

```bash
cd test_pressure/webbench-1.5
make

# 如果报错，可能需要修改 socket.c
# 将报错行的内容注释掉即可（本项目已修复）
```

### Step 2：基准测试（LT + LT，Proactor，关闭日志）

```bash
# 启动服务器（关闭日志以减少 IO 干扰）
./server -p 9006 -c 1

# 另一个终端运行压测
./webbench -c 1000 -t 5 http://127.0.0.1:9006/
```

**解释参数：**
- `-c 1000`：1000 个并发客户端
- `-t 5`：测试持续 5 秒

**预期输出：**
```
Webbench - Simple Web Benchmark 1.5
Copyright (c) Radim Kolar 1997-2004, GPL Open Source Software.

Benchmarking: GET http://127.0.0.1:9006/
1000 clients, running 5 sec.

Speed=XXXXXX pages/min, XXXXXXX bytes/sec.
Requests: XXXXX susceed, 0 failed.
```

### Step 3：四种 ET/LT 组合测试

| -m 参数 | listenfd | connfd | 说明 |
|---------|----------|--------|------|
| 0 | LT | LT | 最简单，最稳妥 |
| 1 | LT | ET | connfd 用 ET，减少可读事件触发 |
| 2 | ET | LT | listenfd 用 ET，减少 accept 触发 |
| 3 | ET | ET | 全部 ET，理论上最优 |

```bash
# 依次测试四种组合
./server -p 9006 -m 0 -c 1 -a 0  # LT + LT (Proactor)
./server -p 9006 -m 1 -c 1 -a 0  # LT + ET
./server -p 9006 -m 2 -c 1 -a 0  # ET + LT
./server -p 9006 -m 3 -c 1 -a 0  # ET + ET

# 记录每次的 QPS 和 Speed
```

### Step 4：Reactor vs Proactor 对比

```bash
# Proactor
./server -p 9006 -a 0 -c 1
./webbench -c 1000 -t 5 http://127.0.0.1:9006/

# Reactor
./server -p 9006 -a 1 -c 1
./webbench -c 1000 -t 5 http://127.0.0.1:9006/
```

### Step 5：core dump 分析

当程序崩溃（Segmentation Fault）时，Linux 可以生成 core dump 文件供事后分析：

```bash
# 启用 core dump（无限制大小）
ulimit -c unlimited

# 运行服务器直到崩溃
./server -p 9006

# 崩溃后会生成 core 文件
ls -la core*

# 用 gdb 分析 core dump
gdb ./server core
(gdb) backtrace        # 崩溃时的调用栈
(gdb) info registers   # 崩溃时的寄存器状态
(gdb) frame 0          # 查看崩溃帧的详细信息
(gdb) print var        # 查看变量（如果符号存在）
```

### Step 6：perf 火焰图（可选，进阶）

```bash
# 安装 perf
sudo apt install linux-tools-common linux-tools-generic

# 采样（CPU 热点分析）
sudo perf record -g ./server -p 9006 -c 1 &
# 在另一个终端运行压测
./webbench -c 100 -t 30 http://127.0.0.1:9006/
# 压测结束后 Ctrl+C 停止 server

# 生成报告
sudo perf report

# 生成火焰图（需要 FlameGraph 脚本）
sudo perf script > out.perf
git clone https://github.com/brendangregg/FlameGraph.git
./FlameGraph/stackcollapse-perf.pl out.perf > out.folded
./FlameGraph/flamegraph.pl out.folded > flamegraph.svg
```

---

## 验证用例与预期结果

### 测试 1：WebBench 基准测试

```bash
./server -p 9006 -c 1
./webbench -c 1000 -t 5 http://127.0.0.1:9006/
```

**预期：**
- Failed: 0
- QPS > 10000 (具体取决于机器性能)

### 测试 2：高并发

```bash
./server -p 9006 -c 1
./webbench -c 5000 -t 5 http://127.0.0.1:9006/
```

**预期：** 所有请求成功。服务器不崩溃。

### 测试 3：ET/LT 对比

记录四种 `-m` 组合的 QPS，确认 ET+ET 通常最高。

### 失败排查

| 症状 | 可能原因 |
|------|---------|
| webbench 报 `Connect failed` | 服务器端口未监听或已满 |
| webbench 报 `Fork failed` | 并发客户端太多，超过系统限制。`ulimit -u 65536` |
| 压测中服务器崩溃 | 文件描述符上限。`ulimit -n 65536` |
| 压测中部分请求失败 | 连接池/线程池不够。增加 `-s`/`-t` 参数 |

---

## 📖 附录：C++11 现代替代方案

本项目的代码使用了 C 风格的 pthread API。在生产环境中，C++11 提供了更安全、更简洁的替代方案：

| 项目中的实现 | C++11 替代 | 说明 |
|-------------|-----------|------|
| `pthread_mutex_t` + `locker` 类 | `std::mutex` + `std::lock_guard` | 自动加锁/解锁，更安全 |
| `sem_t` + `sem` 类 | `std::counting_semaphore` (C++20) | 标准库信号量 |
| `pthread_cond_t` + `cond` 类 | `std::condition_variable` | 配合 `std::mutex` 使用 |
| `pthread_t` + `pthread_create` | `std::thread` | RAII 线程管理 |
| `void*` 传参 | `std::function` + lambda | 类型安全，可捕获上下文 |
| `pthread_detach` | `std::thread::detach()` | 成员函数 |
| `new[]` / `delete[]` | `std::vector` / `std::unique_ptr<T[]>` | 自动内存管理 |

**示例对比：**

```cpp
// === C 风格（本项目） ===
pthread_mutex_t mutex;
pthread_mutex_init(&mutex, NULL);
pthread_mutex_lock(&mutex);
// ... critical section ...
pthread_mutex_unlock(&mutex);
pthread_mutex_destroy(&mutex);

// === C++11 风格 ===
std::mutex mtx;
{
    std::lock_guard<std::mutex> lock(mtx);
    // ... critical section ...
    // 自动解锁！
}
```

---

## 阶段小结

你完成了 TinyWebServer 的最终阶段：

- ✅ WebBench 编译和使用
- ✅ ET/LT 四种组合性能对比
- ✅ Reactor/Proactor 模式对比
- ✅ core dump 分析方法
- ✅ C++11 现代替代方案了解

🎉 **恭喜！** 你已经从头实现了一个完整的 Linux C++ Web 服务器，并掌握了从环境搭建到性能分析的全流程工程技能。

---

## 🏆 全部阶段完成后的能力清单

完成 Phase 0-9 后，你应该能：

**编码能力：**
- 用 C++ 写出线程安全的类（RAII、模板、单例）
- 理解并能手写生产者-消费者模型
- 实现 HTTP/1.1 协议的状态机解析器

**系统编程：**
- 使用 Linux socket API 编写网络程序
- 使用 epoll 实现事件驱动的服务器
- 理解 LT/ET/EAGAIN/EPOLLONESHOT 的底层含义
- 使用 mmap/writev 实现零拷贝优化

**并发编程：**
- 实现通用线程池（半同步/半反应堆模式）
- 理解 Proactor 和 Reactor 的区别及适用场景
- 使用信号量 + 互斥锁实现资源池

**数据库：**
- 使用 MySQL C API 进行查询操作
- 设计并实现数据库连接池
- 理解 RAII 在资源管理中的应用

**工程工具：**
- 从 g++ 裸命令到 Makefile 到 CMakeLists.txt 的完整构建链
- gdb 调试：断点、单步、watch、条件断点、attach、core dump
- 使用 WebBench 进行压力测试
