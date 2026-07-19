# Stage 5：HTTP 状态机解析

> 🎯 **本阶段目标**：实现项目的核心模块 `http/http_conn.{h,cpp}` ——
> 用**主从状态机**严谨地解析 HTTP 请求，用 **mmap + writev** 高效发送文件，
> 支持 **keep-alive** 长连接。这是整个项目最值得精读的代码。

## 📚 理论铺垫

### 5.1 为什么需要状态机？

Stage 4 的解析有个致命假设：**一次「循环 read」能拿到完整的 HTTP 请求**。
但 TCP 是流协议，没有消息边界：

```
情况1: 一个请求被拆成两个 TCP 包到达  → 第一次 read 只拿到半个请求
情况2: 两个请求合并在一个 TCP 包里    → 一次 read 拿到一个半请求
```

所以我们需要一个**可以增量执行的解析器**：读多少解析多少，解析不完记住进度，
下次数据来了接着解析。**状态机**就是这个「记住进度」的天然模型。

### 5.2 主从状态机设计

**从状态机**负责「切行」：HTTP 以 `\r\n` 分隔每一行，逐字符扫描 buffer，
每遇到一对 `\r\n` 就切出一行：

```
每个字符只有三种可能：读到 '\r' → 期待下一个必须是 '\n'
                      读到 '\n' → 上一字符必须是 '\r'，成功切出一行
                      其他     → 普通字符，继续
```

**主状态机**负责「这行是什么」：HTTP 报文三部分 —— 请求行、头部、消息体，
解析进度用三个状态表示：

```
CHECK_STATE_REQUESTLINE  (解析请求行)
        │  切出一行 → parse_request_line()
        ▼
CHECK_STATE_HEADER       (解析头部)
        │  切出一行 → parse_headers()；遇到空行
        ▼
CHECK_STATE_CONTENT      (解析消息体，仅 POST 有)
        │
        ▼
   解析完成 → do_request() → 生成响应
```

整体流程（对应原始项目的 `process_read`）：

```
while (buffer 里有完整的行) {
    从状态机切一行
    主状态机根据当前状态处理这一行
}
buffer 里不够一行？→ 等下一轮数据（返回 NO_REQUEST）
解析出完整请求？  → 返回 GET_REQUEST，进入 do_request
```

### 5.3 mmap：把文件"映射"进内存

Stage 2-4 发送文件的方式是 `fread` 整个文件到 buffer 再 write，
一个 39MB 的视频就要占 39MB 用户态内存，还要从内核态拷贝一次。

`mmap` 把磁盘文件直接映射到进程的虚拟地址空间，**读它就像读内存数组**，
数据由内核按需从磁盘加载，零额外内存占用：

```cpp
int fd = open(path, O_RDONLY);
void* addr = mmap(0, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd);                    // mmap 之后 fd 就可以关了
// addr[0..file_size-1] 就是文件内容
munmap(addr, file_size);      // 用完解除映射
```

### 5.4 writev：一次系统调用发多块内存

响应头和文件内容在两块不同的内存里。`writev` 用「iovec 数组」一次发出去，
避免两次 write 的系统调用开销，也避免把两块拼成一块的拷贝：

```cpp
struct iovec iv[2];
iv[0].iov_base = 响应头;  iv[0].iov_len = 头长度;
iv[1].iov_base = mmap的文件; iv[1].iov_len = 文件大小;
writev(sockfd, iv, 2);
```

### 5.5 keep-alive 长连接

HTTP/1.1 默认长连接：一个 TCP 连接上可以串行传多个请求，省掉反复建连的开销。
要点：响应头带 `Connection: keep-alive` + 准确的 `Content-Length`，
发送完后**不 close，而是用 modfd 重新注册 EPOLLIN**，等客户端的下一个请求。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| `enum` 枚举类型 | 主/从状态机的状态、解析结果 |
| `static` 成员变量与类内常量 | `READ_BUFFER_SIZE` 等 |
| 字符指针操作 `strpbrk`/`strncasecmp`/`strcpy` | 行解析（C 风格字符串实战） |
| `mmap`/`munmap`、`struct stat` | 文件映射 |
| `iovec`/`writev` | 分散写 |
| 类的 .h/.cpp 分离 | 第一个正规模块！声明在 .h，实现在 .cpp |

## 🔨 动手实现

在 `my_tiny_webserver/http/` 下创建两个文件。这是核心代码，建议**先自己按下面的
结构写一遍**，写不出再看原始项目的 `http/http_conn.h/.cpp`（下面给出的是
贴合原版的精讲版骨架 + 关键实现，结构上与原版一致）。

### 5.1 `http/http_conn.h`

```cpp
#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

class http_conn {
public:
    // ---------- 常量 ----------
    static const int FILENAME_LEN = 200;        // 文件路径最大长度
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    // HTTP 方法（本项目只真正实现 GET/POST）
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH };
    // 主状态机：当前在解析请求的哪一部分
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    // 解析结果
    enum HTTP_CODE { NO_REQUEST,      // 请求不完整，继续读
                     GET_REQUEST,     // 拿到完整请求
                     BAD_REQUEST,     // 语法错误
                     NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST,  // 文件类结果
                     INTERNAL_ERROR, CLOSED_CONNECTION };
    // 从状态机：当前行的读取状态
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

    void init(int sockfd, const sockaddr_in& addr);
    void close_conn(bool real_close = true);
    void process();                    // 线程池调用：读解析 + 写响应
    bool read_once();                  // 把 socket 数据读进 m_read_buf
    bool write();                      // 把响应写出去

private:
    void init();                       // 重置所有解析状态（keep-alive 复用对象）
    HTTP_CODE process_read();          // 主状态机驱动
    bool process_write(HTTP_CODE ret); // 根据解析结果拼响应
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();            // 把 URL 映射到文件
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();          // 从状态机：切一行
    void unmap();
    bool add_response(const char* format, ...);   // 往写 buffer 里追加
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char* content);

public:
    static int m_epollfd;              // 所有连接共享一个 epollfd
    static int m_user_count;           // 在线用户数统计

private:
    int m_sockfd;
    sockaddr_in m_address;

    char m_read_buf[READ_BUFFER_SIZE];
    int m_read_idx;                    // buffer 中已读数据的下一个位置
    int m_checked_idx;                 // 从状态机当前分析到哪个字符
    int m_start_line;                  // 当前行在 buffer 中的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;

    CHECK_STATE m_check_state;         // 主状态机当前状态
    METHOD m_method;
    char m_real_file[FILENAME_LEN];    // 请求文件的完整路径
    char* m_url;
    char* m_version;
    char* m_host;
    int m_content_length;              // POST 消息体长度
    bool m_linger;                     // 是否 keep-alive

    char* m_file_address;              // mmap 出的文件内存
    struct stat m_file_stat;
    struct iovec m_iv[2];              // writev 用
    int m_iv_count;
    int m_bytes_to_send;
    int m_bytes_have_send;
};

#endif
```

### 5.2 `http/http_conn.cpp` 关键实现精讲

完整代码较长，下面按函数给出**必须理解的核心**，完整版照着原始项目
`http/http_conn.cpp` 写（两者几乎一致，这正是"复现"的意义）。

**(1) 工具函数：epoll 的 add/remove/mod**

```cpp
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    if (one_shot) event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}
```

**(2) 从状态机 parse_line：逐字符切行**

```cpp
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            // '\r' 是 buffer 最后一个字符：数据不完整，等下次
            if ((m_checked_idx + 1) == m_read_idx) return LINE_OPEN;
            // 下一个必须是 '\n'，否则语法错误
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';   // 把 \r\n 改成 \0\0
                m_read_buf[m_checked_idx++] = '\0';   // 这样每行就是 C 字符串
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;   // 没读到完整一行
}
```

> 🔑 精髓：把 `\r\n` 原地改成 `\0\0`，之后每行直接用 `char*` 当字符串处理，
> 零拷贝。`m_checked_idx` 和 `m_start_line` 就是"记住的进度"。

**(3) 主状态机 process_read**

```cpp
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 条件：解析消息体时不用按行处理；或者成功切出一行
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;

        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE: {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER: {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            else if (ret == GET_REQUEST) return do_request();   // 无消息体，完成
            break;
        }
        case CHECK_STATE_CONTENT: {
            ret = parse_content(text);
            if (ret == GET_REQUEST) return do_request();
            line_status = LINE_OPEN;    // 消息体可能不完整
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;   // 数据不够，等下一波
}
```

**(4) parse_request_line：解析 `GET /index.html HTTP/1.1`**

```cpp
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // strpbrk：在 text 中找第一个属于 " \t" 的字符
    m_url = strpbrk(text, " \t");
    if (!m_url) return BAD_REQUEST;
    *m_url++ = '\0';            // 把空格变 \0，text 截断为方法名，m_url 指向 URL

    char* method = text;
    if (strcasecmp(method, "GET") == 0) m_method = GET;
    else if (strcasecmp(method, "POST") == 0) m_method = POST;
    else return BAD_REQUEST;

    m_url += strspn(m_url, " \t");              // 跳过连续空格
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0) return BAD_REQUEST;

    if (strncasecmp(m_url, "http://", 7) == 0) { // 处理完整 URL 形式
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0) {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') return BAD_REQUEST;
    if (strlen(m_url) == 1) strcat(m_url, "index.html");  // "/" → 默认页

    m_check_state = CHECK_STATE_HEADER;         // ★ 主状态机转移
    return NO_REQUEST;
}
```

**(5) parse_headers：逐行处理头部，空行结束**

```cpp
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    if (text[0] == '\0') {              // 空行：头部结束
        if (m_content_length != 0) {    // 有消息体 → 转 CONTENT 状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;             // 无消息体 → 请求解析完毕
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) m_linger = true;
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    // 其他头部：本项目忽略（学习阶段可 printf 出来看看）
    return NO_REQUEST;
}
```

**(6) do_request：URL → 磁盘文件 + mmap**

```cpp
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0) return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH)) return FORBIDDEN_REQUEST;  // 无读权限
    if (S_ISDIR(m_file_stat.st_mode)) return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ,
                                 MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
```

**(7) process_write：根据解析结果拼响应（用可变参数函数 add_response）**

```cpp
bool http_conn::add_response(const char* format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx,
                        WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) { va_end(arg_list); return false; }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
    case FILE_REQUEST: {
        add_status_line(200, ok_200_title);         // "HTTP/1.1 200 OK\r\n"
        if (m_file_stat.st_size != 0) {
            add_headers(m_file_stat.st_size);       // Content-Length/Type/Linger + 空行
            m_iv[0].iov_base = m_write_buf;  m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address; m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            m_bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        // 空文件的特殊处理……
    }
    case BAD_REQUEST:      /* add_status_line(400,...); add_headers(strlen(...)); add_content(...); */
    // ……其他状态码类似
    }
    m_iv[0].iov_base = m_write_buf; m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    m_bytes_to_send = m_write_idx;
    return true;
}
```

**(8) write：非阻塞 writev 循环 + 处理部分写**

```cpp
bool http_conn::write() {
    int temp = 0;
    if (m_bytes_to_send == 0) {          // 没东西可发：重新等输入
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }
    while (1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            if (errno == EAGAIN) {       // 发送缓冲区满了：注册可写事件，下次继续
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;                // 出错：关闭连接
        }
        m_bytes_have_send += temp;
        m_bytes_to_send -= temp;
        if (m_bytes_have_send >= m_iv[0].iov_len) {
            // 头发完了，剩余的是文件部分
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (m_bytes_have_send - m_write_idx);
            m_iv[1].iov_len = m_bytes_to_send;
        } else {
            // 头还没发完
            m_iv[0].iov_base = m_write_buf + m_bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - m_bytes_have_send;
        }
        if (m_bytes_to_send <= 0) {      // 全部发完
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);  // ★ 重新等读
            if (m_linger) { init(); return true; }            // keep-alive
            else return false;                                 // 关闭连接
        }
    }
}
```

**(9) process：线程池的统一入口**

```cpp
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {            // 请求不完整：重新注册读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) close_conn();
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);  // 注册可写，准备发响应
}
```

### 5.3 接入 Stage 4 的主循环

主循环把 Stage 4 的 `http_task` 换成 `http_conn`：

- `read_once()` 由工作线程（Reactor）或主线程（Proactor）调用
- `EPOLLIN` 事件 → Reactor 直接 `pool->append(users + sockfd)`；
  简化起见本阶段在 `process()` 开头调用 `read_once()` 的变体也可以，
  但建议保持与原版一致：**EPOLLIN 时先 read_once 再 append**（这就是模拟 Proactor！
  你已经顺手实现了它）

```cpp
else if (events[i].events & EPOLLIN) {
    if (users[sockfd].read_once()) {
        pool->append(users + sockfd);
        // （定时器逻辑 Stage 6 加在这里）
    } else {
        users[sockfd].close_conn();
    }
}
else if (events[i].events & EPOLLOUT) {
    if (!users[sockfd].write()) users[sockfd].close_conn();
}
```

CMakeLists.txt 更新：

```cmake
add_executable(server
    main.cpp
    http/http_conn.cpp
)
```

## ✅ 验证

**验证 1：curl 看 keep-alive**

```bash
curl -v http://127.0.0.1:9006/index.html
# 期望响应头：Connection: keep-alive，且 curl 结束后连接按预期处理
```

**验证 2：大文件传输（mmap + writev 的价值）**

root 目录里有几 MB 的图片和视频。下载并校验完整性：

```bash
curl -s http://127.0.0.1:9006/xxx.jpg -o /tmp/dl.jpg   # 换成 root/ 里真实文件名
md5sum root/xxx.jpg /tmp/dl.jpg
# 两个 md5 必须一致！说明 mmap 传输零损坏
```

**验证 3：拆包测试（状态机的价值）**

用 Python 模拟「一个请求分两次发」：

```bash
python3 -c "
import socket, time
s = socket.create_connection(('127.0.0.1', 9006))
s.send(b'GET /index.html HT')      # 半个请求
time.sleep(1)                       # 停 1 秒再发另一半
s.send(b'TP/1.1\r\nHost: x\r\n\r\n')
print(s.recv(200))
"
# 期望：依然能拿到 HTTP/1.1 200 OK —— 状态机正确攒住了半个请求
```

**验证 4：错误请求**

```bash
printf 'GARBAGE\r\n\r\n' | nc 127.0.0.1 9006 | head -1
# 期望：HTTP/1.1 400 Bad Request
```

**验证 5：长连接复用**

```bash
curl -v http://127.0.0.1:9006/index.html http://127.0.0.1:9006/index.html 2>&1 | grep -E "(Re-using|Connection #)"
# 期望看到 "Re-using existing connection" —— 第二个请求复用了同一 TCP 连接
```

## 🐛 常见问题

**Q1: 大文件传输总是不完整/连接被重置？**
`write()` 里处理部分写的逻辑有 bug。用 gdb 在 write 里打印 `m_bytes_to_send`/
`m_bytes_have_send`，对照 `iv` 的调整逻辑。这是本模块最容易错的地方。

**Q2: 服务器内存暴涨或 munmap 崩溃？**
mmap 后忘记 munmap（泄漏），或重复 munmap（崩溃）。`unmap()` 里要置空
`m_file_address`，重复调用前先判空。

**Q3: keep-alive 不生效，每个请求都新建连接？**
检查：① 响应头 `Connection: keep-alive` 是否发出 ② `Content-Length` 是否准确
③ 写完后是否 `modfd(EPOLLIN)` + `init()` 重置状态。`init()` 漏了会导致
下一个请求读到上次的残留状态。

## 🤔 思考与练习

1. 画出主从状态机的完整状态转移图（纸笔），标注每个转移的条件。
2. 在 `parse_headers` 里加 `printf` 打印每一行头部，用浏览器访问，观察真实浏览器的请求头。
3. 故意把 `Content-Length` 改大 10，观察 curl 的行为 —— 理解为什么长度必须准确。
4. 面试题自答：为什么用 mmap 而不是 read？writev 相比两次 write 好在哪里？
   EPOLLONESHOT 解决什么问题？
5. 对比你写的 `http_conn.cpp` 和原始项目版本：`diff http/http_conn.cpp ../../http/http_conn.cpp`，
   找出 POST/登录注册相关的差异（Stage 8 补全）。

---

➡️ 下一阶段：[Stage 6：定时器处理非活跃连接](stage-06-timer.md)
