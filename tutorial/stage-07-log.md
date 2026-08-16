# Stage 7：日志系统

> 本阶段目标：给服务器加上一套**同步/异步可切换**的日志系统，把 Stage 6 里散落的 `printf`（`timer tick`、`close fd` 等）替换成规范的 `LOG_INFO` / `LOG_ERROR`，并按**天、按行数**自动切分日志文件。核心是理解**生产者-消费者模型**与**单例模式**：异步模式下，请求线程只负责"把日志丢进队列"，一个专职线程负责"从队列取出并落盘"，从而让磁盘 IO 不再拖慢请求处理。

学完本阶段，你会掌握：单例模式、带变参的宏、变参函数（`va_list`）、模板阻塞队列（循环数组 + 条件变量）、`FILE*` 缓冲写、时间格式化。

---

## 前置要求

- 已完成 [Stage 6：定时器](stage-06-timer.md)，工作区状态如下（本阶段起点，**必须与之一致**）：

```text
my_tiny_webserver/
├── lock/locker.h                 # 与仓库一致（sem / locker / cond）
├── threadpool/threadpool.h       # 简化版线程池
├── http/http_conn.h
├── http/http_conn.cpp            # 内部用 printf 代替 LOG_*
├── timer/lst_timer.h
├── timer/lst_timer.cpp
├── server.cpp                    # Stage 6 集成定时器后的平铺版主程序
├── root/index.html
└── makefile
```

- 本阶段会用到 `lock/locker.h` 里的 `locker` 与 `cond`（Stage 3 已写好，含 `cond::wait(pthread_mutex_t*)`、`cond::timewait`、`cond::broadcast`）。确认它们存在即可。
- 前置知识：互斥锁、条件变量（Stage 3）；`printf` 族（Stage 5 用过变参的 `add_response`）。

---

## 理论学习

### 1. 为什么服务器需要日志

程序一旦跑起来就"黑盒"了：请求怎么进来的、为什么 404、哪个连接被超时关闭、有没有报错——没有记录就只能靠猜。日志是服务器**唯一可信的"黑匣子"**，用于：

- **排障**：出问题时回放"当时发生了什么"；
- **监控**：看访问量、错误率；
- **审计**：谁在什么时候做了什么。

本项目的日志至少记录：时间（精确到微秒）、级别（debug/info/warn/error）、具体内容。一行形如：

```text
2025-01-15 14:30:05.123456 [info]: timer tick
```

### 2. 同步写 vs 异步写：磁盘 IO 是"慢动作"

`printf` 到终端很快，但**写文件是磁盘 IO**，比内存操作慢好几个数量级。如果**请求线程自己**直接 `fputs` + `fflush` 落盘（同步写），那么每次打日志的磁盘延迟都会**叠加到请求处理时间里**，拖慢所有请求：

```text
同步写：请求线程 ──写日志(磁盘IO，慢)──▶ 才能继续处理请求
```

**异步写**的思路：请求线程只把日志字符串 `push` 进一个**内存队列**（很快），立刻返回继续干活；另起一个**专职日志线程**慢慢从队列 `pop` 并落盘：

```text
异步写：请求线程 ──push(内存，快)──▶ 队列 ──pop──▶ 日志线程 fputs(磁盘IO)
                                    （缓冲区）         （专职，不阻塞请求）
```

代价是：极端情况下（进程突然退出）队列里还没落盘的日志会丢（见思考题 1）；以及日志顺序与多线程执行顺序可能略有交错。

### 3. 生产者-消费者模型

异步日志是经典的**生产者-消费者模型**：

```text
                ┌───────────────────────────┐
 生产者           │     block_queue<string>    │           消费者
 （多个请求线程）   │     有界循环数组缓冲区       │          （1 个日志线程）
 LOG_INFO ──push─▶│  ┌──┬──┬──┬──┬──┬──┐      │──pop──▶ fputs 到文件
                  │  └──┴──┴──┴──┴──┴──┘      │
                  └───────────────────────────┘
```

- **生产者** = 所有调用 `LOG_INFO/LOG_ERROR` 的线程（主线程 + 线程池工作线程），负责 `push`；
- **消费者** = `Log` 启动的那一个写日志线程，负责 `pop` 并落盘；
- **阻塞队列** = 两者之间的缓冲区，用互斥锁保证不破坏数据，用条件变量保证"满时不硬塞、空时不空取"：
  - 队列**满**：生产者 `push` 失败（本项目直接退回同步写，保证日志不丢）；
  - 队列**空**：消费者 `pop` 阻塞等待（`cond_wait`），有数据才醒。

生产者和消费者速度不匹配时，队列起"蓄水池"作用，两边解耦、互不拖累。

### 4. 单例模式：为什么日志全局唯一

一个进程里**只需要一个日志对象**：文件句柄、缓冲区、锁、队列都应只有一份。若每个请求线程都 `new Log()`，就会各自打开各自文件、各自计数，日志四分五裂还互相覆盖。单例模式保证**全局唯一实例 + 一个全局访问点**（`Log::get_instance()`）。

### 5. 日志滚动：按天、按行数切分

日志无限增长会撑爆磁盘，所以到点就"切"成新文件。本项目两种切分规则（可同时生效）：

- **按天**：新的一天到来，切到 `2025_01_16_ServerLog`（文件名前缀带日期）；
- **按行数**：写满 `split_lines` 行，切出 `..._ServerLog.1`、`..._ServerLog.2` 这样的递增后缀文件。

"滚动（rolling）"就是"当前文件写到阈值 → 关掉旧文件 → 开新文件"的过程。

---

## 本阶段 C++ 知识点

### 1. 单例模式：局部静态变量的"懒汉式"

```cpp
class Log {
public:
    static Log *get_instance()
    {
        static Log instance;   // 局部静态变量
        return &instance;
    }
private:
    Log();                     // 构造私有：禁止外部 new
    virtual ~Log();
};
```

- **懒汉式**：第一次调用 `get_instance()` 时才创建实例（懒加载）。
- **为什么线程安全**：C++11 起，**局部静态变量的初始化由编译器保证只执行一次**，且多线程同时首次进入时也安全（标准称"magic static"，俗称"魔法静态变量"）。所以这里**不需要手动加锁**，仓库注释也点明了这点（`//C++11以后,使用局部变量懒汉不用加锁`）。
- **构造私有**：外界无法 `new Log`，只能通过 `get_instance()` 拿到唯一实例。
- 需要 `-std=c++11` 及以上。Ubuntu 22.04 的 g++ 11 默认 `-std=gnu++17`，满足。

### 2. 宏定义：`#define LOG_INFO(format, ...)`

```cpp
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...)  if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...)  if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}
```

- **`...` 与 `__VA_ARGS__`**：变参宏。`LOG_INFO("close fd %d", n)` 里 `format` 是 `"close fd %d"`，`__VA_ARGS__` 是 `n`。
- **`##__VA_ARGS__`**：GNU 扩展。当没有可变参数时（如 `LOG_INFO("timer tick")`），`##` 会**吞掉前面的逗号**，让展开结果 `write_log(1, "timer tick")` 而不是 `write_log(1, "timer tick", )`（后者多一个悬空逗号，编译报错）。
- **`if(0 == m_close_log)` 包裹**：日志开关。注意 `m_close_log` 是**调用处作用域**里的变量——在 `http_conn` 里是它的私有成员 `m_close_log`，在仓库 `WebServer` 里是 `WebServer::m_close_log`。宏只是文本替换，替换后才做名字查找，所以"谁用这个宏，谁就要提供一个 `m_close_log`"。这是本项目最容易踩的坑之一（见常见问题 1）。
- **为什么还要 `do { ... } while(0)`**：仓库用 `if(...){...}` 包裹，存在经典的"悬挂 else"隐患：`if (cond) LOG_INFO("x"); else foo();` 展开后 `else` 会错误地绑定到内层 `if(0==m_close_log)`。标准写法应把宏体包进 `do { ... } while(0)`，让宏成为一个**原子语句**。本教程忠实照抄仓库，但你应知道这一点（思考题 6）。
- **宏的优缺点**：宏展开快、能拿到"源码行号"等编译器信息，但没有类型检查、易出边界问题。生产级日志常用函数 + 模板/`__VA_ARGS__` 结合，本项目以教学为主，用宏。

### 3. 变参函数：`va_list` / `vsnprintf`

```cpp
void Log::write_log(int level, const char *format, ...)  // ... 表示可变参数
{
    // ...
    va_list valst;
    va_start(valst, format);                  // 从 format 之后开始取参
    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);  // 格式化进缓冲
    va_end(valst);                            // 收尾
}
```

- `va_list` 是"参数游标"，`va_start` 定位到最后一个**具名参数**（`format`）之后，`vsnprintf` 用 `format` 与游标拼出字符串，`va_end` 释放游标。
- `vsnprintf` 是 `snprintf` 的变参版，**不会越界**（第三个参数限定了最多写入的字节数，含结尾 `'\0'`）。
- 这与 Stage 5 里 `http_conn::add_response` 用到的 `vsnprintf` 是同一个函数，本阶段深入。

### 4. `FILE*` 与 `fputs` / `fflush` / `fopen("a")`

```cpp
m_fp = fopen(log_full_name, "a");   // "a" = append，追加写，文件不存在则创建
fputs(log_str.c_str(), m_fp);       // 写入一行（不带自动换行）
fflush(m_fp);                       // 强制把 stdio 缓冲区刷进内核/磁盘
fclose(m_fp);                       // 关闭（顺带 flush）
```

- `fopen` 的 `"a"` 模式是"追加"：每次写都定位到文件末尾，切文件时不会覆盖旧日志。
- `fputs`/`fprintf` 默认写在**用户态缓冲区**里，不一定会立刻落到磁盘。`fflush` 强制刷出——这也是宏里每次 `write_log` 后都 `flush()` 的原因（希望日志"即写即见"）。

### 5. 模板类 `block_queue<T>`：循环数组 + 条件变量

```cpp
template <class T>
class block_queue { ... };

block_queue<string> *q = new block_queue<string>(max_queue_size);
```

- **模板**：`T` 是占位类型，实例化后替换成 `string`，于是队列能装任意类型（本项目装 `string`）。
- **循环数组**：用固定大小数组 + 两个游标 `m_front`/`m_back`，游标到末尾就绕回开头：

```cpp
m_back = (m_back + 1) % m_max_size;   // 入队：尾游标后移，绕圈
m_front = (m_front + 1) % m_max_size; // 出队：头游标后移，绕圈
```

  为什么不用 `push_back` 的真队列？定长数组避免了频繁动态扩容，内存连续、实现简单。
- **条件变量 `m_cond`**：`pop` 在队列空时 `m_cond.wait(mutex)`（原子地释放锁并睡眠），`push` 时 `m_cond.broadcast()` 唤醒等待者。
- **`broadcast` vs `signal`**：`signal` 只唤醒**一个**等待线程，`broadcast` 唤醒**全部**。本项目日志队列是"单消费者"，理论上 `signal` 就够，但仓库用 `broadcast` 是**保守、稳妥**的选择（避免某些场景下"唤醒错人"导致死等，见思考题 2）。
- **带超时的 `pop(item, ms_timeout)`**：用 `cond.timewait`（即 `pthread_cond_timedwait`）最多等 `ms_timeout` 毫秒，超时返回 false——给"消费者优雅退出"留了后路。

### 6. 时间：`gettimeofday` / `localtime` / `strrchr` / `snprintf`

```cpp
struct timeval now = {0, 0};
gettimeofday(&now, NULL);          // 取当前时间：now.tv_sec 秒、now.tv_usec 微秒
time_t t = now.tv_sec;
struct tm *sys_tm = localtime(&t); // 本地时区，分解成年月日时分秒
struct tm my_tm = *sys_tm;         // 拷贝一份（localtime 返回的是内部静态缓冲，多线程下会被覆盖）

const char *p = strrchr(file_name, '/');        // 找最后一个 '/'（拆目录与文件名）
snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
```

- `%02d` 表示"至少两位、不足补 0"，所以月份 1 会输出 `01`。
- `tm_year` 是从 1900 算起的偏移，所以要 `+1900`；`tm_mon` 从 0 算起，所以 `+1`。
- `localtime` 返回指向**静态缓冲区**的指针，多线程里每次调用都会覆盖同一块内存，所以立刻 `*sys_tm` 拷出来，避免被别的线程改坏。

---

## 动手实现

本阶段新增 3 个文件（`log/block_queue.h`、`log/log.h`、`log/log.cpp`），并修改 `http_conn.*`、`server.cpp`、`makefile`。

### 步骤 1：`my_tiny_webserver/log/block_queue.h`

完整文件如下（与仓库 `log/block_queue.h` 逐行对应）：

```cpp
/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"
using namespace std;

template <class T>
class block_queue
{
public:
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0)
        {
            exit(-1);
        }

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear()
    {
        m_mutex.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_mutex.unlock();
    }

    ~block_queue()
    {
        m_mutex.lock();
        if (m_array != NULL)
            delete [] m_array;

        m_mutex.unlock();
    }

    // 判断队列是否满了
    bool full()
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty()
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 返回队首元素
    bool front(T &value)
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_front];
        m_mutex.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T &value)
    {
        m_mutex.lock();
        if (0 == m_size)
        {
            m_mutex.unlock();
            return false;
        }
        value = m_array[m_back];
        m_mutex.unlock();
        return true;
    }

    int size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    int max_size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_max_size;
        m_mutex.unlock();
        return tmp;
    }

    // 往队列添加元素（生产者）。满则唤醒等待者后返回 false。
    bool push(const T &item)
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {
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

    // 出队（消费者）。空则阻塞等待条件变量。
    bool pop(T &item)
    {
        m_mutex.lock();
        while (m_size <= 0)
        {
            if (!m_cond.wait(m_mutex.get()))
            {
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

    // 增加了超时处理：最多等 ms_timeout 毫秒
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        m_mutex.lock();
        if (m_size <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!m_cond.timewait(m_mutex.get(), t))
            {
                m_mutex.unlock();
                return false;
            }
        }

        if (m_size <= 0)
        {
            m_mutex.unlock();
            return false;
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

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
```

**逐函数讲解：**

- **构造**：`m_front`/`m_back` 初始为 `-1`（"空"的哨兵值）；`new T[max_size]` 分配定长数组。
- **`push`**：先加锁；**满**则 `broadcast()` 唤醒等待者后返回 `false`（生产者据此改走同步写，保证日志不丢）；否则 `m_back` 绕圈后移、放入元素、`m_size++`，再 `broadcast()` 唤醒可能正在空等的消费者，最后解锁。
- **`pop`（无超时）**：`while (m_size <= 0)` 用 **while 而非 if**——因为 `cond_wait` 可能被**虚假唤醒**（spurious wakeup），醒后必须再查一次条件；条件仍不满足就继续睡。条件满足后 `m_front` 绕圈后移、取出、`m_size--`。
- **`pop(item, ms_timeout)`**：把"绝对超时时刻"算成 `timespec`（秒 + 纳秒），`timewait` 超时返回 false；醒来再查一次 `m_size`，避免"超时与有数据"竞争。
- **`clear` / 析构**：都持锁操作，保证与其它线程不冲突。析构持锁 `delete[] m_array` 释放数组。

> 注意 `front/back/empty/full/size/max_size` 每个都先加锁再解锁——这些方法可能被多个线程并发调用，必须串行化读 `m_size` 等共享成员。

### 步骤 2：`my_tiny_webserver/log/log.h`

完整文件如下（与仓库 `log/log.h` 逐行对应）：

```cpp
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "block_queue.h"

using namespace std;

class Log
{
public:
    // C++11 以后，使用局部变量懒汉不用加锁
    static Log *get_instance()
    {
        static Log instance;
        return &instance;
    }

    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    // 可选择的参数有：日志文件名、是否关闭日志、缓冲区大小、最大行数、阻塞队列长度
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        // 从阻塞队列中取出一个日志 string，写入文件
        while (m_log_queue->pop(single_log))
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; // 路径名
    char log_name[128]; // log 文件名
    int m_split_lines;  // 日志最大行数
    int m_log_buf_size; // 日志缓冲区大小
    long long m_count;  // 日志行数记录
    int m_today;        // 因为按天分类，记录当前是那一天
    FILE *m_fp;         // 打开 log 的文件指针
    char *m_buf;
    block_queue<string> *m_log_queue; // 阻塞队列
    bool m_is_async;                  // 是否异步标志位
    locker m_mutex;
    int m_close_log; // 关闭日志
};

#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
```

**逐段讲解：**

- **`get_instance`**：单例入口（见"本阶段 C++ 知识点 1"）。
- **`flush_log_thread`**：静态线程函数（pthread 回调必须静态），转调单例的 `async_write_log`。
- **`async_write_log`**：**消费者主循环**——不停 `pop`，拿到一条就 `fputs` 到文件。持 `m_mutex` 是因为 `m_fp` 也可能会被主线程的 `flush()` 碰。
- **`init`**：初始化文件名、缓冲区、切分行数、队列长度。`max_queue_size >= 1` 即异步（会 `new` 队列 + 起线程），否则同步。默认参数让"同步用法"可以只传前两个参数。
- **四个 `LOG_*` 宏**：见"本阶段 C++ 知识点 2"。

### 步骤 3：`my_tiny_webserver/log/log.cpp`

完整文件如下（与仓库 `log/log.cpp` 逐行对应）：

```cpp
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "log.h"
#include <pthread.h>
using namespace std;

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (m_fp != NULL)
    {
        fclose(m_fp);
    }
}

// 异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    // 如果设置了 max_queue_size，则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        // flush_log_thread 为回调函数，这里表示创建线程异步写日志
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    m_close_log = close_log;
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    if (p == NULL)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL)
    {
        return false;
    }

    return true;
}

void Log::write_log(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    // 写入一个 log，对 m_count++，达到 m_split_lines 最大行数则切分
    m_mutex.lock();
    m_count++;

    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0) // 按天 或 按行 切分
    {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        if (m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld", dir_name, tail, log_name, m_count / m_split_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    // 写入的具体时间内容格式
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                     my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);

    int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';
    log_str = m_buf;

    m_mutex.unlock();

    if (m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);   // 异步：入队
    }
    else
    {
        m_mutex.lock();               // 同步（或队列满退回同步）：直接落盘
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    // 强制刷新写入流缓冲区
    fflush(m_fp);
    m_mutex.unlock();
}
```

**逐段讲解：**

- **`init` 的文件名拼接**：`strrchr(file_name, '/')` 找最后一个 `/`。有 `/` 就把路径拆成 `dir_name`（`"./"`）与 `log_name`（`"ServerLog"`），再拼出 `./2025_01_15_ServerLog`；没有 `/` 就直接用传入名。`m_today` 记录"今天几号"，用于按天切分。
- **`write_log` 的三段式**：
  1. **切分判断（锁内）**：`m_count++` 后，若"跨天"或"写满 `split_lines` 行"，就 `fflush`+`fclose` 旧文件、拼新文件名（跨天用日期前缀、按行用 `.N` 后缀）再 `fopen` 新文件。
  2. **拼日志行（锁内）**：先 `snprintf` 写时间戳 + 级别（`n` 字节），再 `vsnprintf` 把变参内容拼到后面（`m` 字节），补 `'\n'` 和 `'\0'`，得到一行完整日志。
  3. **落盘**：异步且队列没满就 `push`；否则（同步，或异步队列满）直接 `fputs` 落盘。`va_end` 收尾。
- **`flush`**：`fflush(m_fp)` 强制刷盘。宏里每次 `write_log` 后都调一次，保证日志"即时可见"。

> **仓库实现中的两个小瑕疵（进阶思考，不改代码、先看懂）：**
>
> 1. **切分判断的除零/溢出风险**：`m_count % m_split_lines` 里 `m_split_lines` 来自 `init` 参数且**未校验**——若传 0（或超 `INT_MAX` 溢出成负数）会**除零崩溃**；正常 500 万行时 `m_count` 用 `long long` 尚安全，但 `m_split_lines` 是 `int`，传超大值会溢出。教学上可理解为"对配置参数缺少防御性校验"。
> 2. **异步退出不优雅 + 析构泄漏**：`~Log()` 只 `fclose`，既**不 `delete[] m_buf`、不 `delete m_log_queue`**（内存泄漏），也**不等待写日志线程把队列排空**——进程退出瞬间，队列里还没落盘的日志会丢失（这正是思考题 1）。同步/小规模教学场景无感，但要知道。

### 步骤 4：集成——`server.cpp` 与 `http_conn`

#### 4.1 修改 `my_tiny_webserver/server.cpp`

在 Stage 6 的 server.cpp 基础上，做四件事：加 `#include "log/log.h"`、声明全局 `m_close_log`、`main` 开头初始化日志 + 简易解析 `-l/-c`、把 `printf` 换成 `LOG_*`。

完整文件如下（Stage 6 基础上，改动处已用注释标出）：

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>

#include "timer/lst_timer.h"
#include "http/http_conn.h"
#include "threadpool/threadpool.h"
#include "log/log.h"                     // 新增：日志

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000
#define TIMESLOT 5

// ---- 全局变量 ----
static int listenfd = -1;
static int epollfd = -1;
static int pipefd[2];
static int trig_mode = 0;
static char *doc_root = NULL;
static http_conn *users = NULL;
static client_data *users_timer = NULL;
static Utils utils;
static threadpool<http_conn> *pool = NULL;

// 新增：LOG_* 宏会引用"当前作用域里的 m_close_log"。
// 仓库里它是 WebServer 的成员变量；平铺版里用一个全局变量顶上。
// 简化点：Stage 9 收拢回 WebServer 类后，删掉这个全局、改用成员。
int m_close_log = 0;

void timer(int connfd, struct sockaddr_in client_address)
{
    // 简化点：仓库版 init 有 8 个参数，学习者版现在 5 个（Stage 8 补全 sql 相关 3 个）
    users[connfd].init(connfd, client_address, doc_root, trig_mode, m_close_log);

    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *t = new util_timer;
    t->user_data = &users_timer[connfd];
    t->cb_func = cb_func;
    time_t cur = time(NULL);
    t->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = t;
    utils.m_timer_lst.add_timer(t);
}

void adjust_timer(util_timer *t)
{
    time_t cur = time(NULL);
    t->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(t);
    LOG_INFO("%s", "adjust timer once");              // 原 printf 换成 LOG_INFO
}

void deal_timer(util_timer *t, int sockfd)
{
    t->cb_func(&users_timer[sockfd]);
    if (t)
    {
        utils.m_timer_lst.del_timer(t);
    }
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);  // 原 printf 换成 LOG_INFO
}

bool dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
    if (connfd < 0)
    {
        LOG_ERROR("%s:errno is:%d", "accept error", errno);   // 原 printf 换成 LOG_ERROR
        return false;
    }
    if (http_conn::m_user_count >= MAX_FD)
    {
        utils.show_error(connfd, "Internal server busy");
        LOG_ERROR("%s", "Internal server busy");
        return false;
    }
    timer(connfd, client_address);
    return true;
}

bool dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1 || ret == 0)
    {
        return false;
    }
    for (int i = 0; i < ret; ++i)
    {
        switch (signals[i])
        {
        case SIGALRM:
        {
            timeout = true;
            break;
        }
        case SIGTERM:
        {
            stop_server = true;
            break;
        }
        }
    }
    return true;
}

void dealwithread(int sockfd)
{
    util_timer *t = users_timer[sockfd].timer;
    if (users[sockfd].read_once())
    {
        LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        pool->append(users + sockfd);
        if (t)
        {
            adjust_timer(t);
        }
    }
    else
    {
        deal_timer(t, sockfd);
    }
}

void dealwithwrite(int sockfd)
{
    util_timer *t = users_timer[sockfd].timer;
    if (users[sockfd].write())
    {
        LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if (t)
        {
            adjust_timer(t);
        }
    }
    else
    {
        deal_timer(t, sockfd);
    }
}

int main(int argc, char *argv[])
{
    // 新增：简易命令行解析 ./server -l 1(异步)/0(同步)  -c 1(关日志)/0(开)
    // 简化点：Stage 9 用 getopt + Config 类替换这里
    int log_write = 0;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-l") == 0 && i + 1 < argc)
            log_write = atoi(argv[++i]);
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            m_close_log = atoi(argv[++i]);
    }

    // 新增：日志初始化（必须先于其它模块，因为后面都会打日志）
    if (0 == m_close_log)
    {
        if (1 == log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);  // 异步
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);    // 同步
    }

    // 1. 站点根目录
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    doc_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(doc_root, server_path);
    strcat(doc_root, root);

    // 2. 监听 socket
    listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(9006);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // 3. epoll
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);
    utils.addfd(epollfd, listenfd, false, 0);
    http_conn::m_epollfd = epollfd;

    // 4. 统一事件源
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    utils.setnonblocking(pipefd[1]);
    utils.addfd(epollfd, pipefd[0], false, 0);

    // 5. 信号
    Utils::u_pipefd = pipefd;
    Utils::u_epollfd = epollfd;
    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);
    alarm(TIMESLOT);

    // 6. 资源与线程池
    users = new http_conn[MAX_FD];
    users_timer = new client_data[MAX_FD];
    pool = new threadpool<http_conn>(8, 10000);

    // 7. 事件循环
    bool timeout = false;
    bool stop_server = false;
    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");       // 原 printf 换成 LOG_ERROR
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                bool f = dealclientdata();
                if (!f)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *t = users_timer[sockfd].timer;
                deal_timer(t, sockfd);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool f = dealwithsignal(timeout, stop_server);
                if (!f)
                    LOG_ERROR("%s", "dealwithsignal failure");
            }
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");           // 原 printf 换成 LOG_INFO
            timeout = false;
        }
    }

    // 8. 清理
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
```

#### 4.2 修改 `my_tiny_webserver/http/http_conn.h`（节选）

```cpp
// 节选：http/http_conn.h —— 新增 include、init 参数、m_close_log 成员
#include "../lock/locker.h"
#include "../log/log.h"        // 新增：拿到 LOG_* 宏

class http_conn
{
public:
    ...
    // 简化点：学习者版 init 从 4 参数变为 5 参数（新增 close_log），
    //         向仓库 8 参数版靠拢；剩余 sql 相关 3 个参数 Stage 8 补全。
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log);
    ...

private:
    ...
    int m_TRIGMode;
    int m_close_log;            // 新增：日志开关，供 LOG_* 宏使用（与仓库同名同义）
};
```

#### 4.3 修改 `my_tiny_webserver/http/http_conn.cpp`（节选）

```cpp
// 节选 1：init() —— 保存 close_log 参数（其余成员赋值保持不变）
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;   // 新增：记录日志开关

    init();                    // 调内部 init() 重置状态
}

// 节选 2：三处 printf 换成 LOG_INFO（与仓库 http_conn.cpp 的用法一一对应）

// ① parse_headers() 里，遇到无法识别的头部时（原为 printf）：
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }

// ② process_read() 里，每解析出一行时（原为 printf）：
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);          // 原 printf 换成 LOG_INFO
        // ... 后续 switch 不变
    }

// ③ add_response() 里，拼好响应后（原为 printf）：
    LOG_INFO("request:%s", m_write_buf);   // 原 printf 换成 LOG_INFO
    return true;

// 注意：close_conn() 里的 printf("close %d\n", m_sockfd) 仓库原样保留 printf，照抄即可，无需改。
```

> 集成要点：`server.cpp` 里 `m_close_log` 是全局变量、`http_conn` 里 `m_close_log` 是成员变量——**它们各自服务自己作用域里的 `LOG_*` 宏**，这正是"宏展开后再做名字查找"的体现。

### 步骤 5：更新 `my_tiny_webserver/makefile`

```makefile
CXX ?= g++
CXXFLAGS += -g -Wall

# 简化点：仓库版还编译 CGImysql 并链接 -lmysqlclient，Stage 8 再补
server: server.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread

clean:
	rm -f server
```

---

## 编译与运行

```bash
cd ~/projects/my_tiny_webserver
make          # 或：g++ -o server server.cpp timer/lst_timer.cpp http/http_conn.cpp log/log.cpp -lpthread
./server            # 默认同步日志（-l 缺省为 0）
./server -l 0       # 显式同步
./server -l 1       # 异步
./server -c 1       # 关闭日志
```

预期：启动后不再往终端刷 `printf`（改为写文件），`ls` 能看到带日期的日志文件（如 `2025_01_15_ServerLog`）。

---

## 验收清单

每行都是一条"命令/操作 + 明确预期输出"，全部打勾才算过关。

- [ ] **编译通过**：`make` 无 error，生成 `server`。
- [ ] **同步模式生成带日期日志文件**：`./server -l 0` 运行几秒后 `Ctrl+C` 停掉；`ls` 输出形如 `2025_01_15_ServerLog` 的文件（文件名格式 `yyyy_mm_dd_ServerLog`）。
- [ ] **日志行格式正确**：`cat 2025_*_ServerLog`，每一行形如 `2025-01-15 14:30:05.123456 [info]: timer tick`——即"日期 时:分:秒.微秒 [级别]: 内容"。
- [ ] **访问触发日志**：后台运行 `./server -l 0`，另开终端 `curl http://127.0.0.1:9006/`（或 `nc` 连接后发 `GET / HTTP/1.1`）；`tail -f 2025_*_ServerLog` 能看到新增的 `[info]: deal with the client(...)`、`[info]: timer tick` 等行。
- [ ] **同步 vs 异步**：分别用 `./server -l 0` 与 `./server -l 1` 运行，各自都能正常服务并写日志（两者日志内容一致，异步多一条写线程）。
- [ ] **异步模式看到写日志线程**：`./server -l 1` 后另开终端 `ps -L -p <pid>`；异步模式比同步模式**多一条线程**（写日志线程，加上 8 条线程池线程 + 1 主线程，共约 10 条；同步约 9 条）。
- [ ] **按行切分**：临时把 `server.cpp` 里 `Log::get_instance()->init("./ServerLog", m_close_log, 2000, 10, 0)` 的 `split_lines` 改成 `10`，重新 `make` 运行并用 `curl`/`nc` 触发超过 10 条日志；`ls` 出现 `..._ServerLog.1` 后缀文件（首文件 9 行，`.1` 起存第 10 行，见思考题 3）。验证后改回 `800000`。
- [ ] **关闭日志开关**：`./server -c 1` 运行并发起一次 `curl`；不生成日志文件、也无日志输出（`ls` 无新增 `*_ServerLog`）。
- [ ] **定时器日志接入**：保持 `./server` 运行，`tail -f` 日志文件确认每 5 秒出现一行 `timer tick`；另开 `nc 127.0.0.1 9006` 连上不发数据，约 15 秒后 `nc` 自动退出（Stage 6 的 `printf` 已成功升级为 `LOG_*`。注意：这条超时路径由 `tick()` 直接回调关闭，日志里**不会**出现 `close fd`——那行日志属于"读写出错/对端断开"的 `deal_timer` 路径；想让 `close fd` 出现，可在 `nc` 连上后直接 `Ctrl+C` 断开，服务器端 `EPOLLRDHUP` 分支会打印它）。

---

## 参考答案对照

| 学习者的文件 | 仓库参考答案 | 差异说明 |
|---|---|---|
| `log/block_queue.h` | `log/block_queue.h` | 基本一致（逐行对应） |
| `log/log.h` | `log/log.h` | 基本一致；学习者沿用仓库的局部静态单例与四个宏 |
| `log/log.cpp` | `log/log.cpp` | 基本一致；瑕疵（split_lines 未校验、析构不排空队列）照抄并已标注 |
| `http/http_conn.h` | `http/http_conn.h` | 学习者版 `init` 目前 5 参数，仓库版 8 参数（`close_log` 之后还有 `string user, string passwd, string sqlname`）；学习者版暂无 `cgi/mysql/m_users/sql_*`，Stage 8 补全 |
| `http/http_conn.cpp` | `http/http_conn.cpp` | `LOG_INFO` 调用点（`parse_headers`/`process_read`/`add_response`）与仓库一致；`close_conn` 保留 `printf`（仓库原样） |
| `server.cpp` | `webserver.cpp` + `main.cpp` | 学习者版是 main 里平铺逻辑 + 简易 `-l/-c` 解析；仓库版把日志初始化放在 `WebServer::log_write()`，参数用 `main.cpp` 里的 `Config`/getopt 解析（Stage 9 对齐） |

> 对照时重点看：① `init` 里 `max_queue_size` 如何决定同步/异步；② `write_log` 里"切分 → 拼行 → 落盘"三段分别在哪个锁里；③ 异步队列满时如何退回同步写。

---

## 常见问题

1. **编译报 `m_close_log` 未声明**：`LOG_*` 宏引用的 `m_close_log` 是**调用处作用域**的变量。在 `server.cpp` 的自由函数里用宏，就要在 `server.cpp` 里声明一个全局 `int m_close_log;`；在 `http_conn` 里用宏，`http_conn` 要有同名成员。两处缺一不可。
2. **`fopen` 返回 NULL / 不生成日志文件**：检查运行目录是否有写权限；`init` 的文件名路径（`./ServerLog` 会落在**当前工作目录**，不是源码目录）是否正确。
3. **异步模式退出丢日志 / 程序一停队列没写完**：仓库的 `~Log()` 不 `join` 写线程、不排空队列，退出瞬间未落盘的日志会丢。教学场景可接受；若想改善，可在退出前给队列发"结束标记"并 `pthread_join`（见思考题 1）。
4. **切分时程序崩（SIGFPE）**：把 `split_lines` 传成 0 导致 `m_count % m_split_lines` 除零。`init` 参数务必传正数。
5. **`##__VA_ARGS__` 报错 / 悬空逗号**：老编译器或非 GNU 模式可能不支持 `##__VA_ARGS__`。Ubuntu 22.04 的 g++ 11 默认支持；若换环境报错，把 `-std` 设为 `gnu++17` 即可。
6. **`localtime` 返回的时间偶尔错乱**：`localtime` 返回内部静态缓冲，多线程下会被覆盖。仓库用 `struct tm my_tm = *sys_tm;` 立即拷贝规避——照抄即可，不要直接长期持有 `sys_tm` 指针。
7. **日志里没换行 / 两行粘在一起**：`fputs` 不自动加 `\n`，本实现靠 `m_buf[n+m] = '\n'` 手动补换行。若你的行粘连，检查这一步是否漏写。

---

## 思考题

1. 异步模式下，进程退出时阻塞队列里还没写完的日志会怎样？如果要"优雅排空"，你会在哪里、用什么机制通知写线程退出并等待它结束？
2. `block_queue` 的 `push` 为什么用 `broadcast()` 而不是 `signal()`？本项目只有一个消费者，用 `signal()` 会不会有问题？什么场景下必须用 `broadcast()`？
3. 按行切分时，第 10 行日志落在 `_ServerLog.1` 还是 `_ServerLog`？结合 `write_log` 里"先 `m_count++` 再判断、切分在写之前"的顺序，解释这个"首文件少一行"的现象。
4. 局部静态变量单例为什么在 C++11 之后天然线程安全？如果在 C++11 之前（C++98/03），要怎样保证线程安全？
5. `write_log` 里 `m_mutex.lock()` 与 `m_mutex.unlock()` 出现了多次，为什么"切分判断"和"拼日志行"要分两段加锁？能不能合并成一把锁从头锁到尾？合并后有什么副作用？
6. 仓库的 `LOG_*` 宏用 `if(0 == m_close_log){...}` 包裹而不是 `do{...}while(0)`，在 `if (cond) LOG_INFO("x"); else foo();` 这种写法下会发生什么？你能把宏改写得更安全吗？

---

## 下一步

下一阶段 [Stage 8：数据库与注册登录](stage-08-mysql.md) 会引入 MySQL 连接池，用 **RAII** 管理连接，并给 `http_conn` 补全 `sql_user/sql_passwd/sql_name` 与 `init` 的剩余参数，实现浏览器端的注册/登录全流程。

返回主索引：[README.md](README.md)
