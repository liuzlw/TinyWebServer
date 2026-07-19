# Stage 9：整合、命令行参数与压力测试

> 🎯 **本阶段目标**：收官阶段 —— 把 Stage 3-8 的所有模块用 `Config` + `WebServer`
> 两个类装配成最终形态，支持全部命令行参数（LT/ET 组合、Reactor/Proactor、
> 优雅关闭、日志开关），并用 **webbench 压测**验证上万并发。完成本项目 100% 复现。

## 📚 理论铺垫

### 9.1 为什么需要 WebServer 这一层？

现在 main.cpp 已经很长：socket、epoll、定时器、线程池、连接池、信号处理全在里面。
工程的下一步就是**封装**：把所有这些收进一个 `WebServer` 类，
main.cpp 只负责「解析参数 + 装配 + 启动」：

```cpp
int main(int argc, char* argv[]) {
    Config config;
    config.parse_arg(argc, argv);     // 解析命令行

    WebServer server;
    server.init(...);                 // 收参数
    server.log_write();               // 挂日志
    server.sql_pool();                // 挂连接池
    server.thread_pool();             // 挂线程池
    server.trig_mode();               // 设触发模式
    server.eventListen();             // 监听
    server.eventLoop();               // 跑事件循环
}
```

这就是原始项目 main.cpp 的完整内容 —— 每一行对应你已经实现的一个模块。
**main 函数即文档**：读一眼就知道服务器的装配顺序。

### 9.2 四种触发模式组合（-m 参数）

| TRIGMode | listenfd | connfd |
|----------|----------|--------|
| 0 | LT | LT |
| 1 | LT | ET |
| 2 | ET | LT |
| 3 | ET | ET |

`trig_mode()` 就是一个 switch，设置 `m_LISTENTrigmode` 和 `m_CONNTrigmode` 两个标志，
之后 `addfd`/`dealclientdata`/`read_once` 根据标志走不同分支：

- listenfd 为 ET 时：`dealclientdata` 必须**循环 accept 到 EAGAIN**（同 ET 读）
- connfd 为 LT 时：`read_once` read 一次即可；ET 时循环 read 到 EAGAIN

### 9.3 Reactor 与 Proactor 的分工（-a 参数）

到 Stage 5，你其实已经同时摸到了两种模式。区别只在 EPOLLIN/EPOLLOUT 事件里
「谁做 I/O」：

```
Proactor (m_actormodel=0):  主线程 read_once() 读完数据 → 线程池只 process()
                            主线程 write() 发数据        → 线程池只 process_write()

Reactor  (m_actormodel=1):  主线程只把任务扔进池子
                            工作线程自己 read_once() / write()
```

对应 `webserver.cpp` 的 `dealwithread`/`dealwithwrite`：

```cpp
void WebServer::dealwithread(int sockfd) {
    util_timer* timer = users_timer[sockfd].timer;
    if (1 == m_actormodel) {                    // Reactor
        if (timer) adjust_timer(timer);
        m_pool->append(users + sockfd, 0);      // state=0 表示读任务
    } else {                                    // Proactor
        if (users[sockfd].read_once()) {        // 主线程先读
            LOG_INFO("deal with the client(%s)", inet_ntoa(...));
            m_pool->append_p(users + sockfd);   // 只处理业务
            if (timer) adjust_timer(timer);
        } else {
            deal_timer(timer, sockfd);
        }
    }
}
```

线程池相应提供两个 append：`append(T*, int state)`（Reactor，带读/写标记）和
`append_p(T*)`（Proactor）。`run()` 里按 state 分发到 read_once/process/write。

### 9.4 优雅关闭（-o 参数，OPT_LINGER）

`close()` 一个还有数据没发完的 socket 时，默认行为是后台继续发。
`SO_LINGER` 选项控制这个行为：

```cpp
struct linger tmp = {1, 1};   // 开启 linger，超时 1 秒
setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
```

本项目中用于控制「关闭连接时是否等残留数据发完/对方确认」，压测大文件时有影响。

### 9.5 getopt：命令行参数解析

C 标准做法：

```cpp
int opt;
while ((opt = getopt(argc, argv, "p:l:m:o:s:t:c:a:")) != -1) {
    switch (opt) {
    case 'p': PORT = atoi(optarg); break;
    case 'm': TRIGMode = atoi(optarg); break;
    ...
    }
}
```

字符串 `"p:l:m:..."` 中字母后的冒号表示「该选项带参数」，参数值在 `optarg` 里。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| `getopt`/`optarg` | Config::parse_arg |
| 类的组合（composition） | WebServer 持有各模块对象/指针 |
| `switch` 状态分发 | trig_mode、dealwith* |
| 工程组织：8 个文件协同编译 | CMake 多文件管理 |

## 🔨 动手实现

### 9.1 `config.h` + `config.cpp`

对照原始项目 `config.h/.cpp` 写 —— 结构简单：字段 + `parse_arg`。
默认值：PORT=9006, LOGWrite=0（同步）, TRIGMode=0（LT+LT）, OPT_LINGER=0,
sql_num=8, thread_num=8, close_log=0, actor_model=0（Proactor）。

### 9.2 `webserver.h` + `webserver.cpp`

把你 main.cpp 里的逻辑搬进 WebServer 类。结构对照（每个函数对应你已经写过的代码块）：

| WebServer 成员函数 | 内容来自 |
|--------------------|----------|
| `init()` | 保存参数 + getcwd 拼 root 路径 |
| `log_write()` | Stage 7 的 Log::init |
| `sql_pool()` | Stage 8 的连接池 init + initmysql_result |
| `thread_pool()` | Stage 3 的 new threadpool |
| `trig_mode()` | 9.2 的 switch |
| `eventListen()` | Stage 4 的 socket/bind/listen/epoll + Stage 6 的 socketpair/addsig/alarm |
| `eventLoop()` | Stage 4 的 epoll_wait 循环 + Stage 6 的信号分支 |
| `dealclientdata()` | accept 新连接（含 ET 循环 accept）+ Stage 6 挂定时器 |
| `dealwithread/write()` | 9.3 的 Reactor/Proactor 分发 |
| `timer/adjust_timer/deal_timer()` | Stage 6 的定时器管理 |

建议写法：**把 Stage 4-6 的 main.cpp 逐段剪切进对应函数**，
改改变量名（变成成员变量 m_xxx），30 分钟就能完成。
然后 `diff` 你的版本和原版 `webserver.cpp`，查缺补漏。

### 9.3 线程池补全 Reactor 支持

把 Stage 3 的简化版 threadpool 升级为原版完整版，关键差异：

```cpp
// append 增加 state 参数：0=读任务 1=写任务
bool append(T* request, int state) {
    ...
    request->m_state = state;    // http_conn 里要加 m_state 成员
    ...
}
bool append_p(T* request);       // Proactor 专用（不区分读写）

void run() {
    ...
    if (1 == m_actormodel) {          // Reactor
        if (0 == request->m_state) {
            if (request->read_once()) {
                request->improv = 1;
                request->process();
            } else { ... }
        } else {
            if (request->write()) { request->improv = 1; }
            else { ... }
        }
    } else {                          // Proactor
        connectionRAII mysqlcon(&request->mysql, request->connPool);
        request->process();
    }
}
```

### 9.4 最终 main.cpp

就是 9.1 开头那段代码，逐行实现。数据库信息：

```cpp
string user = "root";
string passwd = "root";
string databasename = "yourdb";
```

### 9.5 最终 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_tiny_webserver CXX)

set(CMAKE_CXX_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIB mysqlclient)

add_executable(server
    main.cpp
    config.cpp
    webserver.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

target_include_directories(server PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(server pthread ${MYSQL_LIB})
```

## ✅ 验证

### 验证 1：功能完整性回归

```bash
cd build && cmake .. && make
./server
```

- [ ] 浏览器访问 `http://127.0.0.1:9006/` 正常
- [ ] 注册新用户、登录、错误密码报错 —— 全流程通
- [ ] 点击 welcome 页的图片/视频链接，大文件完整加载
- [ ] 日志文件正常滚动

### 验证 2：命令行参数全组合抽查

```bash
./server -p 9007           # 换端口：浏览器访问 9007 成功
./server -m 3 -a 1         # ET+ET + Reactor：功能全部正常
./server -m 2 -a 0 -c 1    # ET+LT + Proactor + 关日志：正常且无日志输出
./server -l 1              # 异步日志：日志正常
```

### 验证 3：webbench 压测（终极大考）

仓库自带 webbench：

```bash
cd ../test_pressure/webbench-1.5
make                          # 编译出 webbench 可执行文件
# 若报 ctags 相关错误：sudo apt install ctags，或只编译 webbench.o 手动链接
```

压测命令（10000 并发、跑 5 秒、关日志压性能上限）：

```bash
./server -c 1 &                          # 后台启动，关日志
./webbench -c 10000 -t 5 http://127.0.0.1:9006/index.html
```

期望输出类似：

```
Webbench - Simple Web Benchmark 1.5
10000 clients, running 5 sec.
Speed=xxxxx pages/min, xxxxx bytes/sec.
Requests: xxxxx susceed, 0 failed.
```

**验收标准：failed = 0，且 susceed 数量在数万级**（虚拟机性能不同数值差异大，
原版在物理机上达到 9 万+ QPS，WSL2 里达到几千~几万 QPS 都正常）。

对比实验（每组跑三次取稳定值，体会各模式差异）：

```bash
./server -c 1 -a 0 -m 0    # Proactor LT+LT
./server -c 1 -a 0 -m 3    # Proactor ET+ET
./server -c 1 -a 1 -m 3    # Reactor  ET+ET
./server -c 0 -a 0 -m 3    # 开同步日志 —— 感受日志对性能的巨大影响
```

### 验证 4：与原版 diff 复盘

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer
diff -r my_tiny_webserver/http http
diff -r my_tiny_webserver/timer timer
diff my_tiny_webserver/webserver.cpp webserver.cpp
# 逐文件对比，理解每一处差异（你的简化 vs 原版的完整实现）
```

## 🐛 常见问题

**Q1: webbench 大量 failed？**
① 日志没关（同步写日志扛不住压测）② fd 上限：WSL 默认 1024？
`ulimit -n 65536` 调大 ③ 端口耗尽 TIME_WAIT 过多 —— 压测属正常现象，只要
failed 率为 0 即可。

**Q2: ET 模式下压测偶发请求超时？**
检查 listenfd 的 ET 分支是否循环 accept 到 EAGAIN；connfd 的 read_once 是否
循环到 EAGAIN。漏一个都会在高并发下暴露。

**Q3: 压测时服务器崩溃？**
gdb 挂上去重现：`gdb -p $(pgrep server)`，崩溃后 `bt` + `info threads`。
高并发下暴露的多是边界问题：fd 达到 MAX_FD、定时器重复删除、队列满等。

## 🤔 思考与练习（毕业设计级）

1. **画一张完整的架构图**：主线程、线程池、epoll、定时器、日志线程、连接池
   之间的关系。不看代码凭记忆画，画完对照仓库 README 的框架图。
2. 用 `strace -c -f ./server` 统计压测期间各系统调用的耗时占比，找出热点。
3. 自己回答 LEARNING_GUIDE.md 末尾的全部面试题，写在笔记里。
4. **进阶挑战**（选做）：
   - 把升序链表定时器换成时间轮或最小堆，压测对比
   - 用 `std::mutex`/`std::condition_variable` 重写 locker.h（C++11 风格）
   - 参考 markparticle/WebServer（README 提到的 C++11/14 重构版）学习现代写法
   - 加 HTTPS 支持（OpenSSL）
5. 把你的复现过程和 diff 复盘写成一篇博客 —— 能讲出来才是真正掌握。

## 🎓 毕业清单

全部打勾，恭喜毕业：

- [ ] 从零写出了全部 8 个模块，与原版 diff 差异都能解释
- [ ] 四种 LT/ET 组合 + 两种 actor 模式都验证过功能
- [ ] webbench 上万并发 0 failed
- [ ] 能用 gdb 调试多线程程序、能分析 core dump
- [ ] 能用 CMake/Makefile 独立搭建 C++ 工程
- [ ] 能脱稿讲清：状态机、epoll LT/ET、Reactor/Proactor、线程池、
      连接池、RAII、生产者-消费者、信号统一事件源

---

⬅️ 返回[教程总览](README.md) | 附录：[A. CMake 与 Make](appendix-cmake-make.md) ·
[B. GDB 调试](appendix-gdb.md) · [C. C++ 知识点索引](appendix-cpp-map.md)
