# Stage 3 锁 + 线程池

> 让服务器从"一次只处理一个连接"变成"多个连接并发处理"。这一阶段的 `locker.h` 和 `threadpool.h` 直接对应原项目同名模块。

## 1. 本阶段目标

- [ ] 理解 S2 单线程服务器的局限
- [ ] 看懂 `lock/locker.h`:pthread 锁的 RAII 封装
- [ ] 看懂 `threadpool/threadpool.h`:类模板 + 半同步半反应堆模型
- [ ] 用线程池改造 echo 服务器,支持并发连接

**最终效果:** 同时开多个 `nc` 连接,都能独立回显(而不是排队等前一个处理完)。

## 2. 前置知识

- C4:类模板(`threadpool<T>`)
- C5:线程、互斥锁、信号量
- S1/S2:socket 流程

## 3. 问题:S2 为什么只能一次处理一个连接

S2 的循环是:

```text
accept 等一个连接 → 读请求 → 回响应 → close → 回到 accept 等下一个
```

`read()` 是**阻塞**的:处理连接 A 期间,如果 A 迟迟不发数据,`read` 卡在那里,连接 B 根本轮不到 accept。**一个慢客户端拖死整个服务器。**

线程池的解法:

```text
主线程:accept 到连接 → 包装成任务 → 丢进任务队列(立刻回来 accept 下一个)
工作线程×N:排队从队列取任务 → 处理(读/回显)→ 取下一个
```

主线程只负责"接客",工作线程负责"干活",彼此不互相等。这就是**半同步/半反应堆**:主线程同步接客,工作线程同步干活,中间用任务队列解耦。

## 4. lock/locker.h:锁的 RAII 封装

先看 C5 的原始用法:`mtx.lock()` / `mtx.unlock()` 手写配对,容易忘 unlock。原项目把三种同步原语各封装成一个类,靠析构自动释放。

在 `my_tiny_webserver/` 下新建 `lock/locker.h`(**和原仓库同名同内容**):

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
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        return pthread_cond_wait(&m_cond, m_mutex) == 0;
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
    pthread_cond_t m_cond;
};
#endif
```

**讲解:**

| 类 | 封装 | 用法 |
|---|---|---|
| `sem` | POSIX 信号量 `sem_t` | 构造 `sem_init`,析构 `sem_destroy`;`wait()`=P 操作,`post()`=V 操作 |
| `locker` | POSIX 互斥锁 `pthread_mutex_t` | 构造 `pthread_mutex_init`,析构 `pthread_mutex_destroy`;`lock()`/`unlock()` |
| `cond` | 条件变量 `pthread_cond_t` | `wait(互斥锁)` 等待,`signal()` 唤醒一个,`broadcast()` 唤醒全部 |

**这是 C3 学过的 RAII**:三个类都在构造时获取资源、析构时释放资源。用法:

```cpp
locker m_queuelocker;
m_queuelocker.lock();      // 上锁
// 临界区……
m_queuelocker.unlock();    // 解锁
```

> **注意 `sem` 的构造失败处理**:`sem_init` 返回非 0 就 `throw std::exception()`,直接抛异常中断——资源初始化失败不能悄悄继续。`locker`、`cond` 同理。

## 5. threadpool/threadpool.h:类模板线程池

在 `my_tiny_webserver/` 下新建 `threadpool/threadpool.h`。**这是简化版**(去掉了原项目的 MySQL 连接池和 actor 模型,保留线程池核心;差异见第 10 节):

```cpp
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"

template <typename T>
class threadpool
{
public:
    /* thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量 */
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *request);

private:
    /* 工作线程运行的函数，它不断从工作队列中取出任务并执行之 */
    static void *worker(void *arg);
    void run();

private:
    int m_thread_number;        //线程池中的线程数
    int m_max_requests;         //请求队列中允许的最大请求数
    pthread_t *m_threads;       //描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue; //请求队列
    locker m_queuelocker;       //保护请求队列的互斥锁
    sem m_queuestat;            //是否有任务需要处理
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests)
    : m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
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
    m_queuestat.post();     // 通知：有一个任务了
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
        m_queuestat.wait();              // 等有任务
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();

        request->process();              // 处理任务
        delete request;                  // 任务完成，释放
    }
}
#endif
```

**核心机制,按数据流讲:**

```text
                主线程                        工作线程(8 个,同时存在)
   accept 到连接 ──► new Task ──► append()
                                  │
                                  ├─ m_queuelocker.lock()      ← 保护队列
                                  ├─ push 进 m_workqueue
                                  ├─ unlock()
                                  └─ m_queuestat.post() ──────► m_queuestat.wait() 被唤醒
                                                                      │
                                                              lock → front() → pop
                                                                      │
                                                              unlock → task->process() → delete
                                                                      │
                                                              (回到 wait,等下一个)
```

| 成员 | 作用 | 为什么 |
|---|---|---|
| `std::list<T *> m_workqueue` | 任务队列 | 多个线程抢任务,`list` 插入/删除都快 |
| `locker m_queuelocker` | 保护队列的锁 | 队列是共享数据,同时 push/pop 会竞争 |
| `sem m_queuestat` | 任务计数器 | **初始值 0**:没有任务时 `wait()` 阻塞,有任务 `post()` 才放行。这就是"半反应堆"里工作线程等待的方式 |
| `pthread_create(m_threads+i, NULL, worker, this)` | 创建第 i 个工作线程 | 把 `this` 传给 `worker`,线程才能访问池的成员 |
| `pthread_detach` | 分离线程 | 分离后线程结束自动回收资源,主线程不用 join(线程永不退出,`run` 死循环) |
| `worker` 是 `static` | 静态成员函数 | `pthread_create` 需要 C 函数指针,静态成员函数才能被当作普通函数指针传 |
| `request->process()` | 调用任务的处理方法 | **这就是模板的威力**:任务类型由 `T` 决定,线程池不关心任务内部怎么实现 |

> **`worker` 为什么要 static?** `pthread_create` 的回调必须是无成员对象的 C 风格函数指针。静态成员函数没有 `this` 隐藏参数,才能匹配。它收到 `this` 后调 `pool->run()`——这是"把对象指针塞进 C 回调"的经典手法,原项目也是这么做的。

## 6. main.cpp:改造为线程池版

**替换 `my_tiny_webserver/main.cpp`**:

```cpp
// main.cpp —— 线程池版 echo 服务器(Stage 3)
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "lock/locker.h"
#include "threadpool/threadpool.h"

const int PORT = 9006;
const int THREAD_NUM = 8;

// 一个连接任务:负责读数据并回显
class EchoTask {
public:
    EchoTask(int fd) : connfd(fd) {}
    void process() {
        char buf[1024];
        ssize_t n;
        while ((n = read(connfd, buf, sizeof(buf))) > 0) {
            write(connfd, buf, n);
        }
        close(connfd);
        std::cout << "  处理完一个连接" << std::endl;
    }
private:
    int connfd;
};

int main() {
    // 线程池:8 个工作线程
    threadpool<EchoTask> pool(THREAD_NUM);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }
    std::cout << "线程池 echo 服务器已启动, 监听端口 " << PORT << " 线程数 " << THREAD_NUM << std::endl;

    while (true) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd, (struct sockaddr *)&client, &len);
        if (connfd < 0) { perror("accept"); continue; }
        std::cout << "接受到连接: " << inet_ntoa(client.sin_addr)
                  << ":" << ntohs(client.sin_port) << std::endl;

        // 把连接包装成任务,丢进线程池
        EchoTask *task = new EchoTask(connfd);
        if (!pool.append(task)) {
            std::cout << "任务队列已满, 丢弃连接" << std::endl;
            delete task;
            close(connfd);
        }
    }
    close(listenfd);
    return 0;
}
```

**关键变化(对比 S2):**

1. **accept 后不处理,直接丢队列**:`EchoTask *task = new EchoTask(connfd); pool.append(task);` 主线程立刻回 accept 等下一个连接
2. **处理逻辑挪进 `EchoTask::process()`**:工作线程调用它
3. **`threadpool<EchoTask> pool(8)`**:模板实例化,任务类型是 `EchoTask`
4. **队列满的处理**:`append` 返回 `false`,就释放任务、关闭连接——不能让任务泄漏

> **关于 `new EchoTask`**:任务在堆上创建,工作线程 `process()` 后 `delete`(见 threadpool 的 `run()`)。主线程 new、工作线程 delete,跨线程的生命周期传递——要保证 `append` 失败时也 delete(代码里做了)。

## 7. 更新 CMakeLists.txt

线程池用到了 `pthread_create`,必须链接 pthread 库:

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp)
target_link_libraries(server pthread)
```

> `locker.h`、`threadpool.h` 是**头文件**(模板实现都在头里),不用写进 `add_executable`——`#include` 会自己把它们带进来。这也再次说明:模板实现必须放头文件(C4 学过)。

## 8. 编译与运行

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

**预期输出:**

```text
线程池 echo 服务器已启动, 监听端口 9006 线程数 8
```

**另开两个终端,同时测:**

```bash
# 终端 A
nc 127.0.0.1 9006
# 终端 B
nc 127.0.0.1 9006
```

两个终端**同时**输入文字,都能得到回显——而不是终端 A 卡住时 B 无响应。

## 9. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `cmake --build build && ./build/server` | 输出启动信息 | ☐ |
| 2 | 开 **两个** nc 同时连,都输入文字 | 两个都回显(并行而非排队) | ☐ |
| 3 | 连续快速开 6 个 `printf "x\n" | nc ...` 连接 | 全部回显,服务器日志有条记录 | ☐ |
| 4 | 查看线程数:`ps -T -p <服务器PID>`(或 `ls /proc/<PID>/task \| wc -l`) | 9 个线程(1 主线程 + 8 工作线程) | ☐ |
| 5 | **验证主线程不阻塞**:一个 nc 连接建立后不输入任何字(悬着),另一个 nc 连接 | 第二个连接仍能正常回显 | ☐ |

> 第 5 条是**最重要的一条**:它证明了"慢客户端不再拖死服务器"。S2 单线程版做不到——A 连接悬着,B 就永远等不到 accept。

## 10. 调试技巧

### gdb 查看线程

线程池程序是多线程,用 gdb 能看到所有线程:

```bash
gdb ./build/server
```

```text
(gdb) break main.cpp:73      ← 断在 pool.append(task) 附近
(gdb) run
(gdb) info threads           ← 列出所有线程,应能看到 9 个
```

**预期输出(节选):**

```text
  9 Thread 0x7f... (running) worker() at threadpool/threadpool.h:129
  8 Thread 0x7f... (running) worker() at threadpool/threadpool.h:129
  ...
* 1 Thread 0x7f... (running) main() at main.cpp:76
```

`*` 标记当前线程,8 个 `worker()` 线程都停在 `m_queuestat.wait()` 上睡觉——**没任务时工作线程就是挂起状态**,这就是"半同步"的体现。

| gdb 命令 | 用途 |
|---|---|
| `info threads` | 列出所有线程 |
| `thread 3` | 切换到线程 3 |
| `bt` | 看当前线程的调用栈 |
| `print m_workqueue.size()` | 看任务队列长度(需在 pool 作用域) |

## 11. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 编译报 `undefined reference to pthread_create` | 忘了链接 pthread | `target_link_libraries(server pthread)`(CMake)或 `g++ -pthread` |
| 编译警告 `-Wsign-compare` | `size()` 返回无符号数,和 int 比较 | **正常,不影响运行**,原项目同样存在。想消除可改 `(size_t)m_max_requests` |
| 服务器一启动就崩 | 线程创建失败 | 检查线程数参数 > 0;或资源不足 `pthread_create` 失败 |
| 连接被丢弃 | 任务队列满了 | `m_max_requests` 默认 10000,一般不会;看到"任务队列已满"说明压力过大 |
| 退出时崩溃 | 线程池析构时工作线程还在跑 | 本阶段服务器常驻运行,析构逻辑简单(只释放线程数组);S9 再做优雅退出 |

## 12. 与原项目对照

| 本阶段 | 原项目对应 |
|---|---|
| `lock/locker.h` | **逐字一致** | 
| `threadpool/threadpool.h`(简化版) | 原版在**同骨架上加了 3 样东西**:MySQL 连接池成员 `m_connPool`(S8)、actor 模型分支 `m_actor_model`(S4)、`request->read_once()/write()/process()` 三种调用(S5) |
| `EchoTask::process()` | 原项目是 `http_conn::process()`,一个完整连接对象 |

> **diff 对比命令**(在原仓库根目录运行):
> ```bash
> diff my_tiny_webserver/lock/locker.h lock/locker.h              # 应无输出(完全一致)
> diff my_tiny_webserver/threadpool/threadpool.h threadpool/threadpool.h  # 会看到我们简化掉的部分
> ```

**刻意简化的原因**:原版 threadpool 强依赖 `sql_connection_pool.h` 和 `http_conn` 的接口,现在没学到。等你完成 S5 和 S8,第 9 阶段整合时会换上原版完整代码——到时候再看,那些"看不懂的东西"就全通了。

## 13. 下一步

进入 **[Stage 4 epoll 事件循环](stage-04-epoll.md)**——**本项目的灵魂**。线程池解决了并发,但每个工作线程读连接时还是阻塞的。epoll 让"等事件"这件事变得高效,是支撑上万并发的关键。
