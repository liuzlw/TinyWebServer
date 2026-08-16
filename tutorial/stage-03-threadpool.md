# Stage 3：多线程与线程池

上一阶段你写出的 echo 服务器有个硬伤：**一次只能服务一个客户端**，一个慢客户端会拖住后面所有人。本阶段引入多线程来解决它，但不是"来一个连接就随手开一个线程"（线程创建/销毁有开销，连接一多就崩），而是用**线程池**——预先创建一批线程，任务排队、线程领取。这是本项目并发模型的第二环，也是仓库里 `threadpool/threadpool.h` 和 `lock/locker.h` 两个文件的原型。

完成本阶段后，你将：

- 把 Linux 的 pthread 同步原语（互斥锁、信号量、条件变量）封装成三个 C++ 类；
- 亲手写出一个模板类 `threadpool<T>`，理解"生产者-消费者"模型；
- 把 echo 服务器改造成**并发版**：多个客户端同时输入、互不阻塞；
- 学会用 `ps -L` 和 gdb 观察多线程程序的运行状态。

## 前置要求

- 已完成 [Stage 2：阻塞式 echo 服务器](stage-02-socket-echo.md)，工作区当前有两个文件：
  - `my_tiny_webserver/echo_server.cpp`（循环版阻塞 echo）
  - `my_tiny_webserver/makefile`
- 会基本的 make 用法（`make` / `make clean`）、会用 `nc` 连接服务器。

> 本阶段结束时，工作区应新增 `lock/locker.h`、`threadpool/threadpool.h` 两个头文件，并把 `echo_server.cpp` 换成线程池并发版。

## 理论学习

### 1. 进程与线程

- **进程（process）**：操作系统分配资源的单位。每个进程有独立的地址空间、独立的打开文件表，进程之间互不干扰，通信要靠管道、socket 等机制。你每次运行 `./echo_server`，就是一个进程。
- **线程（thread）**：进程内部**共享**同一块地址空间的执行流。同一个进程里的多个线程可以访问同一份全局变量、同一批文件描述符，通信成本极低；但正因共享，也带来了"竞态条件"（见下）。一个进程至少有一个线程（`main` 所在的主线程）。

```text
  进程（echo_server）
  ┌──────────────────────────────────────────────┐
  │  共享：全局变量、堆内存、打开的文件描述符(fd)   │
  │  ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐     │
  │  │主线程 │ │线程 1 │ │线程 2 │ │线程 3 │ ... │
  │  └───┬───┘ └───┬───┘ └───┬───┘ └───┬───┘     │
  │      │         │         │         │          │
  │  accept连接 从队列取任务 从队列取任务 ...       │
  └──────┴─────────┴─────────┴─────────┴──────────┘
```

### 2. 为什么需要并发：回答 Stage 2 的问题

Stage 2 的服务器，主线程要么在 `recv` 上等客户端 A 的数据，要么在 `accept` 上等新连接——**两件事没法同时做**。引入多线程后，分工就清晰了：

- **主线程**：专职 `accept`，一拿到新连接就塞进任务队列，立刻回去继续 `accept`，绝不被某个客户端拖住；
- **工作线程**：专职从任务队列取任务、执行 `recv`/`send` 回显。一个线程被慢客户端卡住，其他线程照常服务别人。

这就是本项目的并发主线：**主线程只负责"接客"，工作线程负责"干活"**。

### 3. 竞态条件与临界区

因为线程共享内存，当两个线程**同时**读写同一份数据，结果会取决于"谁先谁后"，而先后顺序不可控，于是结果随机、可能出错——这叫**竞态条件（race condition）**。典型例子：

```cpp
// 两个线程同时执行 counter++（实际是 读 → 加1 → 写回 三步）
counter++;   // 期望 +2，实际可能只 +1，因为两步互相覆盖
```

被共享、且同一时刻只允许一个线程访问的那段代码，叫**临界区（critical section）**。解决竞态条件的核心思想就是：**给临界区上一把锁，同一时刻只放一个线程进去**。Linux 提供了三类同步原语（见下），本阶段把它们封装成类。

### 4. 互斥锁 / 信号量 / 条件变量：各解决什么问题

| 原语 | 类比 | 解决什么问题 | 本阶段用途 |
|---|---|---|---|
| **互斥锁（mutex）** | 卫生间门锁：一次只能进一个人 | 保护一段临界区，同一时刻只有一个线程执行它 | 保护任务队列的 push/pop |
| **信号量（semaphore）** | 停车场计数牌：剩余车位 N，进一个减一，出一个加一 | 统计"某种资源的数量"，值为 0 时等待 | 统计"队列里有多少个任务"，为 0 时工作线程睡觉 |
| **条件变量（cond）** | 会议通知：等"某个条件成立"再继续 | 等待一个"条件"变成真，通常配合互斥锁使用 | 本阶段线程池暂未用到，仓库完整版用它的 `timewait` 做超时 |

一句话区分：**锁锁的是"数据"，信号量计的是"数量"，条件变量等的是"条件"**。本阶段线程池只用前两者（条件变量在 Stage 7 日志的阻塞队列里会正式登场，这里先学会类怎么写）。

### 5. 生产者-消费者模型

线程池是一个经典的**生产者-消费者**模型：

```text
   生产者（主线程）                      消费者（工作线程 × 8）
   accept 新连接                         每个线程循环：
        │                                    │
        │ append(task)  [任务队列]           │
        ▼               ┌───────┐            ▼
   task* t = new task ──►│ 队列  │── 取出队头 task ──► process() 回显
                         └───────┘
   放一个任务：post() 信号量 +1            取一个任务：wait() 信号量 -1（为 0 则睡觉）
```

- 主线程把任务**放入**队列（生产），工作线程**取出**任务执行（消费）；
- 队列本身是共享数据，用**互斥锁**保护；
- "队列里有没有任务"用**信号量**计数：没任务时信号量为 0，工作线程 `wait()` 阻塞睡觉，CPU 占用几乎为 0；主线程放入一个任务就 `post()` 一次，唤醒一个工作线程。

### 6. pthread 简介

Linux 线程的标准是 POSIX 线程（pthread），核心 API 都来自 C 函数：

```c
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);  // 创建线程
int pthread_detach(pthread_t thread);    // 分离线程，结束后资源自动回收
int pthread_mutex_lock(pthread_mutex_t *mutex);     // 加锁
int pthread_mutex_unlock(pthread_mutex_t *mutex);   // 解锁
int sem_wait(sem_t *sem);   // 信号量减一，为 0 则阻塞
int sem_post(sem_t *sem);   // 信号量加一，唤醒等待者
```

注意第 4 个参数 `start_routine` 的类型是 `void *(*)(void *)`——一个**没有隐式 `this` 指针**的函数。普通成员函数自带 `this`，类型不匹配，没法直接当回调，这正是后面 `worker` 要声明成 `static` 的原因（见 C++ 知识点第 4 条）。

### 7. 半同步/半反应堆并发模式（一句话预热）

本项目采用的并发模型叫**半同步/半反应堆**（Half-Sync/Half-Reactor）：主线程（同步地）`accept` 连接并**分发**任务，工作线程池（同步地）处理任务，二者通过一个任务队列解耦。画成图就是"生产者-消费者"图加上一条主线：**主线程接受连接 → 塞队列 → 工作线程处理**。到 [Stage 4](stage-04-epoll.md) 引入 epoll 后，主线程还会升级为"事件分发器"，但"主线程分发、线程池处理"的骨架从本阶段就固定下来了。

## 本阶段 C++ 知识点

### 1. 类封装：把 C API 包装成类

pthread 的 API 都是"先 `xxx_init` 初始化，用完 `xxx_destroy` 销毁"的套路，而且状态（锁、信号量）要由使用者自己揣在手里传来传去，很容易忘 init 或忘 destroy。C++ 的做法是**封装成类**：把状态作为成员变量，把 init 放进构造函数、destroy 放进析构函数：

```cpp
class locker
{
public:
    locker()  { pthread_mutex_init(&m_mutex, NULL); }   // 构造：初始化
    ~locker() { pthread_mutex_destroy(&m_mutex); }       // 析构：销毁
    bool lock()   { return pthread_mutex_lock(&m_mutex) == 0; }
    bool unlock() { return pthread_mutex_unlock(&m_mutex) == 0; }
private:
    pthread_mutex_t m_mutex;   // 状态藏进成员，外界不用操心
};
```

使用者只需 `locker l; l.lock(); ... l.unlock();`，再也不会忘 init/destroy。

### 2. 构造/析构管理资源（RAII 的雏形）

构造函数负责"获取资源"，析构函数负责"释放资源"，对象的生命周期与资源的生命周期绑定——这是 **RAII（Resource Acquisition Is Initialization，资源获取即初始化）** 思想。本阶段的 `sem`/`locker`/`cond` 三个类正是 RAII 的最小示例（更完整的 RAII 应用见 [Stage 8](stage-08-mysql.md) 的连接池）。

### 3. 模板类：threadpool\<T\>

线程池不知道自己将来要处理的"任务"是什么类型——本阶段是 `task`，Stage 5 变成 `http_conn`。用**类模板**把类型参数化：

```cpp
template <typename T>
class threadpool { /* ... std::list<T*> m_workqueue; ... */ };

threadpool<task> pool;          // T = task，本阶段
threadpool<http_conn> pool;     // T = http_conn，Stage 5 之后
```

模板类有个重要约束：**成员函数的定义必须放在头文件里**（或与声明同一个文件），编译器在实例化 `threadpool<task>` 时才能看到完整定义生成代码。这就是"模板类头文件即可，无需单独编译"的原因。

### 4. 静态成员函数：worker 为什么是 static

`pthread_create` 要求回调是 `void *(*)(void *)`。普通成员函数 `void *worker(void*)` 背后其实有个隐藏参数 `this`，类型是 `void *(threadpool::*)(void*)`，**不匹配**。把 `worker` 声明成 `static`，它就变成了没有 `this` 的普通函数，类型刚好吻合：

```cpp
static void *worker(void *arg);          // 静态：无 this，可作回调
// 但静态成员无法访问非静态成员，所以要用 arg 把 this 传进去，再转型回来：
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;  // 把 void* 转回 threadpool*
    pool->run();                           // 再调用真正的成员函数
    return pool;
}
```

这就是"C 回调"和"C++ 成员函数"之间的经典桥梁：**静态函数 + 传 `this`**。

### 5. this 指针

`this` 是指向"当前对象"的指针。在 `main` 里创建线程池时，我们把对象自己的地址传给线程：

```cpp
pthread_create(m_threads + i, NULL, worker, this);
//                                              ^^^^ 把线程池对象的地址交给线程
```

线程跑起来后，`worker` 收到的 `arg` 就是那个 `this`，转型回来就能调用该对象的 `run()`。`this` 在本阶段出现的三处：构造时传出去、`worker` 里转回来、`run()` 里访问成员。

### 6. std::list：双向链表

任务队列用 `std::list<T*>`，因为它的插入和删除都是 O(1)，且不会像 vector 那样因扩容移动元素（移动会让别处存的指针失效）：

```cpp
std::list<T *> m_workqueue;      // 声明
m_workqueue.push_back(request);  // 队尾插入（生产）
T *request = m_workqueue.front(); // 读队头
m_workqueue.pop_front();          // 队头删除（消费）
```

需要 `#include <list>`。

### 7. 异常：throw std::exception()

初始化失败时，与其返回一个"半死不活"的对象让使用者继续用，不如直接抛异常终止构造：

```cpp
threadpool(int thread_number = 8, int max_requests = 10000);
// 构造里：
if (thread_number <= 0 || max_requests <= 0)
    throw std::exception();     // 参数非法，直接抛
```

抛出异常后，构造函数不会再继续执行，对象不会进入"已构造"状态，从根上杜绝了"用一个坏线程池"。需要 `#include <exception>`。

## 动手实现

### a) `lock/locker.h`：三个同步原语类的封装

这个文件**与仓库 `lock/locker.h` 完全一致**，一个字都不用改，直接照抄。在 `my_tiny_webserver/` 下建 `lock/` 目录，创建 `locker.h`：

```cpp
#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};
class locker
{
public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};
class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            //pthread_mutex_destroy(&m_mutex);
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        //pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        //pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    //static pthread_mutex_t m_mutex;
    pthread_cond_t m_cond;
};
#endif
```

逐段讲解：

**`sem`（信号量封装）**

- 构造函数 `sem(int num)` 调 `sem_init(&m_sem, 0, num)`：第二个参数 `0` 表示"在线程间共享"（不是进程间），第三个参数 `num` 是信号量**初值**；
- 默认构造 `sem()` 等价于 `sem(0)`，初值为 0——线程池的 `m_queuestat` 就用它，表示"一开始队列里 0 个任务"；
- `wait()` 调 `sem_wait`：信号量减一，若已为 0 则阻塞睡眠；`post()` 调 `sem_post`：加一并唤醒一个等待者；
- 析构 `sem_destroy` 释放信号量。注意：初始化失败要 `throw`，否则留着一个未初始化的信号量，后面 `sem_wait` 行为未定义。

**`locker`（互斥锁封装）**

- 构造 `pthread_mutex_init(&m_mutex, NULL)`（NULL 表示用默认属性），析构 `pthread_mutex_destroy`；
- `lock()` / `unlock()` 分别包 `pthread_mutex_lock` / `unlock`；
- `get()` 把内部的 `pthread_mutex_t*` 暴露出去——供条件变量的 `wait` 使用（见下）。本阶段线程池没用到它，但仓库完整版要用，所以保留。

**`cond`（条件变量封装）**

- `wait(pthread_mutex_t *m_mutex)` 包 `pthread_cond_wait`。**为什么必须配合 mutex？** 因为"检查条件"和"睡眠等待"之间有个竞态窗口：如果先检查"队列空"再睡眠，中间可能插入生产者 `post`，导致通知丢失、线程永久睡眠。`pthread_cond_wait` 会**原子地**完成两件事——先释放锁、再进入睡眠；被唤醒后重新拿到锁再返回。所以必须和一把 mutex 配合；
- `timewait(m_mutex, t)` 包 `pthread_cond_timedwait`：多一个超时，超时未满足条件也返回。仓库完整版用它给任务处理加超时（本阶段不用）；
- `signal()` 唤醒一个等待者，`broadcast()` 唤醒全部；
- 那几个 `//pthread_mutex_lock(&m_mutex);` 注释是仓库原样保留的"笔记"——提醒自己"这个锁由外部负责，wait 内部会自动处理"。照抄即可。

### b) `threadpool/threadpool.h`：简化版线程池

**简化点说明（务必阅读）**：本阶段是仓库 `threadpool/threadpool.h` 的**教学简化版**，相对仓库做了如下裁剪：

1. 去掉了构造函数参数 `actor_model`（Reactor/Proactor 模式切换）与 `connection_pool *connPool`（MySQL 连接池）——这两个到 [Stage 8](stage-08-mysql.md)（连接池）和 [Stage 9](stage-09-integration.md)（整合）恢复；
2. `append(T*)` 去掉了 `state` 参数（读/写状态位）；
3. `run()` 中直接调用 `request->process()`，不再区分 `read_once`/`write`/`process` 等分支。

在 `my_tiny_webserver/` 下建 `threadpool/` 目录，创建 `threadpool.h`：

```cpp
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"

// ===== 简化点（相对仓库 threadpool/threadpool.h）=====
// 1. 去掉 actor_model（Reactor/Proactor 切换）与 connection_pool（MySQL 连接池）参数；
// 2. append(T*) 去掉 state（读/写状态）；
// 3. run() 直接调用 request->process()，不再区分 read_once/write。
//    上述内容分别在 Stage 8（连接池）、Stage 9（整合）恢复仓库完整版。
// =====================================================

template <typename T>
class threadpool
{
public:
    /*thread_number 是线程池中线程的数量，max_requests 是请求队列中最多允许等待处理的请求数量*/
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理（信号量，兼作任务计数）
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests) : m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;
        request->process();
    }
}

#endif
```

逐段讲解：

**构造函数：创建并 detach 线程**

- 初始化列表 `m_thread_number(thread_number), ...` 给成员赋初值；
- `m_threads = new pthread_t[m_thread_number];` 动态分配一个 `pthread_t` 数组，用来存每个线程的句柄；
- `for` 循环里 `pthread_create(m_threads + i, NULL, worker, this)` 创建第 i 个线程：线程体是静态函数 `worker`，参数是 `this`；
- `pthread_detach(m_threads[i])` 把线程设为**分离态**：线程结束后内核自动回收它的资源，主程序不用（也无法）`pthread_join` 去收尸。线程池里的工作线程一跑就是一辈子，所以 detach 正合适；
- 任何一步失败都要 `delete[] m_threads` 再 `throw`，避免内存泄漏。

**`worker` 静态函数**

见"知识点 4"：静态成员函数没有 `this`，正好匹配 `pthread_create` 要求的回调签名。它把 `arg`（就是构造时传进去的 `this`）转回 `threadpool*`，再调用真正的成员函数 `run()`。

**`run()`：工作线程的循环（核心）**

```text
 while(true)
   ├─ m_queuestat.wait();         // 信号量减一；为 0 就睡觉，等人 post
   ├─ m_queuelocker.lock();       // 加锁保护队列
   ├─ 若队列空 → 解锁、continue   // 理论上不会发生，但防御一下
   ├─ 取队头 request，pop_front
   ├─ m_queuelocker.unlock();     // 拿到任务立刻解锁，别抱着锁干活
   ├─ 若 request 为空 → continue
   └─ request->process();         // 执行任务（回显）
```

顺序是关键：**先 wait（睡觉等任务）→ 加锁取任务 → 解锁 → 再执行**。取完任务马上解锁，是因为 `process()` 可能很耗时，绝不能占着锁，否则其他线程全被卡在 `lock()` 上，线程池退化成串行。

**`append(T*)`：生产任务**

- `lock()` 保护队列的 `push_back`；
- 队列已满（`size() >= m_max_requests`）则 `unlock()` 后返回 `false`（拒绝任务）；
- 否则 `push_back` 后 `unlock()`，**最后** `m_queuestat.post()` 信号量加一，唤醒一个睡觉的工作线程。

**为什么用信号量做"任务计数"，而不是只用锁？** 如果只用锁，工作线程只能"不停地加锁看队列空不空、空就解锁再试"——这叫**忙等（busy-wait）**，CPU 空转。用信号量后，队列空时工作线程在 `sem_wait` 上**睡眠**，几乎不耗 CPU；一旦有任务 `post` 才被唤醒，既省电又响应快。锁负责"队列数据安全"，信号量负责"有没有活干"，二者各司其职。

### c) 并发版 echo：定义 task 类 + 主线程塞任务

现在把 echo 服务器改成线程池版。**完整替换** `my_tiny_webserver/echo_server.cpp`：

```cpp
// echo_server.cpp —— Stage 3：线程池并发版 echo 服务器
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <arpa/inet.h>

#include "threadpool/threadpool.h"

#define BUF_SIZE 1024

void error_handling(const char *msg)
{
    perror(msg);
    exit(1);
}

// 任务类：一个客户端连接对应一个 task 对象
class task
{
public:
    task(int sockfd) : m_sockfd(sockfd) {}

    // 工作线程实际执行的函数：回显循环（原 Stage 2 主线程里那段逻辑挪到了这里）
    void process()
    {
        int str_len;
        while ((str_len = recv(m_sockfd, m_buf, sizeof(m_buf) - 1, 0)) > 0)
        {
            m_buf[str_len] = '\0';
            send(m_sockfd, m_buf, str_len, 0);
        }
        close(m_sockfd);
    }

private:
    int m_sockfd;         //该任务负责的客户端 socket
    char m_buf[BUF_SIZE]; //接收缓冲区
};

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        printf("用法: %s <端口号>\n", argv[0]);
        exit(1);
    }

    // 1. 创建套接字
    int serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1)
        error_handling("socket() error");

    // 2. 绑定地址
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(atoi(argv[1]));

    if (bind(serv_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        error_handling("bind() error");

    // 3. 监听
    if (listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    printf("echo 服务器（线程池版）已启动，监听端口 %s\n", argv[1]);

    // 创建线程池：默认 8 个工作线程，任务队列最多 10000 个
    threadpool<task> pool;

    while (1)
    {
        // 4. 主线程专职 accept，拿到连接立即交给线程池
        struct sockaddr_in clnt_addr;
        socklen_t clnt_addr_size = sizeof(clnt_addr);
        int clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);
        if (clnt_sock == -1)
        {
            perror("accept() error");
            continue;
        }

        printf("客户端连接: %s:%d\n",
               inet_ntoa(clnt_addr.sin_addr), ntohs(clnt_addr.sin_port));

        // 每 accept 到一个连接，就 new 一个 task 丢进线程池，主线程立刻回去 accept
        task *t = new task(clnt_sock);
        pool.append(t);
    }

    close(serv_sock);
    return 0;
}
```

关键点讲解：

**task 类**：成员 `int m_sockfd`（负责的客户端 fd）和 `char m_buf[1024]`（接收缓冲区）；`process()` 就是 Stage 2 里"为单个客户端回显"的那段 while 循环，现在整体搬进了任务类。构造函数 `task(int sockfd) : m_sockfd(sockfd) {}` 用初始化列表把 fd 存进成员。

**主线程**：`accept` 拿到 `clnt_sock` 后，`task *t = new task(clnt_sock); pool.append(t);` 把任务交给线程池，然后**立即回到循环顶部继续 accept**，不再亲自 recv。这正是"主线程接客、工作线程干活"的分工。

**主线程 accept 与 task 共用 fd 的并发安全**：`clnt_sock` 是内核分配的一个 fd 号，主线程把它 `new` 进某个 task 之后，就**再也不碰它**；这个 fd 只被"从队列里取出该 task 的那一个工作线程"读写。因为每个 task 只被放入队列一次、只被一个线程取出一次，所以**同一个连接在同一时刻只有一个任务在跑**，不存在两个线程同时读写同一个 fd 的竞态。主线程唯一的共享对象是**任务队列**，而它由 `m_queuelocker` 保护——并发安全由此得到保证。

**`new` 出来的 task 谁来 delete？（内存管理伏笔）** 本阶段故意不 delete：`process()` 结束后 task 对象就"没人管了"，是**内存泄漏**。这是留给你的思考题（见文末）。真实项目里不会每个连接都 `new` 一个对象——仓库的做法是**预先分配一个 `http_conn` 数组**，按 fd 复用，见 [Stage 5](stage-05-http.md) 与 [Stage 9](stage-09-integration.md)。

## 编译与运行

### makefile 更新：多文件 + 模板头文件

本阶段有 3 个源文件（其实是 1 个 .cpp + 2 个 .h）。**模板类的实现全部写在头文件里，因此头文件不需要（也不能）单独编译**，编译器是在编译 `echo_server.cpp`、遇到 `#include "threadpool/threadpool.h"` 和 `threadpool<task> pool;` 时，现场把模板实例化、生成代码的。所以 makefile 里只需编译 `echo_server.cpp`，但要把头文件列为**依赖**——这样"头文件一改，make 就知道要重新编译"。

**完整替换** `my_tiny_webserver/makefile`（命令行缩进是 Tab）：

```makefile
# makefile —— 命令行缩进必须是 Tab 键！
CXX = g++
CXXFLAGS = -Wall -g
# pthread 在多线程程序中需要链接；Ubuntu 22.04 的 glibc 已内置 pthread，
# -lpthread 是空壳但仍建议写上，保证在旧系统上也能编译通过
LDLIBS = -lpthread

# 依赖列表含头文件：头文件改动也会触发重编译
# 命令里只编译 echo_server.cpp，头文件靠 #include 引入，不单独编译
echo_server: echo_server.cpp lock/locker.h threadpool/threadpool.h
	$(CXX) $(CXXFLAGS) -o $@ echo_server.cpp $(LDLIBS)

clean:
	rm -f echo_server
```

解释新增的两点：

- **`LDLIBS = -lpthread`**：链接线程库。这里把编译和链接两步合成了一条命令（g++ 一次搞定），所以 `-lpthread` 跟在源文件后面即可；
- **头文件列入依赖但不出现在编译命令里**：注意命令里是 `echo_server.cpp`，**没有**用 `$^`（否则 `$^` 会把两个 .h 也展开进去，g++ 会试图把它们当独立翻译单元编译，反而不对）。头文件只出现在"依赖"位置，作用是触发重编译。

### 完整操作流程

```bash
cd ~/projects/my_tiny_webserver
make
./echo_server 9006
```

两个终端同时连接（见验收清单），观察并发回显。

## 验收清单

- [ ] 目录结构正确：`ls -R` 能看到 `echo_server.cpp`、`makefile`、`lock/locker.h`、`threadpool/threadpool.h`；
- [ ] `make clean && make`：打印 `g++ -Wall -g -o echo_server echo_server.cpp -lpthread`，无报错、无警告，`ls` 出现 `echo_server`；
- [ ] 运行：`./echo_server 9006`，打印 `echo 服务器（线程池版）已启动，监听端口 9006`；
- [ ] 查监听：`ss -tlnp | grep 9006` 显示 `LISTEN ... :9006 ... echo_server`；
- [ ] 并发回显（关键）：终端 A `nc 127.0.0.1 9006` 输入 `aaa` 回显 `aaa`；**不退出**，再开终端 B `nc 127.0.0.1 9006` 输入 `bbb`，**立即**回显 `bbb`（Stage 2 里这里是"无回显"，现在被线程池解决了）；
- [ ] 交叉验证互不阻塞：终端 A 保持连接但**什么都不发**，终端 B 输入 `ccc` 仍能**立即**回显 `ccc`；
- [ ] 观察线程数：保持服务器运行，另开终端执行 `pgrep echo_server` 拿到 PID，再 `ps -L -p <PID>`，看到 **9 行线程**（1 个主线程 + 8 个工作线程，`LWP` 一列各不相同）；
- [ ] gdb 观察线程：`gdb ./echo_server`，`(gdb) run 9006`，服务器启动后按 `Ctrl+C` 打断，`(gdb) info threads` 列出约 9 个线程（主线程停在 `accept` 附近，工作线程停在 `sem_wait`/`futex` 附近）；
- [ ] gdb 切换线程：`(gdb) thread 2` 切到工作线程，`(gdb) bt` 能看到调用栈里出现 `threadpool<task>::run` / `worker` 字样；
- [ ] 退出与清理：`(gdb) quit` 后，普通运行用 `Ctrl+C` 退出服务器；`ss -tlnp | grep 9006` 无输出；`make clean` 删除 `echo_server`。

> gdb 里 `info threads` 看到的主线程帧可能显示为 `__libc_accept`、工作线程显示为 `sem_wait` 或 `do_futex_wait`，具体符号随 glibc 版本略有差异，属正常；关键是线程数量正确、`bt` 里能认出 `run`/`worker`。

## 参考答案对照

| 文件 | 与仓库的关系 | 说明 |
|---|---|---|
| `lock/locker.h` | **完全一致** | 对应仓库 [lock/locker.h](../lock/locker.h)，逐字节相同，`sem`/`locker`/`cond` 三个类、成员名、注释全部照搬 |
| `threadpool/threadpool.h` | **简化版** | 对应仓库 [threadpool/threadpool.h](../threadpool/threadpool.h)，差异见下表 |
| `echo_server.cpp` | 无对应文件 | 仓库没有独立的 echo 程序；本文件是"socket 流程 + 线程池用法"的练习载体，其 socket 部分对应 `webserver.cpp`，线程池用法对应 `WebServer::thread_pool()`（[webserver.cpp](../webserver.cpp) 第 97～101 行 `m_pool = new threadpool<http_conn>(...)`） |

`threadpool.h` 相对仓库的**差异点清单**（即本阶段的"简化点"，将在后续阶段恢复）：

| 差异 | 本阶段 | 仓库完整版 | 恢复于 |
|---|---|---|---|
| 构造函数参数 | `(int thread_number, int max_requests)` | 多 `actor_model`、`connection_pool *connPool` | [Stage 8](stage-08-mysql.md) / [Stage 9](stage-09-integration.md) |
| 成员 | 无 | 多 `connection_pool *m_connPool`、`int m_actor_model` | 同上 |
| `append` | `append(T*)` | `append(T*, int state)` + `append_p(T*)` | [Stage 9](stage-09-integration.md) |
| `run()` | 直接 `request->process()` | 按 `m_actor_model` 分支，调 `read_once`/`write`/`process`，并包 `connectionRAII` | [Stage 8](stage-08-mysql.md) / [Stage 9](stage-09-integration.md) |
| 任务类型 | `task`（自定义） | `http_conn` | [Stage 5](stage-05-http.md) |
| 任务生命周期 | 每次 `new`，暂未释放（见思考题） | 预先分配 `users` 数组，按 fd 复用，不逐个 new | [Stage 5](stage-05-http.md) |

其余（类名 `threadpool`、成员名 `m_thread_number`/`m_workqueue`/`m_queuelocker`/`m_queuestat`、`worker`/`run`/`append` 函数名、"信号量 + 锁"的协作方式）均与仓库保持一致。

## 常见问题

1. **编译报 `undefined reference to pthread_create` 之类的链接错误**
   忘了链接线程库。检查 makefile 里有没有 `-lpthread`。手工编译时命令要带：`g++ -Wall -g -o echo_server echo_server.cpp -lpthread`。

2. **`undefined reference to threadpool<task>::...`（模板链接错误）**
   把模板类的成员函数定义写到了单独的 `.cpp` 文件里。模板必须"定义可见"才能实例化，解决办法：把实现全部放进 `threadpool.h`（本阶段已是如此），或把 `.cpp` 显式 `#include` 进来。这是模板最经典的坑。

3. **`fatal error: ../lock/locker.h: No such file or directory`**
   `threadpool.h` 用相对路径 `#include "../lock/locker.h"` 引锁头文件，路径是以 `threadpool.h` 所在位置为基准的。务必保持目录结构 `lock/locker.h` 与 `threadpool/threadpool.h` 分属两个子目录，别把它们平铺到根目录。

4. **多个客户端同时输入时，回显内容"串行"到别的客户端上 / 数据错乱**
   说明队列没加锁，或锁的粒度不对（比如把 `process()` 也放在了锁内）。请核对 `append` 与 `run` 里的 `lock`/`unlock` 是否配对、`process()` 是否在 `unlock` 之后执行。

5. **程序起来后 CPU 占用 100%（忙等）**
   多半是工作线程没有用 `sem_wait` 睡觉，而是"循环加锁检查队列"的忙等实现。用信号量 `m_queuestat` 计数，队列空时线程应阻塞在 `wait()` 上（见"为什么用信号量做任务计数"）。

6. **`ps -L -p <PID>` 只看到 1 个线程**
   说明 `pthread_create` 没成功或根本没执行到。检查：线程池对象是否在 `main` 里构造（而不是定义在 accept 循环里每次重建）、`thread_number` 是否 > 0、`pthread_create` 返回值是否被检查并抛了异常。

7. **服务器退出后，之前 `new` 的 task 没释放（内存泄漏）**
   这是本阶段**有意保留**的教学伏笔：`process()` 里只 `close(m_sockfd)`，没 `delete this`。后果与正确做法见思考题第 1 题；仓库的根治方案（预分配数组复用）见 [Stage 5](stage-05-http.md)。

8. **gdb 里按 Ctrl+C 后所有线程停住，但不知道当前在哪个线程**
   用 `info threads` 看列表（`*` 标的是当前线程），`thread N` 切换，`bt` 看栈。要单步时注意默认 `scheduler-locking` 会放行其他线程，可 `set scheduler-locking on` 只让当前线程走。更多多线程调试技巧见 [附录 B](appendix-b-gdb.md)。

## 思考题

1. 本阶段 `new` 出来的 `task` 从未被 `delete`，属于内存泄漏。它应该在哪里被释放？如果在 `process()` 末尾写 `delete this;` 会有什么坑（提示：`run()` 里取到 request 后还访问了它吗）？仓库最终用什么方案避免每个连接都 new？
2. 线程池的线程数（默认 8）该怎么选？设置成 1、设置成 10000 分别会怎样？它和"CPU 核数""任务类型（CPU 密集 vs IO 密集）"有什么关系？
3. `m_queuestat` 信号量初值为什么是 0？如果初值设成 8（线程数）会发生什么？
4. `pthread_detach` 的后果是什么？如果改成 `pthread_join` 会怎样？为什么线程池用 detach 更合适？
5. `run()` 里 `m_queuestat.wait()` 返回后为什么要立刻 `lock()`？如果两个线程同时被 `post` 唤醒，会不会同时取到**同一个**任务？锁在这里起了什么作用？
6. 如果 `append()` 在队列满时返回 `false`，`main` 里现在是怎么处理的？这样做有什么隐患？更稳妥的做法是什么（提示：可参考 [Stage 5](stage-05-http.md) 如何拒绝过量连接）？

## 下一步

你已经拥有了一台"能并发回显"的服务器，也掌握了线程池与 pthread 同步原语。但"一个连接开一个 task、线程数固定"仍有瓶颈：连接成千上万时，线程池里的 8 个线程在大量 fd 上切换依然吃力。下一阶段引入 Linux 的高性能事件通知机制 **epoll**，让一个线程就能管理上万个连接。

继续阅读 [Stage 4：epoll 事件驱动](stage-04-epoll.md)。
