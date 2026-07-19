# Stage 7：同步/异步日志系统

> 🎯 **本阶段目标**：实现 `log/` 模块 —— 一个支持**同步/异步**两种模式、
> 按天切分文件、分级输出的日志系统。它也是「生产者-消费者模型」的第二个实战
> （第一次是线程池）。

## 📚 理论铺垫

### 7.1 为什么需要日志系统？

服务器是 7×24 运行的守护程序，出问题时没有屏幕可看。日志就是它的"黑匣子"：
谁什么时候连上来、请求了什么、发生了什么错误。

### 7.2 同步 vs 异步：写日志的性能陷阱

**同步写日志**：`fprintf(fp, ...)` 直接写文件。问题：磁盘 I/O 很慢，
业务线程会卡在写日志上 —— 高并发时这是致命的。

**异步写日志**：业务线程只把日志**字符串放进内存队列**就返回（极快），
一个专门的日志线程在后台把队列里的日志逐条写进文件：

```
业务线程 1 ─┐
业务线程 2 ─┼─→ 阻塞队列(block_queue) ──→ 日志线程 → 写文件
业务线程 N ─┘    (生产者)                 (消费者)
```

这就是 Stage 3 生产者-消费者模型的翻版！区别只在同步原语：
线程池用「信号量」，日志队列用「互斥锁 + 条件变量」。

### 7.3 阻塞队列（block_queue）

基于**循环数组**实现的定长队列，配合条件变量：

```
push: 队列满 → 生产者等待(cond wait)；不满 → 放入尾部 → signal 唤醒消费者
pop:  队列空 → 消费者等待；不空 → 取出头部 → signal 唤醒生产者
```

为什么用循环数组而不是 std::list？定长数组内存连续、无动态分配，
且「队列满」本身是一种**背压（backpressure）机制**：日志产生太快时，
宁可让业务线程等一下或丢弃，也不能让内存无限膨胀。

### 7.4 单例模式（Singleton）

全局只需要一个日志系统。本项目用 C++11 推荐的**局部静态变量单例**：

```cpp
static Log* get_instance() {
    static Log instance;    // C++11 保证线程安全，且自动析构
    return &instance;
}
```

这是「懒汉式单例」最优雅的写法，比双重检查锁（DCLP）简单得多。

### 7.5 可变参数与宏

日志调用要像这样方便：`LOG_INFO("user %s login, fd=%d", name, fd)`。
实现靠两层：
- 可变参数函数：`vsnprintf(buf, fmt, arg_list)`（Stage 5 的 add_response 已用过）
- 变参宏：`#define LOG_INFO(format, ...) Log::get_instance()->write_log(1, format, ##__VA_ARGS__)`

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| 变参宏 `...` / `##__VA_ARGS__` | LOG_DEBUG 等四个宏 |
| 局部静态变量单例 | `Log::get_instance()` |
| 循环数组 | `block_queue` 底层存储 |
| 条件变量实战 | 阻塞队列的 full/empty 等待 |
| `va_list`/`vsnprintf` | 格式化日志内容 |
| `localtime`/`strftime` | 日志时间戳、按天切分文件 |

## 🔨 动手实现

### 7.1 `log/block_queue.h`：阻塞队列

```cpp
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

    bool full() {
        m_mutex.lock();
        bool ret = m_size >= m_max_size;
        m_mutex.unlock();
        return ret;
    }

    bool empty() {
        m_mutex.lock();
        bool ret = (0 == m_size);
        m_mutex.unlock();
        return ret;
    }

    // 生产者：往队尾放（满了返回 false，由上层决定丢弃）
    bool push(const T& item) {
        m_mutex.lock();
        if (m_size >= m_max_size) {
            m_cond.broadcast();       // 队列满：叫醒消费者赶紧消费
            m_mutex.unlock();
            return false;
        }
        m_back = (m_back + 1) % m_max_size;   // 循环数组：取模回绕
        m_array[m_back] = item;
        m_size++;
        m_cond.broadcast();           // 通知"队列不空了"
        m_mutex.unlock();
        return true;
    }

    // 消费者：从队头取，空了阻塞等待
    bool pop(T& item) {
        m_mutex.lock();
        while (m_size <= 0) {
            if (!m_cond.wait(m_mutex.get())) {   // ★ 经典套路
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

private:
    locker m_mutex;
    cond m_cond;
    T* m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
```

> 🔑 `while (m_size <= 0) cond.wait(...)` 必须用 **while** 不能用 if：
> 条件变量可能虚假唤醒（spurious wakeup），醒来后要重新检查条件。
> 这是条件变量使用的铁律，面试必考。

### 7.2 `log/log.h` + `log/log.cpp`：日志系统

**log.h 核心**：

```cpp
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

class Log {
public:
    static Log* get_instance() {
        static Log instance;
        return &instance;
    }

    // 异步写线程的入口（static + 传 this，和线程池同样的套路）
    static void* flush_log_thread(void* args) {
        Log::get_instance()->async_write_log();
        return args;
    }

    // 初始化：文件名、是否异步、队列大小、日志缓冲
    bool init(const char* file_name, int close_log,
              int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char* format, ...);
    void flush(void);

private:
    Log();
    virtual ~Log();
    void* async_write_log() {
        std::string single_log;
        while (m_log_queue->pop(single_log)) {   // 阻塞等日志
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
        return NULL;
    }

private:
    char dir_name[128];       // 目录名
    char log_name[128];       // 文件名
    int m_split_lines;        // 单个日志文件最大行数
    int m_log_buf_size;
    long long m_count;        // 已写行数
    int m_today;              // 按天切分
    FILE* m_fp;
    char* m_buf;
    block_queue<std::string>* m_log_queue;
    bool m_is_async;
    locker m_mutex;
    int m_close_log;
};

// 四个分级宏：close_log=1 时全部编译为空操作（零开销关闭日志）
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...)  if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...)  if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
```

**log.cpp 核心逻辑**：

```cpp
#include "log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <pthread.h>

Log::Log() {
    m_count = 0;
    m_is_async = false;
}

Log::~Log() {
    if (m_fp != NULL) fclose(m_fp);
}

// max_queue_size = 0 → 同步；> 0 → 异步并启动写线程
bool Log::init(const char* file_name, int close_log, int log_buf_size,
               int split_lines, int max_queue_size) {
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<std::string>(max_queue_size);
        pthread_t tid;
        pthread_create(&tid, NULL, flush_log_thread, NULL);  // 启动日志线程
    }
    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 文件名形如 ../log/2026_07_19_server.log（目录需提前创建）
    const char* p = strrchr(file_name, '/');
    char log_full_name[256] = {0};
    if (p == NULL) {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s",
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    } else {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name,
                 my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }
    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL) return false;
    return true;
}

void Log::write_log(int level, const char* format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm* sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    char s[16] = {0};
    switch (level) {
    case 0: strcpy(s, "[debug]:"); break;
    case 1: strcpy(s, "[info]:");  break;
    case 2: strcpy(s, "[warn]:");  break;
    case 3: strcpy(s, "[erro]:");  break;
    default: strcpy(s, "[info]:"); break;
    }

    m_mutex.lock();
    m_count++;
    // 跨天或超行数 → 切分新文件
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900,
                 my_tm.tm_mon + 1, my_tm.tm_mday);
        if (m_today != my_tm.tm_mday) {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        } else {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name,
                     m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }
    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);
    std::string log_str;

    m_mutex.lock();
    // 时间戳 + 级别
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    // 用户内容
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;
    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);    // 异步：入队就完事
    } else {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);  // 同步：直接写
        m_mutex.unlock();
    }
    va_end(valst);
}

void Log::flush(void) {
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}
```

### 7.3 接入服务器

在 main.cpp 的事件循环关键位置加日志：

```cpp
// 启动时（异步：队列 800）
mkdir("log", 0755);        // 或手动建目录
Log::get_instance()->init("./log/server.log", close_log, 2000, 800000, 800);

// 新连接
LOG_INFO("new connection fd=%d, user_count=%d", connfd, http_conn::m_user_count);

// 请求处理
LOG_INFO("%s %s %s", method_str, url, version);

// 定时器踢人
LOG_INFO("kick fd=%d", sockfd);
```

CMakeLists.txt 加入 `log/log.cpp`。

## ✅ 验证

**验证 1：同步模式日志生成**

```bash
./server                 # init 时 max_queue_size=0
curl http://127.0.0.1:9006/index.html
cat log/2026_07_19_server.log    # 按当天日期
# 期望：看到 [info]: new connection ... 等日志行，带微秒时间戳
```

**验证 2：异步模式**

init 改为 max_queue_size=800，重启服务器，压测一下：

```bash
ab -n 5000 -c 100 http://127.0.0.1:9006/index.html
# 期望：5000 条请求全部成功，日志文件内容完整、按顺序追加
```

**验证 3：对比同步/异步的性能差异**

```bash
# 分别用同步和异步模式跑压测，对比 QPS
ab -n 10000 -c 200 http://127.0.0.1:9006/index.html | grep "Requests per second"
# 期望：异步模式的 QPS 明显高于同步模式（磁盘 I/O 不再阻塞业务线程）
```

**验证 4：跨天/超行数切分（可选）**

把 `split_lines` 临时改为 100，压测后查看 log 目录：
应出现 `2026_07_19_server.log.1`、`.2` 等切分文件。

## 🐛 常见问题

**Q1: 日志文件没生成？**
`fopen` 的目录不存在 —— fopen 不会自动创建目录。先 `mkdir -p log`。

**Q2: 异步模式退出时丢日志？**
程序退出时队列里还有没写完的日志。学习阶段可接受；严谨做法是在退出前
等队列空（或给日志线程发退出标志，join 后再退）。

**Q3: 日志内容错乱（两行的内容混在一起）？**
m_buf 是共享的，写 buf 的两段 snprintf 必须**在同一把锁内**完成。
检查是否把锁的粒度拆错了。

**Q4: 变参宏报编译错误 `expected expression before ')' token`？**
当 LOG_INFO 只有 format 没有参数时，`##__VA_ARGS__` 的 `##` 是关键：
它让 `, ##__VA_ARGS__` 在参数为空时把前面的逗号也消掉。

## 🤔 思考与练习

1. 为什么 `block_queue::pop` 用 `while` 循环等待，而 `push` 满了却直接返回 false？
   （提示：生产者可以"丢日志"降级，消费者必须等有货 —— 设计哲学不同）
2. 用 `ps -T` 观察异步模式下多了一个日志线程；用 gdb `thread apply all bt`
   看所有线程在干什么。
3. 面试题：单例模式有哪几种写法？为什么局部静态变量是 C++11 后的最佳实践？
4. 拓展：现在的日志只有写文件。如何加一个「日志级别过滤」
   （比如线上只输出 WARN 以上）？在 write_log 里加个判断即可，动手试试。
5. 思考题：异步日志队列满了 push 返回 false，这条日志就被**悄悄丢弃**了。
   这个设计合理吗？什么场景下不能接受？（金融系统 vs Web 服务器）

---

➡️ 下一阶段：[Stage 8：MySQL 连接池与注册登录](stage-08-mysql.md)
