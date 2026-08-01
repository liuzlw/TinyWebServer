# Stage 7 日志系统:同步 / 异步

> 服务器一直在跑,出了问题怎么排查?答案是把运行状态写进日志文件。本阶段实现项目自己的日志系统,顺带用上 C5 学的生产者消费者。

## 1. 本阶段目标

- [ ] 理解阻塞队列 `block_queue<T>`(生产者消费者)
- [ ] 理解日志的**同步模式**(直接写)与**异步模式**(先入队,后台线程写)
- [ ] 理解单例模式(`Log::get_instance()`)
- [ ] 服务器关键事件写入日志文件,`tail -f` 实时观察

**最终效果:** 服务器运行后,当前目录生成 `2026_08_01_ServerLog` 日志文件,每次 accept/请求/关闭都记录带时间戳的日志。

## 2. 前置知识

- C5:线程、互斥锁、条件变量;S6:定时器服务器
- 新增:单例模式、阻塞队列、可变参数函数(`vsnprintf`)

## 3. 问题:printf 调试的局限

到目前为止我们一直用 `printf` 打印。它的局限:
1. 输出到终端,**程序退出就没了**
2. 线上环境看不到终端
3. 没有统一的时间戳、级别(debug/info/error)

**日志系统的目标:** 统一的格式(时间 + 级别 + 内容),写进**文件**,且支持"高并发下不能因为写日志拖慢服务器"的异步模式。

## 4. 阻塞队列 block_queue:异步的"快递柜"

异步日志的思路:写日志的调用方**不直接写文件**,而是把日志字符串丢进一个队列,由**专门的后台线程**负责取出来写文件。这样写日志的调用方立刻返回,不阻塞。

`block_queue<T>` 就是这个队列——**循环数组 + 互斥锁 + 条件变量**,和 C5 的生产者消费者一模一样。

在 `my_tiny_webserver/log/` 下新建 `block_queue.h`(**与原项目一致**,模板类):

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

    int size()
    {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_size;
        m_mutex.unlock();
        return tmp;
    }

    //往队列添加元素(生产者)
    bool push(const T &item)
    {
        m_mutex.lock();
        if (m_size >= m_max_size)
        {
            m_cond.broadcast();      // 满时也唤醒(虽然失败,但避免消费者饿死)
            m_mutex.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;   // 循环数组:到末尾绕回开头
        m_array[m_back] = item;

        m_size++;

        m_cond.broadcast();          // 唤醒等待的消费者
        m_mutex.unlock();
        return true;
    }
    //pop时,如果当前队列没有元素,将会等待条件变量(消费者)
    bool pop(T &item)
    {
        m_mutex.lock();
        while (m_size <= 0)          // 用 while 不用 if:防止"假唤醒"
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

private:
    locker m_mutex;      // 保护队列
    cond m_cond;         // 条件变量:没数据就等

    T *m_array;          // 循环数组
    int m_size;          // 当前元素个数
    int m_max_size;      // 容量
    int m_front;         // 队首下标
    int m_back;          // 队尾下标
};

#endif
```

**讲解:**

| 部分 | 说明 |
|---|---|
| `m_back = (m_back + 1) % m_max_size` | **循环数组**:到末尾绕回 0,不用搬数据 |
| `m_mutex` | 所有操作先加锁——队列是共享的 |
| `m_cond` | `pop` 时队列空就 `wait` 睡觉;`push` 后 `broadcast` 唤醒 |
| `while (m_size <= 0)` | 用 while 不用 if——防止**假唤醒**(被唤醒但又被别的线程抢了) |

> 这里的 `cond::wait(m_mutex.get())` 需要 `locker.h` 里的 `get()` 返回原生 `pthread_mutex_t *`。S3 的简化版 locker.h 没带 `timewait`,所以本阶段**把 locker.h 换成原版**(多了 `timewait` 方法,其他一致)。

## 5. log.h + log.cpp:单例 + 同步/异步切换

在 `my_tiny_webserver/log/` 下新建 `log.h`(**与原项目一致**):

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
    //C++11以后,使用局部静态变量的懒汉单例,不用加锁
    static Log *get_instance()
    {
        static Log instance;     // 局部静态变量:第一次用到才创建,只创建一次
        return &instance;
    }

    // 异步日志的后台线程入口:从队列取日志,写文件
    static void *flush_log_thread(void *args)
    {
        Log::get_instance()->async_write_log();
    }

    // 参数:文件名、是否关闭日志、缓冲区大小、最大行数、日志队列长度(>=1 表示异步)
    bool init(const char *file_name, int close_log, int log_buf_size = 8192, int split_lines = 5000000, int max_queue_size = 0);

    void write_log(int level, const char *format, ...);

    void flush(void);

private:
    Log();
    virtual ~Log();
    void *async_write_log()
    {
        string single_log;
        while (m_log_queue->pop(single_log))      // 后台线程:一直取,取到就写
        {
            m_mutex.lock();
            fputs(single_log.c_str(), m_fp);
            m_mutex.unlock();
        }
    }

private:
    char dir_name[128]; //路径名
    char log_name[128]; //log文件名
    int m_split_lines;  //日志最大行数
    int m_log_buf_size; //日志缓冲区大小
    long long m_count;  //日志行数记录
    int m_today;        //按天分类,记录当前时间是哪一天
    FILE *m_fp;         //打开log的文件指针
    char *m_buf;
    block_queue<string> *m_log_queue; //阻塞队列(异步用)
    bool m_is_async;                  //是否异步标志位
    locker m_mutex;
    int m_close_log; //关闭日志
};

// 四个级别宏:0=关闭日志时跳过;否则 write_log + flush
#define LOG_DEBUG(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(0, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_INFO(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(1, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_WARN(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(2, format, ##__VA_ARGS__); Log::get_instance()->flush();}
#define LOG_ERROR(format, ...) if(0 == m_close_log) {Log::get_instance()->write_log(3, format, ##__VA_ARGS__); Log::get_instance()->flush();}

#endif
```

**单例模式:** `Log::get_instance()` 返回**同一个** Log 对象(整个程序一份)。`static Log instance;` 是 C++11 的"局部静态变量",第一次调用时创建,以后直接返回——线程安全的懒加载。整个项目都用 `Log::get_instance()` 拿到这同一个日志对象,保证写文件不冲突。

**LOG 宏的秘密:** 注意 `LOG_INFO(...)` 展开后是 `if(0 == m_close_log) {...}`——里面引用了 **`m_close_log`**。在类成员函数里(如 http_conn),它解析为类的成员变量;在 main 里,需要一个全局变量 `int m_close_log = 0;` 供它解析。这就是"用宏的代价":宏不知道作用域。

在 `my_tiny_webserver/log/` 下新建 `log.cpp`(**与原项目一致**):

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

//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int close_log, int log_buf_size, int split_lines, int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数,创建线程异步写日志
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

    // 文件名里没斜杠 → 直接拼"日期_文件名";有 → 拆出目录
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

    m_fp = fopen(log_full_name, "a");    // "a" 追加模式
    if (m_fp == NULL)
        return false;

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
    case 0: strcpy(s, "[debug]:"); break;
    case 1: strcpy(s, "[info]:"); break;
    case 2: strcpy(s, "[warn]:"); break;
    case 3: strcpy(s, "[erro]:"); break;
    default: strcpy(s, "[info]:"); break;
    }
    //写入一个log，对m_count++, m_split_lines最大行数
    m_mutex.lock();
    m_count++;

    // 跨天 或 行数超上限 → 换一个新文件
    if (m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
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

    //写入的具体时间内容格式: 日期 时间 微秒 级别
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
        m_log_queue->push(log_str);        // 异步:丢进队列,后台线程写
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);      // 同步:直接写文件
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    fflush(m_fp);                          // 把缓冲里的内容强制写到磁盘
    m_mutex.unlock();
}
```

**同步 vs 异步的开关,就在 `write_log` 末尾:**

```text
m_is_async?
 ├─ 是 → m_log_queue->push(log_str)     调用方立即返回,后台线程慢慢写
 └─ 否 → fputs(log_str) 直接写文件      调用方等写完才返回
```

`m_is_async` 在 `init` 里根据 `max_queue_size >= 1` 决定。**异步 = 快但不保证立刻落盘;同步 = 慢但写日志时就写好了。**

## 6. main.cpp:接入日志

**替换 `my_tiny_webserver/main.cpp`**(在 S6 基础上加日志初始化 + LOG_INFO 调用):

```cpp
// main.cpp —— epoll + http_conn + 定时器 + 日志(Stage 7)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http/http_conn.h"
#include "timer/lst_timer.h"
#include "log/log.h"

// LOG_INFO 等宏内部引用了 m_close_log。
// 在类里它对应类的成员(http_conn::m_close_log);在 main 里定义一个全局变量让它可用
int m_close_log = 0;

const int PORT = 9006;
const int MAX_FD = 65535;
const int MAX_EVENT_NUMBER = 10000;
const int TRIGMode = 1;        // 0 = LT, 1 = ET
const int TIMESLOT = 5;        // 每 5 秒扫描一次定时器

http_conn *users = new http_conn[MAX_FD];
client_data *users_timer = new client_data[MAX_FD];
sort_timer_lst timer_lst;

int pipefd[2];

extern void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev, int TRIGMode);

// 初始化日志:最后一个参数 1024>=1 → 异步模式
// 想用同步,把 1024 改成 0
void init_log()
{
    Log::get_instance()->init("./ServerLog", 0, 8192, 5000000, 1024);
}

void init_timer(int connfd, const sockaddr_in &client_address)
{
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;

    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    timer_lst.add_timer(timer);
}

void adjust_timer(int connfd)
{
    util_timer *timer = users_timer[connfd].timer;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    timer_lst.adjust_timer(timer);
}

int main()
{
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }

    int epollfd = epoll_create(5);
    http_conn::m_epollfd = epollfd;
    addfd(epollfd, listenfd, false, TRIGMode);

    Utils utils;
    utils.init(TIMESLOT);
    Utils::u_epollfd = epollfd;
    Utils::u_pipefd = pipefd;
    socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    utils.setnonblocking(pipefd[0]);
    utils.setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false, 0);
    utils.addsig(SIGALRM, Utils::sig_handler, false);
    alarm(TIMESLOT);

    init_log();
    LOG_INFO("服务器启动, 端口 %d", PORT);

    char root[] = "root";
    printf("服务器已启动, 监听端口 %d\n", PORT);

    epoll_event events[MAX_EVENT_NUMBER];
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) { perror("epoll_wait"); break; }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == pipefd[0])
            {
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret <= 0) continue;
                for (int j = 0; j < ret; j++)
                {
                    if (signals[j] == SIGALRM)
                    {
                        timer_lst.tick();
                        alarm(TIMESLOT);
                    }
                }
                continue;
            }

            if (sockfd == listenfd)
            {
                while (true)
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0) break;
                    if (connfd >= MAX_FD) { close(connfd); continue; }
                    users[connfd].init(connfd, client_address, root, TRIGMode);
                    init_timer(connfd, client_address);
                    LOG_INFO("accept 连接 %d, 当前连接数 %d", connfd, http_conn::m_user_count);
                    printf("accept, 当前连接数: %d\n", http_conn::m_user_count);
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                timer_lst.del_timer(users_timer[sockfd].timer);
                LOG_INFO("连接 %d 关闭", sockfd);
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                if (users[sockfd].read_once())
                {
                    adjust_timer(sockfd);
                    users[sockfd].process();
                    LOG_INFO("处理连接 %d 的请求", sockfd);
                }
                else
                {
                    timer_lst.del_timer(users_timer[sockfd].timer);
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                adjust_timer(sockfd);
                if (!users[sockfd].write())
                {
                    timer_lst.del_timer(users_timer[sockfd].timer);
                    users[sockfd].close_conn();
                }
            }
        }
    }
    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete[] users_timer;
    return 0;
}
```

**注意**:S7 的 `lock/locker.h` 要换成**原版**(多了 `cond::timewait`,block_queue 需要)。命令:

```bash
cp ../lock/locker.h lock/locker.h
```

## 7. 编译与运行

更新 `CMakeLists.txt`(加入 `log/log.cpp`):

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp http/http_conn.cpp timer/lst_timer.cpp log/log.cpp)
target_link_libraries(server pthread)
```

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

**另开一个终端,实时看日志:**

```bash
tail -f 2026_08_01_ServerLog
```

再发几个请求(`curl http://127.0.0.1:9006/welcome.html`),`tail -f` 会实时滚动出新日志。

**预期日志内容(节选):**

```text
2026-08-01 21:31:28.060582 [info]: 服务器启动, 端口 9006
2026-08-01 21:31:28.669601 [info]: accept 连接 8, 当前连接数 1
2026-08-01 21:31:28.669720 [info]: 处理连接 8 的请求
```

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 启动服务器 | 当前目录生成 `2026_08_01_ServerLog`(日期为当天) | ☐ |
| 2 | `curl http://127.0.0.1:9006/welcome.html` | 200,日志新增 accept + 处理请求两行 | ☐ |
| 3 | `tail -f 2026_08_01_ServerLog` 另开终端 | 发请求时日志实时滚动 | ☐ |
| 4 | 把 `init_log()` 里最后参数 1024 改成 0(同步模式)重启 | 功能一致,日志照常生成 | ☐ |
| 5 | 用 `nc` 连上不发数据,等超时 | 日志出现 `定时器关闭空闲连接` | ☐ |
| 6 | 日志格式 | 每行都有 `日期 时间.微秒 [级别]:` 前缀 | ☐ |

## 9. 调试技巧

### 看异步线程在干嘛

```bash
gdb ./build/server
```

```text
(gdb) break log.cpp:144        ← 断在 write_log 的 push(异步路径)
(gdb) run
(gdb) info threads             ← 会看到:主线程 + 1 个 flush_log_thread 后台线程
```

`info threads` 里那个 `flush_log_thread` 就是异步日志的后台消费者,它一直 `pop` 队列里的日志字符串写文件。

### 用 grep 过滤日志

```bash
grep "error" 2026_08_01_ServerLog      # 只看错误
grep "accept" 2026_08_01_ServerLog     # 只看连接
```

## 10. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 日志文件没生成 | 工作目录不对(没在 `my_tiny_webserver/` 下运行) | `cd` 到项目目录再启动 |
| 日志文件生成了但**内容为空** | 异步模式 + 直接调 `write_log` 没调 `flush()` | 用 `LOG_INFO` 等宏(宏里带 flush),或手动 `flush()` |
| `LOG_INFO` 报 `m_close_log was not declared` | main 里没定义全局 `m_close_log` | `int m_close_log = 0;` 加在 main 顶部 |
| 编译报 `timewait` 未定义 | locker.h 还是 S3 的简化版 | 换成原版 `lock/locker.h` |
| 换天后日志还写在旧文件 | `m_today` 判定跨天 | 代码已处理(跨天换新文件),重启服务器验证 |
| 日志乱码 | 中文字符编码 | 日志内容尽量用英文,或终端 UTF-8 |

## 11. 与原项目对照

| 本阶段 | 原项目 |
|---|---|
| `log/block_queue.h` | **逐字一致** |
| `log/log.h` + `log.cpp` | **逐字一致** |
| `lock/locker.h`(换回原版) | **逐字一致** |
| main 里的 `init_log` + `LOG_INFO` | 对应 `webserver.cpp` 的 `log_write()` 和散布各处的 `LOG_*` 调用 |
| 用 `m_close_log = 0` 全局变量 | 原项目在 `WebServer`/`http_conn` 类里有自己的 `m_close_log` 成员 |

> **diff 对照**:
> ```bash
> diff my_tiny_webserver/log/log.cpp log/log.cpp
> diff my_tiny_webserver/log/block_queue.h log/block_queue.h
> ```

## 12. 下一步

进入 **[Stage 8 MySQL 连接池与注册登录](stage-08-mysql.md)**——项目"完整功能"的最后一块:数据库连接池 + 用户注册/登录。Stage 0 建的 `qgydb` 库和 `user` 表终于要派上用场了。
