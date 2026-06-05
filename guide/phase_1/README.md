# Phase 1 —— 线程同步原语

## 目标

用 C++ 类封装 pthread 的三种同步原语，实现一个**生产者-消费者**测试程序。

**可见结果：** 多个生产者线程生产数据 → 多个消费者线程消费数据，全程无竞态、无死锁，最终所有数据恰好被消费完一次。

---

## 前置知识

- C++ 类的基本写法（构造函数、析构函数、成员函数）
- 知道多线程的大概概念（多个函数同时跑）

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | `include_directories`、多文件编译、`.h` 和 `.cpp` 分离 |
| **gdb** | `thread apply all bt`（查看所有线程栈）、`thread N`（切换线程） |

---

## 分步实现

### Step 1：理解 pthread 的三种原语

Linux 下多线程同步有三个核心工具，都在 `<pthread.h>` 里：

**互斥锁（mutex）**——保证同一时刻只有一个线程访问临界区：

```c
pthread_mutex_t mutex;
pthread_mutex_init(&mutex, NULL);   // 初始化
pthread_mutex_lock(&mutex);         // 加锁
// ... 临界区 ...
pthread_mutex_unlock(&mutex);       // 解锁
pthread_mutex_destroy(&mutex);      // 销毁
```

**信号量（semaphore）**——控制同时访问资源的线程数量：

```c
sem_t sem;
sem_init(&sem, 0, 5);   // 初始值 5，表示允许 5 个线程同时访问
sem_wait(&sem);         // P 操作：值减 1，如果值为 0 则阻塞
sem_post(&sem);         // V 操作：值加 1，唤醒等待的线程
sem_destroy(&sem);
```

**条件变量（condition variable）**——让线程等待"某个条件成立"：

```c
pthread_cond_t cond;
pthread_cond_init(&cond, NULL);
pthread_cond_wait(&cond, &mutex);        // 释放 mutex 并等待条件
pthread_cond_signal(&cond);              // 唤醒一个等待线程
pthread_cond_broadcast(&cond);           // 唤醒所有等待线程
pthread_cond_destroy(&cond);
```

### Step 2：封装成 C++ 类

C 风格 API 的问题是：忘记 `destroy` 就会泄漏资源。C++ 的解决方案是**RAII**——资源在构造时获取，在析构时自动释放。

```cpp
// locker.h
#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <semaphore.h>
#include <exception>

// ---- 信号量 ----
class sem {
public:
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0)
            throw std::exception();
    }
    explicit sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0)
            throw std::exception();
    }
    ~sem() { sem_destroy(&m_sem); }

    bool wait() { return sem_wait(&m_sem) == 0; }
    bool post() { return sem_post(&m_sem) == 0; }

private:
    sem_t m_sem;
};

// ---- 互斥锁 ----
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
            throw std::exception();
    }
    ~locker() { pthread_mutex_destroy(&m_mutex); }

    bool lock()   { return pthread_mutex_lock(&m_mutex) == 0; }
    bool unlock() { return pthread_mutex_unlock(&m_mutex) == 0; }
    pthread_mutex_t* get() { return &m_mutex; }

private:
    pthread_mutex_t m_mutex;
};

// ---- 条件变量 ----
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0)
            throw std::exception();
    }
    ~cond() { pthread_cond_destroy(&m_cond); }

    bool wait(pthread_mutex_t* m) {
        return pthread_cond_wait(&m_cond, m) == 0;
    }
    bool signal()    { return pthread_cond_signal(&m_cond) == 0; }
    bool broadcast() { return pthread_cond_broadcast(&m_cond) == 0; }

private:
    pthread_cond_t m_cond;
};

#endif
```

**设计要点：**
- 构造函数里 `init`，析构函数里 `destroy`——绝不可能忘。
- 构造函数里如果 `init` 失败就抛异常——不让"半成品"对象存在。
- 每个类的接口极简：只暴露使用方真正需要的操作。

### Step 3：多文件项目 + cmake

完整的 CMakeLists.txt：

```cmake
cmake_minimum_required(VERSION 3.10)
project(LockDemo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

include_directories(${CMAKE_SOURCE_DIR})

add_executable(producer_consumer
    test_producer_consumer.cpp
)
target_link_libraries(producer_consumer pthread)
```

> **注意 `target_link_libraries(... pthread)`：** pthread 是 Linux 的线程库，必须显式链接。如果不加，编译不会报错但运行时会崩溃或行为异常。

### Step 4：生产者-消费者测试

```cpp
// test_producer_consumer.cpp
#include <iostream>
#include <queue>
#include <pthread.h>
#include <unistd.h>
#include "locker.h"

// 共享资源
std::queue<int> q;       // 任务队列
locker mtx;              // 保护队列的互斥锁
sem slots(10);           // 队列空位数（初始 10）
sem items(0);            // 队列中数据数（初始 0）

bool done = false;       // 生产结束标志

void* producer(void* arg) {
    int id = *(int*)arg;
    for (int i = 0; i < 5; ++i) {
        sleep(1);                               // 模拟生产耗时
        slots.wait();                           // 等待空位
        mtx.lock();
        q.push(i);
        std::cout << "Producer " << id
                  << " produced " << i << std::endl;
        mtx.unlock();
        items.post();                           // 通知有新数据
    }
    return NULL;
}

void* consumer(void* arg) {
    int id = *(int*)arg;
    while (true) {
        items.wait();                           // 等待数据
        mtx.lock();
        if (q.empty() && done) {
            mtx.unlock();
            items.post();   // 通知其他消费者也退出
            break;
        }
        int val = q.front();
        q.pop();
        std::cout << "Consumer " << id
                  << " consumed " << val << std::endl;
        mtx.unlock();
        slots.post();                           // 释放空位
        sleep(2);                               // 模拟消费耗时
    }
    return NULL;
}

int main() {
    pthread_t producers[2], consumers[3];
    int ids[] = {1, 2, 3};

    // 创建 2 个生产者
    for (int i = 0; i < 2; ++i)
        pthread_create(&producers[i], NULL, producer, &ids[i]);

    // 创建 3 个消费者
    for (int i = 0; i < 3; ++i)
        pthread_create(&consumers[i], NULL, consumer, &ids[i]);

    // 等待生产者完成
    for (int i = 0; i < 2; ++i)
        pthread_join(producers[i], NULL);

    // 标记生产结束
    mtx.lock();
    done = true;
    mtx.unlock();
    items.post();   // 唤醒可能阻塞的消费者

    // 等待消费者完成
    for (int i = 0; i < 3; ++i)
        pthread_join(consumers[i], NULL);

    std::cout << "All done!" << std::endl;
    return 0;
}
```

编译运行：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/producer_consumer
```

你应该看到生产者和消费者的打印交替出现，最终 "All done!" 且所有数据恰好被消费一次。

### Step 5：gdb 多线程调试

```bash
gdb ./build/producer_consumer

# 在 gdb 内：
(gdb) break producer           # 在 producer 函数设断点
(gdb) break consumer           # 在 consumer 函数设断点
(gdb) run
# 程序会在某个线程停在断点

(gdb) info threads             # 查看所有线程
(gdb) thread 2                 # 切换到线程 2
(gdb) thread apply all bt      # 查看所有线程的调用栈
```

---

## 验证方法

- [ ] 程序每次运行都能正常结束（无死锁、无崩溃）
- [ ] 生产者生产的每个数据恰好被一个消费者消费（检查输出计数）
- [ ] `valgrind --tool=helgrind ./build/producer_consumer` 无竞态报告
- [ ] gdb 中能用 `thread apply all bt` 看到所有线程栈

---

## 踩坑记录

1. **忘记链接 `-lpthread`。** 症状：编译通过但运行时崩溃。在 cmake 里一定要 `target_link_libraries(xxx pthread)`。

2. **条件变量虚假唤醒。** `pthread_cond_wait` 返回后，你等待的条件不一定为真（OS 可能虚假唤醒）。所以必须在 while 循环中检查条件，而不是 if。

3. **信号量和条件变量的区别。** 信号量存"状态"（值），条件变量不存。信号量 `post` 时即使没人 wait，值也会 +1；下次 `wait` 直接通过。条件变量 `signal` 时如果没人 `wait`，信号就丢了。

4. **析构顺序。** 确保所有线程 `pthread_join` 结束之后再销毁锁和信号量，否则线程可能在已销毁的对象上操作。
---

## 阶段小结

你封装了三个同步原语（mutex、semaphore、cond），并用 `locker.h` 驱动了一个生产者-消费者程序。这份 `locker.h` 将在后续几乎所有模块中被引用——它是整个项目的"地基"。

下一阶段：**阻塞队列与日志系统**——在 `locker.h` 之上构建线程安全的阻塞队列，再基于阻塞队列实现异步日志。
