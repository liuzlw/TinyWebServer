# Phase 4 —— 线程池

## 本阶段目标

实现一个**C++ 模板化的通用线程池**，支持两种并发模型（Proactor 和 Reactor），工作线程自动从数据库连接池获取/归还连接。

**可见结果：** 提交任务到线程池，工作线程并行执行，每个线程从连接池获得自己的数据库连接。

**验收标准：**

- [ ] 线程池创建指定数量的工作线程
- [ ] 任务提交后能被工作线程取出并执行
- [ ] 并发提交多个任务，线程并行处理
- [ ] Proactor 模式：主线程读数据，工作线程处理业务
- [ ] Reactor 模式：工作线程自己读数据 + 处理业务

---

## 理论与机制

### 1. 为什么需要线程池？

如果每个请求都创建一个新线程：

```
请求1 → pthread_create → 处理 → pthread_exit
请求2 → pthread_create → 处理 → pthread_exit
...
```

**问题：**
- 线程创建/销毁有开销（分配栈空间、内核数据结构）
- 线程数不受控（10000 个请求 = 10000 个线程 → 上下文切换开销巨大）
- 每个线程需要独立栈（默认 8MB → 10000 线程 = 80GB 虚拟内存）

**线程池 = 预创建固定数量的线程 + 任务队列：**

```
         ┌─────────────┐
请求 →   │  任务队列    │
         │ [t1][t2][t3]│
         └──┬──┬──┬───┘
            │  │  │
        ┌───▼──▼──▼───┐
        │  线程池       │
        │ [W1][W2][W3] │  ← 预创建的线程，持续运行
        └──────────────┘
```

线程创建一次，持续运行。任务通过队列投递。线程数固定（通常 = CPU 核数）。

### 2. 半同步/半反应堆模式

这是本项目线程池的设计模式名称：

```
  主线程（同步）                    工作线程（反应堆）
  ────────────                    ────────────
  等待事件发生                    等待任务队列有东西
  ↓                              ↓
  epoll_wait 返回                信号量 wait 返回
  ↓                              ↓
  读取数据/接收连接               锁住任务队列
  ↓                              ↓
  构造任务                       取出一个任务
  ↓                              ↓
  投递到任务队列                 释放锁
  ↓                              ↓
  继续 epoll_wait                处理任务（逻辑计算）
                                 ↓
                                 继续等待任务队列
```

"半同步" = 主线程用 epoll_wait 同步等待 IO 事件
"半反应堆" = 工作线程对任务队列"反应"（队列有东西就处理）

### 3. Proactor vs Reactor：本项目的实现

```
【Proactor 模式】(actor_model = 0，默认)
═══════════════════════════════════════════
主线程：
  read_once()  ← 主线程读完所有数据
  append_p()   ← 把"已读好数据"的请求投递到队列

工作线程：
  run() → process() → process_read() + process_write()
  ← 工作线程只做"纯计算"：解析 HTTP + 生成响应

【Reactor 模式】(actor_model = 1)
═══════════════════════════════════════
主线程：
  append(users+sockfd, 0)  ← 只投递"需要读"的事件

工作线程：
  run() → read_once()  ← 工作线程自己读数据
       → process()     ← 解析 + 生成响应
```

**线程池中的体现（`run()` 函数）：**

```cpp
void run() {
    while (true) {
        m_queuestat.wait();    // 等待任务
        // ... 从队列取任务 ...

        if (1 == m_actor_model) {      // Reactor
            if (0 == request->m_state) {  // 读任务
                if (request->read_once()) {  // 工作线程自己读
                    request->process();
                }
            } else {                       // 写任务
                request->write();
            }
        } else {                          // Proactor
            // 数据已读好，直接处理
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
```

---

## 实现指南

### Step 1：线程池的模板定义

```cpp
template <typename T>  // [语法] 模板：线程池可处理任意类型任务
class threadpool {
public:
    threadpool(int actor_model, connection_pool *connPool,
               int thread_number = 8, int max_request = 10000);
    ~threadpool();

    // Proactor 入口：数据已读好
    bool append_p(T *request);

    // Reactor 入口：需要自己读（state: 0=读, 1=写）
    bool append(T *request, int state);

private:
    static void* worker(void *arg);  // [语法] 静态函数作为 pthread 入口
    void run();                       // 工作线程的主循环

    int m_thread_number;
    pthread_t *m_threads;           // [语法] 线程 ID 数组（C 风格数组）
    std::list<T*> m_workqueue;      // [语法] STL 双向链表作为任务队列
    locker m_queuelocker;           // 保护队列的互斥锁
    sem m_queuestat;                // 信号量：队列中任务数（0 = 空，线程等待）
    connection_pool *m_connPool;    // 数据库连接池
    int m_actor_model;              // 0=Proactor, 1=Reactor
};
```

### Step 2：构造时创建线程

```cpp
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool,
                          int thread_number, int max_requests)
    : m_actor_model(actor_model), m_thread_number(thread_number),
      m_max_requests(max_requests), m_connPool(connPool)
{
    m_threads = new pthread_t[thread_number];  // [语法] new[] 动态数组
    for (int i = 0; i < thread_number; ++i) {
        pthread_create(m_threads + i, NULL, worker, this); // [语法] 指针运算
        pthread_detach(m_threads[i]);  // 分离线程：不需 join，自动回收
    }
}
```

**`pthread_detach` vs `pthread_join`：**
- `pthread_detach`：线程结束时自动回收资源。适合"一创建就干活到死"的线程。
- `pthread_join`：主线程等待子线程结束，显式回收。适合"需要获取返回值"的场景。
- 线程池的工作线程用 detach——它们和进程同生共死，不需要 join。

### Step 3：任务投递

```cpp
// Proactor：数据已读好
template <typename T>
bool threadpool<T>::append_p(T *request) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;  // 队列满，拒绝
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();  // 信号量 +1，唤醒一个工作线程
    return true;
}

// Reactor：需要指定读/写状态
template <typename T>
bool threadpool<T>::append(T *request, int state) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;  // 0=读, 1=写
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}
```

### Step 4：工作线程主循环

```cpp
template <typename T>
void threadpool<T>::run() {
    while (true) {
        m_queuestat.wait();  // 阻塞直到有任务
        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        if (request == NULL) continue;

        // Proactor 模式
        if (1 != m_actor_model) {
            connectionRAII mysqlcon(&request->mysql, m_connPool); // RAII 获取连接
            request->process();  // 纯业务处理
        }
        // Reactor 模式：工作线程自己负责 IO
        else {
            if (0 == request->m_state) {
                if (request->read_once()) {  // 工作线程读数据
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;  // 读失败 → 标记超时
                }
            } else {
                if (request->write()) {  // 工作线程写数据
                    request->improv = 1;
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
    }
}
```

**`improv` 和 `timer_flag` 的作用：**
- `improv = 1`：告诉主线程"我处理完了"
- `timer_flag = 1`：告诉主线程"这个连接有问题，请关闭它"
- Reactor 模式中，主线程在 `dealwithread`/`dealwithwrite` 中自旋等待这些标志

---

## 验证用例与预期结果

### 测试 1：编译

```bash
# Phase 4 的线程池通常需要结合 http_conn 来测试
# 最简单的验证方式：用 threadpool<int> 做一个简单的并行任务测试
```

### 测试 2：gdb 验证线程数

```bash
gdb ./build/server
(gdb) break threadpool::threadpool
(gdb) run
(gdb) info threads
# 应该看到 1 (main) + thread_number 个工作线程
```

### 失败排查

| 症状 | 可能原因 |
|------|---------|
| 线程创建失败 | `thread_number <= 0` 或系统线程数已达上限 |
| 任务投递后不执行 | 信号量 fail 或工作线程在 wait 之前就退了 |
| 死锁 | 锁的获取顺序不一致（locker → queuestat 还是反过来？） |

---

## C++ 语法速查

| 语法 | 示例 | 说明 |
|------|------|------|
| `template <typename T>` | `class threadpool { ... }` | 类模板 |
| `static` 成员函数 | `static void* worker(void*)` | 无 this 指针，可作 pthread 回调 |
| `pthread_create` | `pthread_create(&tid, NULL, worker, this)` | 创建线程，this 作为参数传给 worker |
| `pthread_detach` | `pthread_detach(tid)` | 分离线程，不 join |

---

## 阶段小结

你实现了 `threadpool<T>`（模板化 + Proactor/Reactor 双模式 + 数据库连接池对接）。

下一阶段：**Phase 5 — 定时器**。
