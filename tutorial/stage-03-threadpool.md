# Stage 3：线程同步与线程池

> 🎯 **本阶段目标**：实现 TinyWebServer 的两大基础模块 ——
> `lock/locker.h`（线程同步封装）和 `threadpool/threadpool.h`（线程池），
> 并用它把 HTTP 服务器升级为**多线程版**：多个客户端同时连接都能被服务。
>
> 从本阶段开始，代码写入 `my_tiny_webserver/` 的正式目录结构，用 CMake 构建。

## 📚 理论铺垫

### 3.1 为什么需要线程池？

Stage 1 的实验已经证明：单线程阻塞模型一次只能服务一个客户端。
最直观的解法是「**每个连接来一个线程**」（thread-per-connection）：

```
main 线程 accept → 为 connfd 创建新线程 → 新线程处理请求 → 线程退出
```

问题：连接数上万时，创建/销毁线程的开销和线程数量本身就把系统拖垮了。

**线程池（thread pool）**的思路：预先创建固定数量（如 8 个）的工作线程，
连接来了不入新线程，而是把「任务」扔进一个队列，工作线程从队列里抢任务干：

```
              ┌─────────────────┐
accept 主线程  │  任务队列(list)  │ ← 工作线程 1
   └───────→ │ [req][req][req] │ ← 工作线程 2   (谁抢到谁干)
              └─────────────────┘ ← 工作线程 N
```

这就是经典的**生产者-消费者模型**，也是 TinyWebServer「半同步/半反应堆」模式的基础：
主线程（反应堆/异步层）负责接收事件，工作线程（同步层）负责处理任务。

### 3.2 多线程的麻烦：共享数据要加锁

任务队列被「主线程放任务」和「N 个工作线程取任务」同时访问，不加保护就会数据错乱。
Linux 提供三种基本同步工具，TinyWebServer 把它们各自封装成类（`lock/locker.h`）：

| 工具 | 系统调用 | 用途 | 项目中的封装类 |
|------|----------|------|----------------|
| 互斥锁 mutex | `pthread_mutex_*` | 同一时刻只允许一个线程访问共享数据 | `locker` |
| 条件变量 cond | `pthread_cond_*` | 线程等待某个条件成立（队列非空） | `cond` |
| 信号量 sem | `sem_*` | 计数器：表示"还有几个资源" | `sem` |

一个类比：队列是厕所，mutex 是门锁，cond 是门口排队等位的人，
信号量是「还剩几个空位」的计数牌。

### 3.3 RAII：C++ 管理资源的核心思想

注意 `locker` 类的设计：**构造函数里 `pthread_mutex_init`，析构函数里 `pthread_mutex_destroy`**。
这就是 RAII（Resource Acquisition Is Initialization）：把资源的生命周期绑在对象的生命周期上，
对象创建时拿到资源，对象析构时自动释放 —— 不怕忘记 destroy、不怕中途 return 泄漏。

这个思想在 C++ 里无处不在：锁、文件、内存、数据库连接（Stage 8 的 `connectionRAII`）都是这么管的。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| 类、构造函数/析构函数、RAII | `locker`/`cond`/`sem` 三个封装类 |
| 类模板 `template<typename T>` | `threadpool<T>` —— 池子不关心任务是什么类型 |
| `std::list`（链表容器） | 任务队列 `m_workqueue` |
| `static` 成员函数、成员函数指针 | 工作线程入口 `worker`/`run` |
| pthread 线程 API | `pthread_create` / `pthread_detach` |
| 头文件保护 `#ifndef/#define/#endif` | 所有 .h 文件 |

## 🔨 动手实现

### 3.1 线程同步封装 `lock/locker.h`

在 `my_tiny_webserver/lock/` 下创建 `locker.h`：

```cpp
#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <semaphore.h>
#include <exception>

// 信号量封装
class sem {
public:
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0) throw std::exception();
    }
    sem(int num) {                          // 指定初始值，如"8 个数据库连接"
        if (sem_init(&m_sem, 0, num) != 0) throw std::exception();
    }
    ~sem() { sem_destroy(&m_sem); }
    bool wait() { return sem_wait(&m_sem) == 0; }   // 信号量 -1，为 0 则阻塞
    bool post() { return sem_post(&m_sem) == 0; }   // 信号量 +1，唤醒等待者
private:
    sem_t m_sem;
};

// 互斥锁封装
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) throw std::exception();
    }
    ~locker() { pthread_mutex_destroy(&m_mutex); }
    bool lock()   { return pthread_mutex_lock(&m_mutex) == 0; }
    bool unlock() { return pthread_mutex_unlock(&m_mutex) == 0; }
    pthread_mutex_t* get() { return &m_mutex; }    // cond 需要配合原生 mutex
private:
    pthread_mutex_t m_mutex;
};

// 条件变量封装
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) throw std::exception();
    }
    ~cond() { pthread_cond_destroy(&m_cond); }
    bool wait(pthread_mutex_t* mutex) {             // 原子地"释放锁+等待"
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }
    bool timewait(pthread_mutex_t* mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }
    bool signal()     { return pthread_cond_signal(&m_cond) == 0; }    // 唤醒一个
    bool broadcast()  { return pthread_cond_broadcast(&m_cond) == 0; } // 唤醒全部
private:
    pthread_cond_t m_cond;
};

#endif
```

> 📝 与原始项目对照：这正是仓库 `lock/locker.h` 的内容（类名略有简化的版本也能用）。
> 写完自己 `diff` 一下：`diff lock/locker.h ../../lock/locker.h`。

### 3.2 线程池 `threadpool/threadpool.h`

在 `my_tiny_webserver/threadpool/` 下创建 `threadpool.h`。先写一个**简化版**
（暂不考虑 Reactor/Proactor 区分，Stage 9 再补全 `m_actormodel` 相关逻辑）：

```cpp
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include "../lock/locker.h"

template <typename T>
class threadpool {
public:
    // thread_number: 工作线程数；max_requests: 队列最大长度
    threadpool(int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request);        // 生产者：把任务放入队列

private:
    static void* worker(void* arg); // pthread_create 要求的入口形式
    void run();                     // 消费者：每个工作线程都在跑这个循环

private:
    int m_thread_number;
    int m_max_requests;
    pthread_t* m_threads;           // 线程 ID 数组
    std::list<T*> m_workqueue;      // 任务队列
    locker m_queuelocker;           // 保护队列的互斥锁
    sem m_queuestat;                // 信号量：队列里有几个任务
    bool m_stop;
};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests)
    : m_thread_number(thread_number), m_max_requests(max_requests),
      m_threads(NULL), m_stop(false) {
    if (thread_number <= 0 || max_requests <= 0) throw std::exception();

    m_threads = new pthread_t[m_thread_number];
    for (int i = 0; i < m_thread_number; ++i) {
        // 创建线程，把 this 传进去，让静态函数能访问成员变量
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]) != 0) {   // 分离：线程退出自动回收
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T* request) {
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests) {   // 队列满了，拒绝
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();       // 信号量 +1，唤醒一个等待的工作线程
    return true;
}

template <typename T>
void* threadpool<T>::worker(void* arg) {
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        m_queuestat.wait();          // 信号量 -1：没任务就阻塞在这里
        m_queuelocker.lock();
        if (m_workqueue.empty()) {   // 防御性检查
            m_queuelocker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request) continue;

        request->process();          // ★ 干活：任务对象自己知道怎么处理
    }
}

#endif
```

**关键设计点**（也是面试高频问题）：

1. **为什么 `worker` 是 static？** `pthread_create` 需要 C 风格的函数指针，
   而普通成员函数自带隐含的 `this` 指针，类型不匹配。static 成员函数没有 `this`，
   所以把 `this` 作为 `arg` 显式传进去。
2. **为什么信号量和互斥锁都要？** mutex 保护队列的读写不冲突；
   信号量负责「任务数量」的计数和线程唤醒。两者职责不同。
3. **`request->process()`** 体现了「任务对象自带行为」的设计：
   线程池不需要知道任务是 HTTP 请求还是别的，只要能 `process()` ——
   这就是用模板 `T` 的原因。

### 3.3 搭建正式工程的 CMakeLists.txt

在 `my_tiny_webserver/` 根目录创建 `CMakeLists.txt`（后续阶段逐步往里加文件）：

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_tiny_webserver CXX)

set(CMAKE_CXX_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_executable(server
    main.cpp
)

target_link_libraries(server pthread)
```

### 3.4 用线程池改造 HTTP 服务器 `main.cpp`

思路：把 Stage 2 的 `handle_request` 包装成一个任务类，accept 后扔进线程池。

在 `my_tiny_webserver/` 创建 `main.cpp`：

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <string>
#include "threadpool/threadpool.h"

const int PORT = 9006;
const int BUF_SIZE = 4096;
const char* DOC_ROOT = "./root";

// 任务类：一个 http_task 对应一个客户端连接
class http_task {
public:
    void init(int fd) { m_fd = fd; }

    void process() {                  // 线程池会调用这个函数
        char buf[BUF_SIZE] = {0};
        int n = read(m_fd, buf, BUF_SIZE - 1);
        if (n <= 0) { close(m_fd); return; }

        std::string request(buf);
        size_t sp1 = request.find(' ');
        size_t sp2 = request.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            close(m_fd); return;
        }
        std::string url = request.substr(sp1 + 1, sp2 - sp1 - 1);
        if (url == "/") url = "/index.html";

        std::string path = std::string(DOC_ROOT) + url;
        struct stat st;
        std::string body;
        int status = 200;
        if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            FILE* fp = fopen(path.c_str(), "rb");
            body.resize(st.st_size);
            fread(&body[0], 1, st.st_size, fp);
            fclose(fp);
        } else {
            status = 404;
            body = "<html><body><h1>404</h1></body></html>";
        }

        char header[BUF_SIZE];
        snprintf(header, BUF_SIZE,
                 "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                 status, status == 200 ? "OK" : "Not Found", body.size());
        write(m_fd, header, strlen(header));
        write(m_fd, body.data(), body.size());
        close(m_fd);
    }

private:
    int m_fd;
};

const int MAX_FD = 1000;

int main() {
    threadpool<http_task>* pool = NULL;
    try {
        pool = new threadpool<http_task>(8, 10000);
    } catch (...) {
        return 1;
    }

    http_task* tasks = new http_task[MAX_FD];   // 按 fd 索引任务对象

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }
    printf("thread-pool http server on port %d\n", PORT);

    while (true) {
        sockaddr_in client;
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd, (sockaddr*)&client, &len);
        if (connfd < 0) continue;
        if (connfd >= MAX_FD) { close(connfd); continue; }

        printf("new connection fd=%d\n", connfd);
        tasks[connfd].init(connfd);
        pool->append(&tasks[connfd]);   // 扔进池子，立即返回继续 accept
    }
    return 0;
}
```

> ⚠️ 这个版本有一个隐藏的 bug：如果客户端保持连接不发数据，主线程 read 不到数据
> 会……等等，read 在工作线程里，所以主线程不受影响。但工作线程会被这个慢客户端
> 占住！8 个慢连接就能耗尽线程池。Stage 4 的 epoll 会根治这个问题。
> 现在先用「浏览器/curl 都是发完就等响应」的场景验证。

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer/my_tiny_webserver
cp -r ../root ./root        # 静态资源（若上一步没拷的话）
mkdir -p build && cd build
cmake .. && make
./server
```

## ✅ 验证

**验证 1：基本功能不变**

浏览器打开 `http://127.0.0.1:9006/` 正常显示；`curl -v http://127.0.0.1:9006/index.html` 返回 200。

**验证 2：并发能力（与 Stage 2 的关键区别）**

```bash
# 同时发 20 个请求，全部应该成功且很快
for i in $(seq 1 20); do curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:9006/ & done; wait
```

Stage 2 的服务器做同样实验会串行排队，现在 8 个线程并行处理。

**验证 3：观察线程**

```bash
ps -T -p $(pgrep -f './server') | head -15
# 应看到 1 个主线程 + 8 个工作线程（线程在创建时就全部存在了）
```

**验证 4：gdb 多线程初体验**

```bash
gdb ./server
(gdb) break main.cpp:60      # 在 pool->append 那行打断点（行号按你的代码调整）
(gdb) run
# 浏览器访问一次，触发断点后：
(gdb) info threads           # 查看所有线程
(gdb) thread 3               # 切换到 3 号线程
(gdb) bt                     # 看它的调用栈（应该在 run() 里等信号量）
```

## 🐛 常见问题

**Q1: 编译报 `undefined reference to pthread_create`？**
忘了链接 pthread。检查 CMakeLists.txt 里的 `target_link_libraries(server pthread)`。

**Q2: 程序一启动就挂或者偶发崩溃？**
典型原因是忘了初始化/销毁同步对象，或队列操作没加锁。
用 `gdb ./server` + `run`，崩溃后 `bt` 定位。也可以编译时加
`-fsanitize=thread`（CMake 里 `add_compile_options(-fsanitize=thread)` +
`add_link_options(-fsanitize=thread)`）检测数据竞争。

**Q3: 浏览器标签页开着，服务器工作线程全被占满？**
这正是 3.4 末尾说的缺陷。把浏览器标签页关掉释放连接即可，Stage 4 彻底解决。

## 🤔 思考与练习

1. 把线程数改成 1（`threadpool<http_task>(1, 10000)`），重做验证 2，感受差别。
2. 在 `run()` 的 `process()` 前后各加一行 `printf`（带 `pthread_self()`），
   观察 20 个并发请求是被哪些线程处理的 —— 验证「谁抢到谁干」。
3. 思考：`append()` 里为什么先 `unlock()` 再 `post()`？反过来行不行？
   （提示：可以反过来，但先解锁能减少唤醒后再等锁的争抢。）
4. 阅读参考答案 `threadpool/threadpool.h`，找到比我们简化版多出的部分
   （`m_actormodel`、`append_p`），现在不用懂，Stage 9 会补上。
5. 思考：信号量换成条件变量 `cond` 能不能实现同样的唤醒效果？
   原始项目的日志模块（`log/block_queue.h`）用的就是条件变量，Stage 7 见。

---

➡️ 下一阶段：[Stage 4：epoll 与 Reactor 事件驱动](stage-04-epoll.md)
