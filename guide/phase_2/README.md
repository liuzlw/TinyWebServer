# Phase 2 —— 阻塞队列与日志系统

## 目标

实现一个线程安全的**循环数组阻塞队列**，并基于它构建支持同步/异步写入的**单例日志类**。

**可见结果：** 程序运行时日志实时写入文件；日志文件按日期和行数自动分割（如 `2026_06_05_ServerLog`、`2026_06_05_ServerLog.1`）。

---

## 前置知识

- Phase 1 的 `locker.h`（互斥锁、信号量、条件变量）
- 单例模式的基本概念
- `FILE*`、`fputs` 等 C 文件操作函数

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | `add_library`（静态库）、`target_link_libraries`（链接静态库） |
| **gdb** | 条件断点（`break if`）、`watchpoint`（监视变量变化） |

---

## 分步实现

### Step 1：循环数组阻塞队列

阻塞队列 = 普通队列 + 满时阻塞生产者 + 空时阻塞消费者。

用**循环数组**而非链表实现：内存连续、无频繁 new/delete。

```cpp
// block_queue.h
#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

template <class T>
class block_queue {
public:
    block_queue(int max_size = 1000) {
        if (max_size <= 0) exit(-1);

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    ~block_queue() {
        m_mutex.lock();
        if (m_array != NULL) delete[] m_array;
        m_mutex.unlock();
    }

    // 生产者：往队尾加元素。队列满则返回 false
    bool push(const T& item) {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    // 消费者：从队头取元素。队列空则阻塞等待
    bool pop(T& item) {
        m_mutex.lock();
        while (m_size <= 0) {
            if (!m_cond.wait(m_mutex.get())) {
                m_mutex.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    // 带超时的 pop
    bool pop(T& item, int ms_timeout) {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();    
        if (m_size <= 0) {
            t.tv_sec  = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t)) {
                m_mutex.unlock();
                return false;
            }
        }
        if (m_size <= 0) {
            m_mutex.unlock();
            return false;
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_mutex.unlock();
        return true;
    }

    bool full() {
        m_mutex.lock();
        bool ret = (m_size >= m_max_size);
        m_mutex.unlock();
        return ret;
    }

    bool empty() {
        m_mutex.lock();
        bool ret = (m_size == 0);
        m_mutex.unlock();
        return ret;
    }

private:
    locker m_mutex;
    cond   m_cond;
    T*     m_array;
    int    m_size;
    int    m_max_size;
    int    m_front;
    int    m_back;
};

#endif
```

**核心设计：** 用 `m_cond` 条件变量实现阻塞。生产者发现队列满 → 返回 false（上层可选重试或丢弃）。消费者发现队列空 → `m_cond.wait()` 阻塞直到生产者 `broadcast()`。

### Step 2：同步日志类

日志类要做三件事：

1. **格式化**：给日志加时间戳、日志级别标签
2. **写文件**：追加写入文件
3. **分文件**：隔天或超过行数上限就切新文件

```cpp
// log.h
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"
#include "../lock/locker.h"

class Log {
public:
    // C++11 线程安全的懒汉单例
    static Log* get_instance() {
        static Log instance;
        return &instance;
    }

    bool init(const char* file_name, int close_log,
              int log_buf_size = 8192,
              int split_lines = 5000000,
              int max_queue_size = 0);

    void write_log(int level, const char* format, ...);
    void flush();

    // 异步日志的工作线程
    static void* flush_log_thread(void* args) {
        Log::get_instance()->async_write_log();
    }

private:
    Log();
    virtual ~Log();
    void* async_write_log();

    char    dir_name[128];
    char    log_name[128];
    int     m_split_lines;
    int     m_log_buf_size;
    long long m_count;
    int     m_today;
    FILE*   m_fp;
    char*   m_buf;
    block_queue<std::string>* m_log_queue;
    bool    m_is_async;
    locker  m_mutex;
    int     m_close_log;
};

// 四个级别的日志宏
#define LOG_DEBUG(format, ...) if(m_close_log == 0) { Log::get_instance()->write_log(0, format, ##__VA_ARGS__); }
#define LOG_INFO(format, ...)  if(m_close_log == 0) { Log::get_instance()->write_log(1, format, ##__VA_ARGS__); }
#define LOG_WARN(format, ...)  if(m_close_log == 0) { Log::get_instance()->write_log(2, format, ##__VA_ARGS__); }
#define LOG_ERROR(format, ...) if(m_close_log == 0) { Log::get_instance()->write_log(3, format, ##__VA_ARGS__); }

#endif
```

### Step 3：`init` 与 `write_log` 实现

```cpp
// log.cpp
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>

Log::Log() : m_count(0), m_is_async(false) {}

Log::~Log() {
    if (m_fp != NULL) fclose(m_fp);
}

bool Log::init(const char* file_name, int close_log,
               int log_buf_size, int split_lines, int max_queue_size)
{
    // 如果 max_queue_size >= 1，则开启异步模式
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<std::string>(max_queue_size);
        pthread_t tid;
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log   = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char* p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL) {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s",
                 my_tm.tm_year+1900, my_tm.tm_mon+1, my_tm.tm_mday, file_name);
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s",
                 dir_name, my_tm.tm_year+1900, my_tm.tm_mon+1,
                 my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;
    m_fp = fopen(log_full_name, "a");
    return m_fp != NULL;
}

void Log::write_log(int level, const char* format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // ---- 日志级别标签 ----
    char s[16] = {0};
    switch (level) {
        case 0: strcpy(s, "[debug]:"); break;
        case 1: strcpy(s, "[info]:");  break;
        case 2: strcpy(s, "[warn]:");  break;
        case 3: strcpy(s, "[erro]:");  break;
        default:strcpy(s, "[info]:");  break;
    }

    m_mutex.lock();
    m_count++;

    // ---- 检查是否需要切文件 ----
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_",
                 my_tm.tm_year+1900, my_tm.tm_mon+1, my_tm.tm_mday);

        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            snprintf(new_log, 255, "%s%s%s.%lld",
                     dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    // ---- 格式化日志内容 ----
    va_list valst;
    va_start(valst, format);

    std::string log_str;
    m_mutex.lock();
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year+1900, my_tm.tm_mon+1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;
    m_mutex.unlock();

    // ---- 写入：异步推入队列，同步直接写文件 ----
    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    } else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush() {
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}

void* Log::async_write_log() {
    std::string single_log;
    while (m_log_queue->pop(single_log)) {
        m_mutex.lock();
        fputs(single_log.c_str(), m_fp);
        m_mutex.unlock();
    }
}
```

### Step 4：cmake 静态库

将 `block_queue.h`、`log.h`、`log.cpp`、`locker.h` 编译成静态库供后续模块使用：

```cmake
cmake_minimum_required(VERSION 3.10)
project(LogDemo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

# 静态库：只编译，不链接成可执行文件
add_library(log_lib STATIC
    log.cpp
)

# 测试程序
add_executable(test_log test_log.cpp)
target_link_libraries(test_log log_lib pthread)
```

```cpp
// test_log.cpp
#include "log/log.h"

int close_log = 0;  // 0=开启日志

int main() {
    Log::get_instance()->init("./TestLog", close_log,
                               8192, 100, 10);  // 每 100 行切文件

    for (int i = 0; i < 500; ++i) {
        LOG_INFO("This is log line %d", i);
    }
    return 0;
}
```

运行后你应该看到：
- `2026_06_05_TestLog`
- `2026_06_05_TestLog.1`
- `2026_06_05_TestLog.2`
- `2026_06_05_TestLog.3`
- `2026_06_05_TestLog.4`

### Step 5：同步 vs 异步对比

`init` 的最后一个参数 `max_queue_size` 控制模式：

- `max_queue_size = 0`：**同步**——每条日志立即 `fputs` 写文件。简单，但写磁盘时业务线程会卡住。
- `max_queue_size >= 1`：**异步**——日志推入阻塞队列，后台线程专门刷盘。业务线程不卡，但有一条队列长度的延迟。

验证异步模式：把 `max_queue_size` 改成 10，重新运行测试程序，比较两次输出的时间。

### Step 6：gdb 条件断点 & watchpoint

```bash
gdb ./build/test_log

(gdb) break Log::write_log if level==3
# 只在 ERROR 级别日志时停下

(gdb) watch m_count
# 监视 m_count 变量，每次值变化时自动暂停
```

---

## 验证方法

- [ ] 运行后生成日志文件，内容包含时间戳和级别标签
- [ ] 日志行数达到 `split_lines` 后自动切新文件
- [ ] 手动改系统日期测试隔天切文件
- [ ] `max_queue_size >= 1` 时异步日志性能优于同步（可计时对比）

---

## 踩坑记录

1. **单例的线程安全。** C++11 起 `static Log instance;` 的初始化是线程安全的（Magic Statics），不需要自己加锁。但 C++98 不是。

2. **`LOG_XXX` 宏里的 `if`。** 宏定义中的 `if(m_close_log == 0)` 是为了在关闭日志时跳过 `write_log` 的参数计算，减少性能开销。

3. **异步日志的线程退出。** 这个版本的异步日志线程是用 `while (pop(...))` 循环的，`pop` 会无限阻塞。在生产代码中，需要一种优雅的退出机制（比如设置一个 `m_stop` 标志）。本项目用信号处理来终止进程，不是优雅退出。

4. **`vsnprintf` 的返回值。** Windows 和 Linux 行为不同！Linux 下返回"如果缓冲区够大本应写入的字符数"，Windows 下缓冲区不足时返回 -1。注意本教程只针对 Linux。

---

## 阶段小结

你完成了两个关键基础设施：

- `block_queue<T>`：一个线程安全的、基于循环数组的阻塞队列（生产者-消费者模型的直接产物）
- `Log`：一个支持同步/异步写入、自动分文件的单例日志系统

这两个模块将在后续的线程池、服务器主循环中被直接使用。

下一阶段：**MySQL 连接池**——管理与数据库的连接资源，并引入 RAII 自动归还机制。
