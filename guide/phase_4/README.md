# Phase 4 —— 线程池

## 目标

实现一个**模板化的通用线程池**，支持两种并发模型（Proactor 和 Reactor），并能将工作线程与数据库连接池对接。

**可见结果：** 提交一批独立任务到线程池，线程并行完成，每个线程从连接池自动获取/归还 MySQL 连接。

---

## 前置知识

- Phase 1 的 `locker.h`
- Phase 3 的 MySQL 连接池
- C++ 模板基础（`template <typename T>`）
- pthread 的 `pthread_create` 和 `pthread_detach`

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | `add_subdirectory`（多目录项目结构） |
| **gdb** | 查看所有线程栈、死锁定位（`info threads` + `bt full`） |

---

## 分步实现

### Step 1：理解 Reactor 和 Proactor

这是本项目最核心的两个概念，在此先建立直觉：

**Proactor（本项目默认模式，-a 0）：**

```
主线程           工作线程
  │                │
  ├─ read(fd) ──→  │  主线程读完数据
  │                │
  │  把已读数据 ──→ ├─ process()  工作线程只处理
  │                ├─ write()    不碰 socket IO
  │                │
```

主线程负责所有 IO，工作线程只处理业务逻辑。

**Reactor（-a 1）：**

```
主线程           工作线程
  │                │
  ├─ 通知"fd可读" ─→ ├─ read(fd)   工作线程自己读
  │                ├─ process()
  │                ├─ write(fd)  工作线程自己写
  │                │
```

主线程只负责事件分发（通知"哪个 fd 有事件"），工作线程自己读写 socket。

在本项目中，两种模式的差异体现在 `threadpool::run()` 的代码分支和 `WebServer::dealwithread/write()` 的实现中。

### Step 2：线程池模板类

```cpp
// threadpool.h
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool {
public:
    threadpool(int actor_model, connection_pool* connPool,
               int thread_number = 8, int max_request = 10000);
    ~threadpool();

    // Reactor 模式：提交任务时指定 state（0=读，1=写）
    bool append(T* request, int state);

    // Proactor 模式：任务已就绪，只提交给线程处理
    bool append_p(T* request);

private:
    static void* worker(void* arg);
    void run();

    int       m_thread_number;
    int       m_max_requests;
    pthread_t* m_threads;
    std::list<T*> m_workqueue;   // 任务队列
    locker    m_queuelocker;     // 保护任务队列的锁
    sem       m_queuestat;       // 任务信号量：>0 表示有任务待处理
    connection_pool* m_connPool;
    int       m_actor_model;
};
```

### Step 3：构造函数与析构函数

```cpp
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool* connPool,
                          int thread_number, int max_requests)
    : m_actor_model(actor_model), m_thread_number(thread_number),
      m_max_requests(max_requests), m_threads(NULL),
      m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) throw std::exception();

    for (int i = 0; i < thread_number; ++i) {
        // 创建线程，线程函数是 worker，参数是 this
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        // detach：线程结束后自动回收资源，不需要 join
        if (pthread_detach(m_threads[i])) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
}
```

**为什么用 `detach` 而不是 `join`？** 线程池的工作线程永远运行（while(true)），只有进程结束时它们才终止。`detach` 后不需要主线程显式等待。

### Step 4：producer 端（主线程添加任务）

```cpp
// Reactor 模式：主线程投递任务 + 状态标记
template <typename T>
bool threadpool<T>::append(T* request, int state) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;     // 0=读任务, 1=写任务
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();           // 唤醒一个等待的 worker
    return true;
}

// Proactor 模式：只投递，不区分状态
template <typename T>
bool threadpool<T>::append_p(T* request) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
```

### Step 5：consumer 端（工作线程执行任务）

```cpp
template <typename T>
void* threadpool<T>::worker(void* arg) {
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while (true) {
        m_queuestat.wait();          // 等待任务
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (!request) continue;

        if (1 == m_actor_model) {
            // --- Reactor 模式 ---
            if (0 == request->m_state) {
                // 读任务：工作线程自己读 socket
                if (request->read_once()) {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;  // 标记需要清理
                }
            } else {
                // 写任务：工作线程自己写 socket
                if (request->write()) {
                    request->improv = 1;
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        } else {
            // --- Proactor 模式 ---
            // 主线程已完成 IO，工作线程只需处理业务
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
```

**关键细节：**

1. `m_queuestat.wait()` ——工作线程不空转，没有任务时就休眠（信号量阻塞）。
2. `request->improv = 1` ——通知主线程任务已完成（配合 `WebServer::dealwithread` 中的 `while (users[sockfd].improv != 1)` 自旋等待）。
3. `connectionRAII` ——每个任务从连接池获取连接，`process()` 执行完后自动归还。

### Step 6：多目录 cmake 项目

```
project/
├── CMakeLists.txt          # 顶层
├── lock/
│   └── locker.h
├── log/
│   ├── log.h
│   ├── log.cpp
│   └── block_queue.h
├── CGImysql/
│   ├── sql_connection_pool.h
│   └── sql_connection_pool.cpp
├── threadpool/
│   ├── threadpool.h
│   └── CMakeLists.txt      # 线程池子目录
└── main.cpp
```

**顶层 `CMakeLists.txt`：**

```cmake
cmake_minimum_required(VERSION 3.10)
project(TinyWebServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

add_subdirectory(threadpool)   # 引入子目录

add_executable(server main.cpp)
target_link_libraries(server threadpool_lib pthread mysqlclient)
```

**`threadpool/CMakeLists.txt`：**

```cmake
add_library(threadpool_lib INTERFACE)
target_include_directories(threadpool_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
```

> 因为 `threadpool.h` 是纯头文件模板，不需要编译，所以用 `INTERFACE` 库。

### Step 7：测试线程池

```cpp
// test_threadpool.cpp
#include <iostream>
#include <unistd.h>
#include "threadpool/threadpool.h"

// 一个简单的任务类型
struct Task {
    int id;
    int m_state;    // 必须（线程池用）
    int improv;     // 必须
    int timer_flag; // 必须
    MYSQL* mysql;   // 必须（数据库连接）

    void process() {
        std::cout << "Task " << id << " processing..." << std::endl;
        sleep(1);  // 模拟耗时处理
        std::cout << "Task " << id << " done!" << std::endl;
    }
};

int main() {
    // 测试用，传 NULL 连接池（本例不访问数据库）
    threadpool<Task> pool(0, NULL, 4, 100);
    sleep(1);  // 等待线程全部启动

    const int N = 10;
    Task tasks[N];
    for (int i = 0; i < N; ++i) {
        tasks[i].id = i;
        tasks[i].mysql = NULL;
        pool.append_p(&tasks[i]);  // Proactor 模式提交
    }

    sleep(5);  // 等待所有任务完成
    std::cout << "All tasks submitted." << std::endl;
    return 0;
}
```

运行后你会看到 10 个任务被 4 个线程并行处理。

### Step 8：gdb 多线程调试

```bash
gdb ./build/test_threadpool

(gdb) break threadpool<Task>::run
(gdb) run
# 停在线程池的 run 函数

(gdb) info threads
#   1  Thread ...  (running)
# * 2  Thread ...  threadpool::run() at ...
#   3  Thread ...  threadpool::run() at ...
#   4  Thread ...  ...

(gdb) thread 2
(gdb) bt full
# 查看线程 2 的完整调用栈和局部变量
```

**检测死锁：** 当程序卡住不动时

```bash
(gdb) thread apply all bt
# 如果多个线程都停在 pthread_mutex_lock 或 sem_wait，
# 说明大概率发生了死锁。
```

---

## 验证方法

- [ ] 提交 N 个任务到 M 线程的池中，全部处理完成
- [ ] 任务数和线程数不同时（如 10 任务 / 4 线程），观察负载分布
- [ ] 用 gdb 的 `info threads` 确认创建了正确数量的线程
- [ ] 模拟任务抛异常，确认不会导致线程退出

---

## 踩坑记录

1. **`pthread_detach` 的时机。** 线程一创建就 detach 是安全的，因为 `run()` 一开始就进入 `while(true)` 循环。但如果线程函数可能立即退出，detach 后访问已销毁对象会导致未定义行为。

2. **`m_queuestat.wait()` 之前已经检查过队列？** wait 返回后仍要检查 `m_workqueue.empty()`，因为存在虚假唤醒。同时多个线程可能被 `broadcast` 唤醒，但只有一个能拿到任务。

3. **任务对象的生命周期。** `append` 和 `append_p` 接收的是指针。调用方必须保证任务对象在线程处理期间有效。这就是为什么项目中 `users` 数组是全局预分配的而不是动态 new。

4. **worker 函数是 static。** 因为 `pthread_create` 要求 C 函数指针。`static void* worker(void* arg)` 通过 `arg` 把 `this` 传入，再调 `pool->run()`——这是 C++ 多线程编程的经典模式。

---

## 阶段小结

你实现了一个完整的模板线程池，包含：
- 信号量驱动的工作队列（有任务就干活，没任务就睡）
- 互斥锁保护任务队列的并发操作
- Proactor 和 Reactor 两种并发模型的代码路径

现在，HTTP 处理的并发执行已经有了基础设施。下一阶段：**定时器**——清理那些超时不活跃的连接。
