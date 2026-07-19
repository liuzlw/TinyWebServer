# 附录 C：C++ 知识点索引

> 本项目用到的所有 C++ 语法、STL 数据结构、设计模式，按「在哪用、学什么」整理。
> 遇到不懂的语法点，来这里查它在项目中的位置和推荐学习材料。

## 1. C++ 基础语法

| 知识点 | 项目中的位置 | 要点 |
|--------|--------------|------|
| 头文件保护 | 所有 .h 文件的 `#ifndef/#define/#endif` | 防止重复包含。现代替代：`#pragma once` |
| 类的声明与实现分离 | `http_conn.h/.cpp`、`config.h/.cpp` | .h 放声明，.cpp 放实现；模板除外（见下） |
| 构造/析构函数 | `locker()`/`~locker()`、`connectionRAII` | RAII 的载体，C++ 最重要的惯用法 |
| 初始化列表 | `threadpool(...) : m_thread_number(n), ...` | 比在函数体里赋值更高效，const 成员必须用它 |
| const 修饰 | `const sockaddr_in& addr` | 引用传参避免拷贝，const 保证不改 |
| 引用 `&` | `void init(int sockfd, const sockaddr_in& addr)` | 起别名，无空指针风险 |
| new/delete 与数组 | `new pthread_t[n]` / `delete[]` | 配对使用；现代 C++ 应优先用容器/智能指针 |
| static 成员 | `http_conn::m_epollfd`、`m_user_count` | 所有对象共享一份；定义要放在 .cpp 里 |
| static 成员函数 | `threadpool::worker`、`Log::flush_log_thread` | 无 this 指针 —— 正好匹配 pthread 的入口要求 |
| 枚举 enum | `METHOD`、`CHECK_STATE`、`HTTP_CODE`、`LINE_STATUS` | 状态机的状态定义 |
| switch/case | `process_read`、`trig_mode` | 状态分发的标准写法 |
| 可变参数函数 | `add_response(const char* fmt, ...)` + `va_list` | printf 家族的原理 |
| 变参宏 | `LOG_INFO(format, ...)` + `##__VA_ARGS__` | `##` 处理零参数时的逗号 |
| typedef/using | 项目较少，了解即可 | C++11 推荐 `using` |
| friend | 原版项目无，不用管 | — |

## 2. 类模板与泛型

| 知识点 | 项目中的位置 | 要点 |
|--------|--------------|------|
| 类模板 `template<typename T>` | `threadpool<T>`、`block_queue<T>` | 池子/队列不关心元素类型 |
| **模板实现必须放在头文件** | 两个模板类都只有 .h 没有 .cpp | 编译器实例化时需要看到完整定义 —— 新手最常踩的链接错误 |
| 模板实例化 | `threadpool<http_conn>`、`block_queue<std::string>` | 编译期为每个用到的类型生成一份代码 |

## 3. STL 数据结构（本项目全部出场阵容）

| 容器 | 项目中的位置 | 为什么选它 |
|------|--------------|-----------|
| `std::list<T>` | 线程池任务队列、连接池 connList、定时器相关 | 头尾 O(1) 插删；任意位置 O(1) 删除（有迭代器时） |
| `std::map<K,V>` | `http_conn::m_users` 用户表 | 按 key 有序存储，查找 O(log n) |
| `std::string` | 日志内容、URL 处理 | 自动内存管理的字符串 |
| `std::vector` | 本项目没用（可对比 epoll_event 裸数组） | 了解它与裸数组的取舍 |
| 裸数组 | `epoll_event events[10000]`、`char m_read_buf[2048]` | 定长、零开销、和 C API 对接 |
| 手写双向链表 | `sort_timer_lst` | 数据结构基本功：next/prev 指针操作 |

**自学清单**（按优先级）：

1. `std::string`：find/substr/append/c_str —— Stage 2 就在用
2. `std::map`：insert/operator[]/find/end —— Stage 8
3. `std::list`：push_back/pop_front/push_front/clear —— Stage 3、8
4. 迭代器基本用法：`for (auto it = m.begin(); it != m.end(); ++it)`
5. 范围 for：`for (auto con : connList)` —— C++11，连接池销毁处用到

## 4. 设计模式与惯用法（面试高频）

| 模式 | 项目中的位置 | 一句话 |
|------|--------------|--------|
| **RAII** | locker、connectionRAII、文件描述符管理 | 资源生命周期 = 对象生命周期 |
| **单例** | `Log::get_instance()`、`connection_pool::GetInstance()` | 局部静态变量写法，C++11 线程安全 |
| **生产者-消费者** | 线程池（信号量版）、异步日志（条件变量版） | 两种同步原语的对比实战 |
| **半同步/半反应堆** | 主线程接事件 + 线程池处理 | 并发架构的经典分层 |
| **状态机** | HTTP 主从状态机 | 解析协议的标准方法 |
| **回调函数** | 定时器 `cb_func` 函数指针 | C 风格的"多态" |
| **Reactor/Proactor** | 事件分发层 | I/O 复用的两种编程模型 |

## 5. 系统编程（Linux API，不算 C++ 但必须会）

| 主题 | 涉及 API | 首次出现的 Stage |
|------|----------|------------------|
| socket 编程 | socket/bind/listen/accept/connect | Stage 1 |
| 字节序 | htons/htonl/ntohs/inet_ntoa | Stage 1 |
| 文件 I/O | open/read/write/close/stat | Stage 2 |
| 多线程 | pthread_create/detach/join | Stage 3 |
| 线程同步 | pthread_mutex/pthread_cond/sem_* | Stage 3 |
| I/O 多路复用 | epoll_create/epoll_ctl/epoll_wait | Stage 4 |
| 非阻塞 I/O | fcntl/O_NONBLOCK/EAGAIN | Stage 4 |
| 内存映射 | mmap/munmap | Stage 5 |
| 分散聚集 I/O | writev/iovec | Stage 5 |
| 信号 | sigaction/alarm/SIGALRM/SIGPIPE | Stage 6 |
| Unix 域 socket | socketpair（信号统一事件源） | Stage 6 |
| 命令行解析 | getopt/optarg | Stage 9 |
| MySQL C API | mysql_init/real_connect/query/store_result | Stage 8 |

## 6. 推荐学习材料

按「用到再学，边学边查」的原则：

| 材料 | 用法 |
|------|------|
| [cppreference.com](https://zh.cppreference.com) | 查任何语法/STL 的第一选择（有中文） |
| 《C++ Primer》第 5 版 | 语法系统学习，当字典翻 |
| 《Linux 高性能服务器编程》（游双） | **本项目的直接蓝本**，第 8/9/10/15 章对应 epoll/线程池/状态机 |
| 《UNIX 网络编程》卷 1 | socket/epoll 的权威参考 |
| 仓库根目录 README「庖丁解牛」系列文章 | 逐模块的图文讲解 |
| 仓库 LEARNING_GUIDE.md | 源码阅读顺序指南 |

## 7. 自测：这些你都能讲清了吗？

- [ ] 为什么模板类的实现要写在头文件里？
- [ ] `threadpool::worker` 为什么必须是 static？`this` 是怎么传进去的？
- [ ] RAII 在 locker 和 connectionRAII 里分别怎么体现的？
- [ ] `std::map::find` 返回什么？怎么判断"没找到"？
- [ ] 局部静态变量单例为什么线程安全？
- [ ] 变参宏里的 `##__VA_ARGS__` 解决什么问题？
- [ ] `static` 成员变量为什么必须在类外定义一次？
- [ ] 构造函数初始化列表和函数体内赋值有什么区别？
