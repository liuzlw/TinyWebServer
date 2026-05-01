# TinyWebServer 学习路线

## 先看整体

这个项目是一台 Linux 下的轻量级 C++ Web 服务器。主线是：

1. `main.cpp` 解析参数并装配 `WebServer`
2. `webserver.cpp` 创建监听 socket、注册 epoll、处理事件循环
3. `http/http_conn.cpp` 读取请求、解析 HTTP、生成响应
4. `threadpool/threadpool.h` 把连接任务交给工作线程
5. `CGImysql/` 提供 MySQL 连接池和 RAII 归还连接
6. `timer/` 用升序链表定时器清理非活跃连接
7. `log/` 提供同步或异步日志

## 阅读顺序

### 1. 启动流程

从 `main.cpp` 开始，重点看 6 个步骤：

- `Config::parse_arg`
- `WebServer::init`
- `WebServer::log_write`
- `WebServer::sql_pool`
- `WebServer::thread_pool`
- `WebServer::eventListen`
- `WebServer::eventLoop`

这里先不要钻细节，只要知道每个模块什么时候初始化。

### 2. 事件循环

重点阅读 `webserver.cpp`：

- `eventListen`：socket、bind、listen、epoll、信号管道、alarm
- `eventLoop`：epoll_wait 后分发事件
- `dealclientdata`：accept 新连接
- `dealwithread`：处理读事件
- `dealwithwrite`：处理写事件
- `dealwithsignal`：把信号转成管道上的普通 IO 事件

读完后你应该能画出一条链路：

```text
浏览器连接 -> listenfd 可读 -> accept -> connfd 注册 epoll
请求到达 -> connfd EPOLLIN -> read_once/process -> connfd EPOLLOUT
响应发送 -> write/writev -> keep-alive 切回 EPOLLIN 或关闭连接
```

### 3. HTTP 状态机

重点阅读 `http/http_conn.cpp`：

- `read_once`：LT/ET 两种模式下如何读 socket
- `parse_line`：按 CRLF 切行
- `parse_request_line`：解析请求行
- `parse_headers`：解析请求头
- `parse_content`：解析 POST body
- `process_read`：主状态机
- `do_request`：把 URL 映射到静态文件或登录/注册逻辑
- `process_write`：拼 HTTP 响应
- `write`：用 `writev` 发送响应头和文件内容

这个文件是项目最值得反复看的部分。

### 4. 并发模型

看 `threadpool/threadpool.h` 和 `webserver.cpp` 的读写分支：

- Proactor：主线程先读 socket，再把已读好的请求交给工作线程处理
- Reactor：主线程只投递事件，工作线程自己读或写 socket

对应入口：

- `WebServer::dealwithread`
- `WebServer::dealwithwrite`
- `threadpool<T>::run`

### 5. 资源管理

按这个顺序看支撑模块：

- `lock/locker.h`：互斥锁、信号量、条件变量封装
- `CGImysql/sql_connection_pool.cpp`：连接池的取出和归还
- `timer/lst_timer.cpp`：定时器链表的 add/adjust/del/tick
- `log/log.cpp` 和 `log/block_queue.h`：异步日志的生产者/消费者模型

## 建议实验

1. 只跑静态页面：先注释或绕过数据库初始化，理解基本 HTTP 服务。
2. 打印事件流：在 `eventLoop` 中给 listen/read/write/signal 分支加日志。
3. 对比 LT 和 ET：用 `./server -m 0` 与 `./server -m 3` 对照 `read_once` 行为。
4. 对比 Reactor 和 Proactor：用 `./server -a 0` 与 `./server -a 1` 看读写责任在哪里。
5. 用 `curl -v http://127.0.0.1:9006/` 观察请求和响应头。

## 面试和自测问题

- 为什么 socket 要设置非阻塞？
- ET 模式为什么必须循环读到 `EAGAIN`？
- 为什么连接 fd 使用 `EPOLLONESHOT`？
- Reactor 和 Proactor 在这个项目里的区别是什么？
- `writev` 为什么适合发送 HTTP 响应？
- 定时器为什么要和连接 fd 绑定？
- MySQL 连接池为什么用 RAII 包装？
- 异步日志的阻塞队列解决了什么问题？

