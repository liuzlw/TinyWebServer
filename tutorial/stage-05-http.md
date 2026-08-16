# Stage 5：HTTP 服务器

> 前面四阶段，我们一直在"收发字节"：客户端发来什么，服务器就原样回显什么。这一阶段要前进一步——**读懂 HTTP 协议**：把浏览器发来的字节流解析成"请求行 + 头部 + 正文"，根据 URL 找到磁盘上的文件，再用规范的 HTTP 响应把它发回去。做完这一阶段，你就有了一台能真正让浏览器访问网页和图片的静态文件服务器，`echo_server` 也就正式退役、升级成了 `server`。

学完本阶段你能：
- 手写 HTTP/1.1 的请求解析与响应构造，讲清状态码、keep-alive、Content-Length；
- 理解**主从状态机**这一解析字节流的经典套路，并逐行读懂仓库 `http_conn.cpp`；
- 会用 `mmap` 零拷贝发文件、`writev` 一次发两块内存、变参函数拼响应；
- 把 Stage 4 的 epoll + 线程池 Proactor 骨架，真正跑成一个 HTTP 服务器。

---

## 前置要求

- 已完成 [Stage 4](stage-04-epoll.md)，`my_tiny_webserver/` 下已有：`lock/locker.h`、`threadpool/threadpool.h`、`echo_server.cpp`（epoll 版）、`makefile`。
- 熟悉 C 字符串（`\0` 结尾）与 `strcpy`/`strlen`（Stage 2），熟悉枚举与位运算（Stage 4）。
- 本阶段结束后的工作区（本阶段起主程序改名 `server.cpp`，因为功能不再是 echo）：

```text
my_tiny_webserver/
├── lock/locker.h
├── threadpool/threadpool.h
├── server.cpp              ← 原 echo_server.cpp 的角色，改名并升级为 HTTP 主程序
├── http/
│   ├── http_conn.h
│   └── http_conn.cpp
├── root/
│   ├── index.html          ← 一个简单页面
│   └── test.jpg            ← 一张测试图片（自备，可从仓库 root/ 拷贝）
└── makefile
```

> **为什么改名**：`echo_server.cpp` 里全是回显逻辑，而 HTTP 服务器要做的是"解析请求 → 查文件 → 回响应"，职责完全不同。仓库里对应的是 `main.cpp` + `webserver.cpp` + `http/http_conn.cpp`；我们把它拆成"主程序 `server.cpp` + 连接类 `http_conn`"两层，与仓库一致。

---

## 理论学习

### 一、HTTP/1.1 请求报文

浏览器请求一个页面时，发给服务器的是一段纯文本字节流，格式固定：

```text
POST /login HTTP/1.1\r\n        ← 请求行：方法 空格 URL 空格 版本，以 \r\n 结尾
Host: 127.0.0.1:9006\r\n        ← 头部（零到多行，每行 "字段: 值"）
Connection: keep-alive\r\n
Content-Length: 27\r\n
\r\n                            ← 空行：头部结束的标志
user=admin&passwd=123456        ← 正文（可选，仅 POST/PUT 等有）
```

四部分：**请求行**、**头部**、**空行**、**正文**。注意每一行都以 `\r\n`（CRLF，回车+换行）结束，这是解析的关键锚点。

- **请求行**：`方法 请求目标 版本`，三个字段用空格分隔。方法常见 `GET`（取资源）与 `POST`（提交数据）。
- **头部**：键值对，如 `Host`（必带）、`Connection`、`Content-Length`。头部以空行 `\r\n` 结束。
- **正文**：只有带 body 的方法（如 POST）才有，长度由 `Content-Length` 指明。

**GET vs POST**：

| | GET | POST |
|---|---|---|
| 用途 | 获取资源（页面、图片） | 提交数据（登录、注册） |
| 数据位置 | 在 URL 的查询串里（`?a=1&b=2`） | 在正文里 |
| 有无正文 | 无 | 有，`Content-Length` 指明长度 |

本阶段只支持 `GET`；`POST` 会按"400 Bad Request"处理，等 [Stage 8](stage-08-mysql.md) 做注册登录时再支持。

### 二、响应报文与状态码

服务器读完请求后回一段响应：

```text
HTTP/1.1 200 OK\r\n               ← 状态行：版本 状态码 原因短语
Content-Type: text/html\r\n       ← 头部
Content-Length: 12\r\n
Connection: close\r\n
\r\n                              ← 空行
hello world                       ← 响应体（正文）
```

状态码三位数，第一位表大类：

| 状态码 | 含义 | 本项目何时返回 |
|---|---|---|
| 200 | OK，成功 | 正常返回文件 |
| 400 | Bad Request，请求语法错误 | 非 GET、版本不对、URL 非法 |
| 403 | Forbidden，禁止访问 | 文件无"其他人读"权限 |
| 404 | Not Found，资源不存在 | URL 对应文件不存在 |
| 500 | Internal Error，服务器内部错误 | 状态机走到 default 等异常 |

### 三、Connection: keep-alive（长连接）

HTTP/1.0 默认"一个请求一个连接"，响应发完就 `close`。HTTP/1.1 默认**长连接**：一次 TCP 连接可以承载多次请求，省去反复握手。

- 请求头 `Connection: keep-alive` → 服务器发完响应**不断开**，继续等下一个请求；
- 请求头 `Connection: close`（或没有 keep-alive）→ 服务器发完就关。

本项目的 `http_conn` 用成员 `m_linger` 记住这个选择，发完响应后据此决定是复用（`init()` 重置）还是关闭。

### 四、Content-Length

响应头里的 `Content-Length` 告诉客户端"响应体有多少字节"。没有它，客户端不知道读多少才算完，只能等服务器关连接来判尾。本项目对静态文件直接取文件大小 `m_file_stat.st_size` 作为 `Content-Length`。

### 五、静态资源服务流程

```text
浏览器请求 GET /test.jpg
      │
      ▼
服务器解析出 URL="/test.jpg"
      │
      ▼
拼出磁盘路径 = 文档根目录 + URL   （如 ./root/test.jpg）
      │
      ▼
stat 查文件：不存在→404；无读权限→403；是目录→400
      │
      ▼
open + mmap 把文件映射进内存
      │
      ▼
构造响应头 + writev 一次性发出"响应头 + 文件内容"
```

### 六、mmap：零拷贝发文件

传统发文件要经历多次拷贝：

```text
磁盘 ──► 内核页缓存 ──read──► 用户缓冲区 ──send──► 内核 socket 缓冲 ──► 网卡
                         ↑ 多一次"内核 → 用户"的拷贝
```

`mmap` 把文件**直接映射**到进程地址空间：访问这块内存就等于访问文件本身，不产生"内核→用户"的拷贝。发文件时，`writev` 直接把映射区交给内核发送，文件内容只从"页缓存"拷一次到 socket：

```text
磁盘 ──► 内核页缓存 ──mmap──► 用户地址空间直接指向页缓存（不拷贝）
                                    │
       响应头 m_write_buf ──┐        │
                            ▼        ▼
              writev 一次系统调用发"响应头 + 文件"两块内存
```

`mmap` 之后要用 `munmap` 解除映射，否则内存泄漏。

### 七、writev：一次发两块不连续内存

响应 = "响应头"（在 `m_write_buf`）+ "文件"（在 mmap 映射区），两块内存不连续。若分开 `send` 两次，就多一次系统调用；若先拼成一块再发，又要拷贝文件。`writev` 用 `struct iovec` 数组描述多块内存，**一次系统调用**把它们按顺序发出：

```cpp
struct iovec m_iv[2];
m_iv[0].iov_base = m_write_buf;      // 第 1 块：响应头
m_iv[0].iov_len  = m_write_idx;
m_iv[1].iov_base = m_file_address;   // 第 2 块：文件
m_iv[1].iov_len  = m_file_stat.st_size;
writev(m_sockfd, m_iv, 2);
```

---

## 本阶段 C++ 知识点

### 1. 主从状态机（本阶段核心）

HTTP 请求是"逐行"到来的字节流，但一行可能被 TCP 拆成好几段才到齐。于是用**两个状态机**协同：

- **主状态机** `m_check_state`（`CHECK_STATE` 枚举）：决定"现在该解析哪部分"——请求行 → 头部 → 正文；
- **从状态机** `parse_line()`（返回 `LINE_STATUS` 枚举）：负责"从字节流里切出一整行"。

```text
主状态机 m_check_state 的三态流转：

  CHECK_STATE_REQUESTLINE ──请求行解析完──► CHECK_STATE_HEADER
                                                  │ 头部解析完（返回 GET_REQUEST）
                                                  ▼
                                            do_request() 处理请求

从状态机 parse_line() 的返回值：
  LINE_OPEN  这一行还没到行尾（数据没到齐），等下一批 recv
  LINE_OK    切出了一整行，交给主状态机处理
  LINE_BAD   行格式错误（\r 后无 \n 等）
```

两者的配合写在 `process_read()` 的 `while` 条件里（后文详解），核心思想：**从状态机负责"凑够一行"，主状态机负责"这一行是什么"**。这两个枚举与仓库 `http_conn.h` 里的 `CHECK_STATE`、`LINE_STATUS` 完全对应。

### 2. 枚举 `enum`

把一组状态/取值命名为枚举，比裸 `0/1/2` 可读、还能让编译器参与检查：

```cpp
enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
enum HTTP_CODE  { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
                  FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
```

不写 `=` 时，枚举值从 0 依次 +1。`HTTP_CODE` 是"解析结果码"，在 `process_read`/`do_request`/`process_write` 之间传递。

### 3. C 字符串函数族（逐个讲透）

| 函数 | 作用 | 本阶段用法 |
|---|---|---|
| `strpbrk(s, " \t")` | 返回 `s` 中**第一个**空格或制表符的指针，找不到返回 `NULL` | 拆请求行 `GET / HTTP/1.1` 的空格 |
| `strspn(s, " \t")` | 返回 `s` **开头**连续由空格/制表符组成的长度 | 跳过字段值前的空白 |
| `strcasecmp(a, b)` | 忽略大小写比较整串，相等返回 0 | 判断 method 是 `GET`、版本是 `HTTP/1.1` |
| `strncasecmp(a, b, n)` | 忽略大小写比较**前 n 个字符** | 匹配 `Connection:`、`Content-length:`、`Host:` |
| `strchr(s, '/')` | 返回第一个 `/` 的指针 | 仓库用它剥离 `http://` 前缀（本阶段省略） |
| `strrchr(s, '/')` | 返回**最后**一个 `/` 的指针 | 仓库在 do_request 里定位 CGI 标志（本阶段省略） |
| `atol(s)` | 字符串转 `long` | `Content-Length: 27` 的 `27` 转数字 |
| `strcpy/strcat/strncpy` | 拷贝/拼接/限长拷贝 | 拼 `doc_root + URL` 路径 |

重点看 `parse_request_line` 里的组合用法：

```cpp
m_url = strpbrk(text, " \t");   // 找到 "GET" 后的第一个空格
*m_url++ = '\0';                // 空格改成 \0，把 "GET" 截成一个独立字符串
char *method = text;            // method 现在指向 "GET"

m_url += strspn(m_url, " \t");  // 跳过可能连续的空白
m_version = strpbrk(m_url, " \t"); // 再找 URL 后的空格
```

### 4. 变参函数（`va_list` / `vsnprintf`）

`add_response` 要像 `printf` 一样接受"格式串 + 任意多参数"，靠 `<stdarg.h>` 实现：

```cpp
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;                       // 1. 声明参数列表
    va_start(arg_list, format);             // 2. 从 format 之后开始取参数
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - 1 - m_write_idx,
                        format, arg_list);  // 3. 格式化写入，返回要写的长度
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);                   // 4. 收尾（每个 va_start 都要配 va_end）
        return false;                       // 缓冲区不够，失败
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}
```

`vsnprintf` 是 `snprintf` 的变参版，安全（不会写超）。它之后被一堆小函数包装：

```cpp
bool add_status_line(int status, const char *title) { return add_response("%s %d %s\r\n", "HTTP/1.1", status, title); }
bool add_content_length(int content_len)            { return add_response("Content-Length:%d\r\n", content_len); }
bool add_linger()                                   { return add_response("Connection:%s\r\n", m_linger ? "keep-alive" : "close"); }
```

### 5. `mmap` / `munmap`

```cpp
m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd);                         // 映射完立刻关 fd 也没关系
// ... 之后用 m_file_address 当数组读文件内容
munmap(m_file_address, m_file_stat.st_size);
```

参数：起始地址（`0` 让内核选）、长度、`PROT_READ` 只读、`MAP_PRIVATE` 私有映射、fd、偏移 0。返回值是映射后的地址；失败返回 `MAP_FAILED`。

### 6. `writev` / `struct iovec`

`iovec` 描述"一块内存的起始 + 长度"，`writev(fd, iov, count)` 一次发 `count` 块。见上文理论第七节，本项目用它"响应头 + 文件"一起发。

### 7. `stat` / `st_mode`

```cpp
struct stat m_file_stat;
stat(m_real_file, &m_file_stat);        // 把文件元信息读进 stat 结构
m_file_stat.st_size;                    // 文件大小（字节）
m_file_stat.st_mode & S_IROTH;          // 检查"其他人是否可读"权限位
S_ISDIR(m_file_stat.st_mode);           // 判断是否目录（S_ISDIR 是个宏）
```

`S_IROTH` 是"others 可读"位；`chmod 000` 后这一位为 0，于是返回 403。

### 8. 静态成员（正式登场）

`http_conn` 所有对象共享同一个 epoll 实例和一个"在线连接计数"，于是声明成静态成员：

```cpp
// http_conn.h 里声明
static int m_epollfd;
static int m_user_count;
// http_conn.cpp 里定义（只有这里真正分配内存）
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;
```

之后所有对象读写 `http_conn::m_epollfd` 都是同一份。**Stage 6 的定时器也要用到 `m_user_count`**，所以这个结构必须与仓库一致。

### 9. `map` 预告

仓库 `http_conn` 里有 `map<string, string> m_users;`，用来存"用户名 → 密码"（注册登录时查重/校验）。本阶段没有注册登录，省略它；[Stage 8](stage-08-mysql.md) 会引入 `map` 和 MySQL。

---

## 动手实现

> 约定：完整文件都用 ```cpp 给出并可编译；只贴片段的标注"节选"。学习者文件用 `my_tiny_webserver/xxx` 表示。

### 1. `http/http_conn.h`（连接类声明，完整最终版）

```cpp
#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH };
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
                     FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address() { return &m_address; }

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;
    int m_TRIGMode;
};

// 工具函数声明（定义在 http_conn.cpp，server.cpp 也会调用）
int setnonblocking(int fd);
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
void removefd(int epollfd, int fd);
void modfd(int epollfd, int fd, int ev, int TRIGMode);

#endif
```

说明：与仓库 `http/http_conn.h` 相比，删掉了本阶段用不到的东西（见"参考答案对照"）：`initmysql_result`、`timer_flag`、`improv`、`mysql`、`m_state`、`cgi`、`m_string`、`map<string,string> m_users`、`m_close_log`、`sql_user/sql_passwd/sql_name`，以及 `mysql/lst_timer/log/sql_connection_pool` 等 include。`static int m_epollfd; static int m_user_count;` 与仓库完全一致。

### 2. 第一步：最小 HTTP（先让浏览器通）

先写一个**最短可编译**的 `http_conn.cpp`：不解析细节，收到请求就硬编码回一个 `200 + hello`。目的是先把 `server.cpp + http_conn + makefile` 这条链路跑通。

完整 `my_tiny_webserver/http/http_conn.cpp`（stub 版）：

```cpp
#include "http_conn.h"

const char *ok_200_title = "OK";

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// ---- 工具函数：与 Stage 4 相同，后面几步不变 ----
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_TRIGMode = TRIGMode;
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;
    doc_root = root;
    init();
}

void http_conn::init()
{
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
            return false;
        return true;
    }
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// ---- 第一步 stub：假装解析，固定回 200 + hello ----
void http_conn::process()
{
    printf("got request, first line head: %.20s\n", m_read_buf);
    const char *response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
    send(m_sockfd, response, strlen(response), 0);
    // stub 不支持 keep-alive，发完直接交给主循环关闭
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--;
}
```

完整 `my_tiny_webserver/server.cpp`（最终版，之后不再改动）：

```cpp
// server.cpp —— Stage 5：静态文件 HTTP 服务器主程序
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <assert.h>
#include <signal.h>

#include "threadpool/threadpool.h"
#include "http/http_conn.h"

#define MAX_FD 65536
#define MAX_EVENT_NUMBER 10000

// 触发模式：0 = LT，1 = ET（与仓库默认 LT 一致）
#define TRIGMODE 0

int main(int argc, char *argv[])
{
    if (argc <= 2)
    {
        printf("usage: %s ip_address port_number\n", argv[0]);
        return 1;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // 忽略 SIGPIPE：向已关闭的连接写数据会触发 SIGPIPE 导致进程退出
    signal(SIGPIPE, SIG_IGN);

    // 1. 创建监听 socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    inet_pton(AF_INET, ip, &address.sin_addr);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    assert(bind(listenfd, (struct sockaddr *)&address, sizeof(address)) >= 0);
    assert(listen(listenfd, 5) >= 0);

    // 2. 创建 epoll 事件表，注册 listenfd（LT）
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);
    addfd(epollfd, listenfd, false, 0);
    http_conn::m_epollfd = epollfd;

    // 3. 线程池 + 连接对象数组
    threadpool<http_conn> *pool = new threadpool<http_conn>(8, 10000);
    http_conn *users = new http_conn[MAX_FD];

    // 4. 文档根目录 = 当前目录 + /root
    char root[200];
    getcwd(root, 200);
    strcat(root, "/root");

    // 5. 事件循环
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 新连接
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                if (connfd < 0)
                    continue;
                if (http_conn::m_user_count >= MAX_FD)
                {
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_address, root, TRIGMODE);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                // proactor：主线程读，读完交给线程池
                if (users[sockfd].read_once())
                {
                    pool->append(&users[sockfd]);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                if (!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}
```

`makefile`（Tab 缩进）：

```makefile
CXX ?= g++
CXXFLAGS += -g -Wall

server: server.cpp http/http_conn.cpp
	$(CXX) -o server server.cpp http/http_conn.cpp $(CXXFLAGS) -lpthread

clean:
	rm -f server
```

先放一个页面 `my_tiny_webserver/root/index.html`（图片先不急着放，稍后补）：

```html
<!DOCTYPE html>
<html>
    <head>
        <meta charset="UTF-8">
        <title>My Tiny WebServer</title>
    </head>
    <body>
        <h1>你好，TinyWebServer！</h1>
        <p>这是我的第一台 C++ 静态文件 HTTP 服务器。</p>
    </body>
</html>
```

编译运行验证：

```bash
cd ~/projects/my_tiny_webserver
mkdir -p http root
make
./server 127.0.0.1 9006
```

另开终端：

```bash
curl -v http://127.0.0.1:9006/
# 预期：HTTP/1.1 200 OK，body 是 "hello"
```

看到 `hello` 就说明"epoll 收数据 → 线程池处理 → 回响应"整条链路通了。**接下来把 stub 的 `process()` 换成真正的状态机。**

### 3. 第二步：主从状态机（完整解析）

把 `http/http_conn.cpp` 换成完整版（工具函数、`init`、`read_once` 与 stub 相同，新增的部分从 `parse_line` 开始）：

```cpp
#include "http_conn.h"

// HTTP 响应状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

// ---- 工具函数（与 stub 相同，见第一步）----
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接（外部调用）
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode)
{
    m_sockfd = sockfd;
    m_address = addr;
    m_TRIGMode = TRIGMode;          // 先存触发模式，再 addfd（修复仓库"先用后赋值"的顺序问题）
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;
    doc_root = root;
    init();
}

// 初始化新接受的连接
void http_conn::init()
{
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机：从字节流里切出一行（\r\n → \0\0）
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;                     // \r 是最后字节，\n 还没到
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;                          // \r 后面不是 \n
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;                          // 单独的 \n
        }
    }
    return LINE_OPEN;                                 // 扫完都没有行尾
}

// 循环读取客户数据，直到无数据可读或对方关闭
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT 读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
        {
            return false;
        }
        return true;
    }
    // ET 读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析 HTTP 请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else
        return BAD_REQUEST;   // 本阶段只支持 GET，POST 等返回 400（Stage 8 恢复）
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");   // "/" 映射 index.html（仓库是 judge.html）
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析 HTTP 头部
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        // 空行：头部结束
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        // 其它头部先忽略；Stage 7 会在这里打日志
    }
    return NO_REQUEST;
}

// 判断请求体是否读完整
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机：驱动 请求行 → 头部 → 正文 的解析
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 生成目标文件路径，并 mmap 到内存
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写数据：大文件分批发送、EAGAIN 重试、keep-alive 复用
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 根据解析结果拼响应
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(400, error_400_title);
        add_headers(strlen(error_400_form));
        if (!add_content(error_400_form))
            return false;
        break;
    }
    case NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        add_content_type();   // 学习者版补上（仓库定义了此函数却未调用，见正文）
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 工作线程入口：解析请求 → 拼响应 → 触发写
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
```

关键点逐段讲解：

**`parse_line()`（从状态机）**：逐字节扫描 `m_read_buf[m_checked_idx .. m_read_idx)`，遇到 `\r\n` 就把它俩都替换成 `\0`，这样 `get_line()`（返回 `m_read_buf + m_start_line`）拿到的就是一条干净的 C 字符串。三种返回：`LINE_OPEN`（行不完整，等下一批 recv）、`LINE_OK`（切出一行）、`LINE_BAD`（格式错）。`m_checked_idx` 是"检查到哪"，`m_read_idx` 是"已读到哪"，`m_start_line` 是"当前行起点"。

**`process_read()`（主状态机）**的 `while` 条件是本项目最经典的一行：

```cpp
while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
       ((line_status = parse_line()) == LINE_OK))
```

- 左半边：主状态机处在 `CHECK_STATE_CONTENT`（正文）时，正文是**按 Content-Length 字节数**切的，不再按行切，所以直接进循环体让 `parse_content` 判断是否读够。
- 右半边：其它情况先调用从状态机 `parse_line()` 切一行；只有 `LINE_OK` 才进循环体，把这一行按 `m_check_state` 分发给 `parse_request_line` / `parse_headers` / `parse_content`。若 `parse_line` 返回 `LINE_OPEN`（数据没到齐），循环退出，`process_read` 返回 `NO_REQUEST`，等下一批数据再继续——这就是"从状态机驱动主状态机"。

**`read_once()`**：与 Stage 4 一致。LT 读一次；ET 循环读到 `EAGAIN`。注意它在 LT 下也可能只读到半条请求行，剩下的交给状态机"等下一批"，所以状态机必须能处理不完整输入——这正是主从状态机存在的意义。

### 4. 第三步：`do_request()` 静态文件

`do_request()` 把 URL 拼成磁盘路径并检查：

```cpp
strcpy(m_real_file, doc_root);                     // ./root
strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);  // 追加 /index.html 或 /test.jpg
stat(m_real_file, &m_file_stat);                   // 查文件
if (!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST;  // 无 others 读权限 → 403
if (S_ISDIR(m_file_stat.st_mode))  return BAD_REQUEST;            // 目录 → 400
int fd = open(m_real_file, O_RDONLY);
m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd);                                         // 映射完即可关 fd
return FILE_REQUEST;
```

返回码依次对应 404（`stat` 失败 `NO_RESOURCE`）、403（无权限）、400（目录）、200（`FILE_REQUEST`）。

> **省略点（重要）**：仓库 `do_request()` 里还有一大段 CGI 逻辑——它检查 URL 中 `'0'/'1'/'2'/'3'/'5'/'6'/'7'` 这些后缀，把请求跳转到 `register.html`/`log.html`/`picture.html` 等页面，并执行注册/登录的 SQL（`http_conn.cpp` 第 388~457 行）。这部分依赖 MySQL，属于 [Stage 8](stage-08-mysql.md)，本阶段**整体省略**，只保留"静态文件"这一条路径（对应仓库第 499~514 行的 `else` 分支）。

### 5. 第四步：`process_write()` 与 `write()`

`process_write()` 根据解析结果码拼响应：错误码拼"状态行 + Content-Length + Connection + 空行 + 错误文案"；`FILE_REQUEST` 则拼状态行后，用 `m_iv[0]` 指响应头、`m_iv[1]` 指 mmap 的文件，`m_iv_count = 2`。

`write()` 是**分批发送**的核心：

```cpp
temp = writev(m_sockfd, m_iv, m_iv_count);
if (temp < 0) {
    if (errno == EAGAIN) { modfd(..., EPOLLOUT, ...); return true; }  // 内核发送缓冲满，下次 EPOLLOUT 再发
    unmap(); return false;                                            // 真错误，关连接
}
bytes_have_send += temp;
bytes_to_send -= temp;
// 响应头发完没？发完就把 m_iv[1] 指到文件剩余部分
if (bytes_have_send >= m_iv[0].iov_len) {
    m_iv[0].iov_len = 0;
    m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
    m_iv[1].iov_len = bytes_to_send;
} else {
    m_iv[0].iov_base = m_write_buf + bytes_have_send;
    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
}
if (bytes_to_send <= 0) {           // 全发完
    unmap();
    modfd(..., EPOLLIN, ...);
    if (m_linger) { init(); return true; }  // keep-alive：重置连接，等下一个请求
    else          return false;             // 否则关闭
}
```

要点：
- `bytes_to_send`（还要发多少）与 `bytes_have_send`（已发多少）两相抵消，大文件一次 `writev` 发不完就 `modfd(EPOLLOUT)`，等可写事件回来接着发。
- `EAGAIN` 表示发送缓冲满，不是错误，重挂 `EPOLLOUT` 等下次。
- 发完后，`m_linger == true` 就 `init()` 复用连接（把读缓冲、状态全部重置），否则返回 `false` 让主循环关闭。
- `process()` 末尾 `modfd(..., EPOLLOUT, ...)` 触发一次写；`write()` 里又用 `modfd(..., EPOLLIN, ...)` 把事件切回"读"，配合 `EPOLLONESHOT` 完成"读→写→读"的闭环。

> **两个相对仓库的修复（务必留意）**：
> 1. 仓库 `process_write` 的 `BAD_REQUEST` 分支错用了 404（`http_conn.cpp` 第 641~648 行），且没有 `NO_RESOURCE` 分支（请求不存在文件时走到 `default` 直接关连接，不回 404）。本阶段改为 **400 = BAD_REQUEST、新增 404 = NO_RESOURCE**，验收清单依赖这两个修复。
> 2. 仓库定义了 `add_content_type()` 却从未调用（响应里没有 `Content-Type`）。本阶段在 `FILE_REQUEST` 里补上调用，让响应头完整。`add_content_type()` 写死 `text/html`，页面正确；图片靠浏览器嗅探仍能正常显示，如何按扩展名区分留作思考题。

### 6. 第五步：集成与主程序

`server.cpp`（第一步已给出全文）就是 Stage 4 epoll 事件循环 + Proactor 线程池，只是把 `task` 换成了 `http_conn`：

- **读路径**（主线程）：`EPOLLIN` → `read_once()` 读进 `m_read_buf` → `pool->append(&users[sockfd])`。
- **业务**（工作线程）：`process()` 里 `process_read()` 解析 + `process_write()` 拼响应 + `modfd(EPOLLOUT)`。
- **写路径**（主线程）：`EPOLLOUT` → `write()` 分批发送；返回 `false` 就 `close_conn()`。
- `close_conn()` 里 `printf("close %d\n", m_sockfd)` 与仓库一致，先保留（Stage 7 换日志）。
- 本阶段**无定时器**（Stage 6）、**无信号管道**（Stage 6）、**无 SO_LINGER**（Stage 9）、**无日志**（Stage 7），主循环比仓库 `eventLoop` 精简。

放一张测试图片（从仓库拷一张即可）：

```bash
cp ~/projects/TinyWebServer/root/test1.jpg ~/projects/my_tiny_webserver/root/test.jpg
```

并在 `root/index.html` 里加上 `<img src="/test.jpg" alt="测试图片">`，重新 `make && ./server 127.0.0.1 9006`。

---

## 编译与运行

```bash
cd ~/projects/my_tiny_webserver
make          # 编译 server.cpp + http/http_conn.cpp，链接 -lpthread
./server 127.0.0.1 9006
```

浏览器访问 `http://127.0.0.1:9006/`，应看到页面与图片。命令行验证见验收清单。

---

## 验收清单

> 每条都要实际跑通并看到预期结果。

- [ ] **编译**：`make` 无报错，生成可执行文件 `server`。
- [ ] **200 与完整响应头**：`curl -v http://127.0.0.1:9006/` 输出含 `HTTP/1.1 200 OK`、`Content-Type: text/html`、`Content-Length:`、`Connection: close`，body 是 `root/index.html` 的内容。
- [ ] **浏览器访问**：浏览器打开 `http://127.0.0.1:9006/`，显示"你好，TinyWebServer！"页面。
- [ ] **图片 Content-Length 正确且渲染**：
  ```bash
  stat -c %s root/test.jpg                              # 记下文件字节数 N
  curl -s -D - -o /dev/null http://127.0.0.1:9006/test.jpg | grep -iE 'HTTP/|Content-Length'
  ```
  预期 `HTTP/1.1 200 OK`，且 `Content-Length: N`（等于上面的字节数）；浏览器打开 `http://127.0.0.1:9006/test.jpg` 能看到图片。
- [ ] **404**：`curl -v http://127.0.0.1:9006/nonexist.html`，预期 `HTTP/1.1 404 Not Found`，body 含 `The requested file was not found on this server.`
- [ ] **403**：
  ```bash
  echo "secret" > root/secret.txt && chmod 000 root/secret.txt
  curl -v http://127.0.0.1:9006/secret.txt              # 预期 HTTP/1.1 403 Forbidden
  chmod 644 root/secret.txt                             # 记得改回权限
  ```
- [ ] **POST 返回 400**：`curl -v -X POST http://127.0.0.1:9006/`，预期 `HTTP/1.1 400 Bad Request`。
- [ ] **gdb 跟踪一次完整请求**：
  ```bash
  gdb ./server
  (gdb) break http_conn::parse_request_line
  (gdb) run 127.0.0.1 9006
  ```
  另开终端 `curl http://127.0.0.1:9006/`，gdb 停在 `parse_request_line`；`print text` 应显示 `GET / HTTP/1.1`（或其变体），`continue` 后 curl 拿到 200。
- [ ] **长连接复用**：
  ```bash
  curl -v -H "Connection: keep-alive" http://127.0.0.1:9006/ http://127.0.0.1:9006/test.jpg -o /dev/null 2>&1 | grep -i 'Re-using'
  ```
  预期出现 `Re-using existing connection`（说明第二个请求复用了第一个请求的 TCP 连接）。
- [ ] **服务端日志**：上述操作后，服务端终端应打印若干 `close N`，进程不崩溃，`Ctrl+C` 才退出。

---

## 参考答案对照

对照仓库 `http/http_conn.h` 与 `http/http_conn.cpp`。

**逐函数对照表（本阶段省略/修改了哪些）**：

| 函数 / 成员 | 本阶段 | 仓库 | 说明 |
|---|---|---|---|
| `init(sockfd, addr, root, TRIGMode)` | 4 参 | 7 参（多 `close_log/user/passwd/sqlname`） | 省略日志开关与数据库账号，Stage 7/8 恢复 |
| `init()`（私有） | 有 | 有 | 仓库额外初始化 `mysql/cgi/m_state/timer_flag/improv`，本阶段删去 |
| `initmysql_result` | 删 | 有 | 依赖 MySQL，Stage 8 恢复 |
| `close_conn` | 同 | 同 | `printf("close %d\n")` 与仓库一致 |
| `read_once` | 同 | 同 | LT/ET 两分支一致 |
| `parse_line` | 同 | 同 | 逐字符完全一致 |
| `parse_request_line` | 只 GET | 支持 GET/POST + 剥 `http://` 前缀 + `/`→`judge.html` | 本阶段 `/`→`index.html`；POST 与前缀剥离省略 |
| `parse_headers` | 同（少日志） | 同 | 未知头部仓库打 `LOG_INFO`，本阶段忽略 |
| `parse_content` | 删 `m_string` | 存 `m_string` | `m_string` 供 Stage 8 取用户名密码 |
| `process_read` | 同 | 同 | `while` 主循环一致 |
| `do_request` | 仅静态文件 | 静态文件 + CGI（`'0'/'1'/'2'/'3'/'5'/'6'/'7'`） | CGI 整段省略，Stage 8 恢复 |
| `process_write` | 400/404 修正 + 补 `Content-Type` | BAD_REQUEST 误用 404、无 NO_RESOURCE 分支、`add_content_type` 未调用 | 见"第四步"两个修复 |
| `write` | 同 | 同 | 分批发送逻辑一致 |
| `add_response` 及 `add_*` 一族 | 同（少日志） | 同 | 变参拼响应一致 |
| `process` | 同 | 同 | 末尾 `modfd(EPOLLOUT)` 一致 |
| 成员 `mysql/m_state/cgi/m_string/m_users/m_close_log/sql_*` | 删 | 有 | 分别属 Stage 8（MySQL）与 Stage 7（日志） |
| `static m_epollfd / m_user_count` | 同 | 同 | Stage 6 定时器要用，保持一致 |

**主程序对照**：本阶段 `server.cpp` 对应仓库 `webserver.cpp::eventLoop` + `main.cpp` 的一部分，但去掉了 `trig_mode` 四种组合（用单个 `TRIGMODE` 宏）、信号/定时器、日志、SO_LINGER、连接池。

---

## 常见问题

1. **`curl: (52) Empty reply from server`**。服务器解析到非法状态码走到了 `process_write` 的 `default` 直接 `close_conn()`，没回任何数据。最常见原因是 `parse_request_line` 对版本/方法判得不对，或 `do_request` 返回了未在 `process_write` 里处理的码。用 gdb 断在 `process_write` 看 `ret` 是哪个值。
2. **浏览器反复转圈、页面出不来，但 `curl /` 正常**。多半是 `index.html` 里 `<img src="/test.jpg">` 指向的文件不存在，而浏览器在等图片；或者响应里没有 `Content-Length`（`add_content_type` 的调用位置放到了 `add_headers` 之后）。检查 `Content-Length` 是否等于文件大小。
3. **访问不存在的路径没回 404 而是连接被直接断开**。这是仓库原版行为（`NO_RESOURCE` 未在 `process_write` 处理）。确认你加了 `case NO_RESOURCE:` 分支。
4. **POST 返回 404 而不是 400**。如果你照抄了仓库 `process_write`，它的 `BAD_REQUEST` 分支写的是 404。按本阶段改为 `add_status_line(400, error_400_title)`。
5. **`chmod 000` 后访问还是 200**。`S_IROTH` 只检查"其他人"位；若服务器以文件属主身份运行，`open` 对属主可能仍成功——但我们的检查发生在 `open` 之前，`chmod 000` 会同时清掉 others 位，应返回 403。若没有，检查你是否 `chmod` 了正确文件、或服务器根目录路径不对（`getcwd` 拼 `/root` 时的相对路径问题）。
6. **keep-alive 不生效，`curl` 每次都新建连接**。`curl` 默认不带 `Connection: keep-alive` 头，本服务器只在该头出现时才 `m_linger = true`。测试时务必加 `-H "Connection: keep-alive"`。
7. **大图片发到一半就断，或只显示上半截**。检查 `write()` 里 `bytes_have_send` / `bytes_to_send` 的更新与 `m_iv` 重定向逻辑；尤其 `m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx)` 里减的是 `m_write_idx`（响应头长度）。
8. **`send`/`writev` 触发 SIGPIPE 把进程打死**。`server.cpp` 里必须有 `signal(SIGPIPE, SIG_IGN);`，否则客户端中途断开、服务器再写会收到 SIGPIPE 直接退出。

---

## 思考题

1. 为什么 `process_read()` 要用 `while` 循环，而不是"读一行 → 解析一行 → 返回"？如果一次 `recv` 只收到了半个请求行，程序会发生什么？
2. `parse_line()` 返回 `LINE_OPEN` 有什么意义？它和"数据没到齐，要等下一批 recv"是怎么对应的？
3. `mmap` 发送文件和 `read` 进缓冲区再 `send` 相比，省掉了哪一步拷贝？`mmap` 之后为什么可以立刻 `close(fd)`？
4. ET 模式下，`read_once()` 为什么循环读到 `EAGAIN` 才算完？如果某次 `recv` 只读到半个请求，状态机如何保证下次还能继续解析？
5. `write()` 里 `bytes_have_send >= m_iv[0].iov_len` 这行在判断什么？为什么重定向 `m_iv[1]` 时要减去 `m_write_idx`？
6. 现在的 `add_content_type()` 写死 `text/html`，访问 `.jpg` 时响应头其实是错的（靠浏览器嗅探才正常显示）。如果要按 `.html`/`.jpg` 等扩展名返回正确的 `Content-Type`，你会怎么设计？（提示：Stage 8 会用到 `map`。）

---

## 下一步

你已经有一台能解析 HTTP、发静态文件的服务器。但它还不会"管理"连接：客户端连上后若一直不请求，连接会永远挂着。在 [Stage 6：定时器](stage-06-timer.md) 里，我们会给每个连接挂一个"倒计时"，超时未活动就自动踢下线——这正好用上本阶段保持一致的 `http_conn::m_user_count` 静态成员，并引入信号 + 管道把定时器接进 epoll 事件循环。
