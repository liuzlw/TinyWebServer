# Phase 6 —— HTTP 解析器

## 本阶段目标

实现一个 HTTP 连接处理类 `http_conn`，能：
1. 从 socket 读取原始 HTTP 请求（LT/ET 两种模式）
2. 用**有限状态机**解析 HTTP 请求（请求行 → 请求头 → 请求体）
3. 构造正确的 HTTP 响应（状态行 + 响应头 + Body）
4. 用 `mmap` + `writev` 高效发送静态文件

**可见结果：** 给定一个原始 HTTP 请求文本，程序输出正确的 HTTP 响应。最终能通过 curl 和服务器的 telnet 测试。

**验收标准：**

- [ ] 正确解析 GET 请求：提取 URL、Host、Connection 等头
- [ ] 正确解析 POST 请求：提取请求体中的表单数据
- [ ] GET 静态文件返回 200 + 文件内容
- [ ] 不存在的文件返回 404
- [ ] POST 登录/注册逻辑正确（配合 Phase 8 集成测试）
- [ ] `mmap` 映射文件，`writev` 一次发送响应头 + 文件内容

---

## 理论与机制

### 1. HTTP/1.1 报文格式

一个 HTTP 请求的原始文本长这样：

```
GET /index.html HTTP/1.1\r\n          ← 请求行
Host: www.example.com\r\n             ← 请求头
Connection: keep-alive\r\n
Content-Length: 0\r\n
\r\n                                   ← 空行（请求头结束）
                                       ← 请求体（GET 请求通常为空）
```

一个 POST 请求：

```
POST /login HTTP/1.1\r\n              ← 请求行
Host: www.example.com\r\n             ← 请求头
Content-Length: 19\r\n
Content-Type: application/x-www-form-urlencoded\r\n
\r\n                                   ← 空行
user=admin&passwd=123                 ← 请求体
```

**每个部分的具体格式：**

| 部分 | 格式 | 示例 |
|------|------|------|
| 请求行 | `METHOD URL HTTP/1.1\r\n` | `GET /index.html HTTP/1.1\r\n` |
| 请求头 | `Name: Value\r\n` | `Host: localhost\r\n` |
| 空行 | `\r\n` | 请求头结束标志 |
| 请求体 | 任意数据 | `user=admin&passwd=123` |

### 2. 为什么用有限状态机？

解析 HTTP 是一个**逐行处理**的过程，不同阶段有不同的合法格式。用状态机可以：

1. **逻辑清晰**：每个状态只关心当前阶段的数据格式
2. **便于扩展**：新增一个 HTTP 方法/头只要改对应状态的处理
3. **便于错误处理**：任何状态遇到非法数据 → 立即返回 BAD_REQUEST
4. **避免回溯**：一次遍历即可完成解析

```
                 ┌──────────┐
        开始 ──▶ │ 请求行    │
                 │ REQUESTLINE│
                 └─────┬────┘
                       │ 解析完请求行
                 ┌─────▼────┐
                 │ 请求头    │
                 │ HEADER   │
                 └─────┬────┘
                       │
            ┌──────────┼──────────┐
            │ 空行      │         │ Content-Length > 0
            │ GET请求   │         │
       ┌────▼────┐ ┌───▼───┐
       │ do_     │ │ 请求体 │
       │ request │ │ CONTENT│
       └─────────┘ └───┬───┘
                       │ 读完请求体
                  ┌────▼────┐
                  │ do_     │
                  │ request │
                  └─────────┘
```

### 3. 主状态机 + 从状态机

本项目用了两层状态机：

**主状态机（CHECK_STATE）**：控制"现在在解析哪个部分"
```cpp
enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0,  // 正在解析请求行
    CHECK_STATE_HEADER,           // 正在解析请求头
    CHECK_STATE_CONTENT           // 正在解析请求体
};
```

**从状态机（LINE_STATUS）**：控制"一行数据读完了吗"
```cpp
enum LINE_STATUS {
    LINE_OK = 0,   // 完整读到一行
    LINE_BAD,      // 行格式错误
    LINE_OPEN      // 行还没读完（数据不完整）
};
```

**主循环逻辑（`process_read`）：**

```cpp
HTTP_CODE process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 条件: 正在解析 CONTENT 且上一行 OK，或者解析出一行 OK
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK)) {

        text = get_line();  // 获取当前行的起始指针

        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);  // 解析请求行
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            break;

        case CHECK_STATE_HEADER:
            ret = parse_headers(text);  // 解析请求头
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            else if (ret == GET_REQUEST) return do_request();  // 头结束 → 处理
            break;

        case CHECK_STATE_CONTENT:
            ret = parse_content(text);  // 解析请求体
            if (ret == GET_REQUEST) return do_request();  // 体结束 → 处理
            line_status = LINE_OPEN;  // CONTENT 不走 parse_line
            break;
        }
    }
    return NO_REQUEST;  // 数据不完整，需要继续读
}
```

**⚠️ 关键理解 — `while` 的条件：**
- 正常情况下：`parse_line()` 返回 `LINE_OK` → 进入循环 → 根据状态处理这一行
- CONTENT 状态下：请求体不是按行分割的，跳过 `parse_line`，直接用 `m_read_idx` 判断是否读完

### 4. HTTP_CODE 的返回码含义

```cpp
enum HTTP_CODE {
    NO_REQUEST,        // 请求不完整，需要继续读数据
    GET_REQUEST,       // 请求完整，可以开始处理
    BAD_REQUEST,       // 请求格式错误 → 400
    NO_RESOURCE,       // 请求的文件不存在 → 404
    FORBIDDEN_REQUEST, // 权限不足 → 403
    FILE_REQUEST,      // 文件请求成功 → 200
    INTERNAL_ERROR,    // 服务器内部错误 → 500
    CLOSED_CONNECTION  // 客户端关闭连接
};
```

### 5. mmap + writev：零拷贝发送文件

传统方式发送文件（两次拷贝）：
```
磁盘 → 内核缓冲区 → read() → 用户缓冲区 → write() → socket 缓冲区 → 网卡
                      ↑ 拷贝1               ↑ 拷贝2
```

`mmap` + `writev` 的方式（减少一次拷贝）：
```
磁盘 → 内核缓冲区(page cache) → mmap → 用户地址空间直接映射
                    ↓
              writev → socket 缓冲区 → 网卡
                    ↑ 只有一次拷贝（内核内）
```

**代码体现：**

```cpp
// 1. mmap 将文件映射到内存
int fd = open(m_real_file, O_RDONLY);
m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ,
                              MAP_PRIVATE, fd, 0);
close(fd);  // mmap 后可以立即关闭 fd，映射仍然有效

// 2. 设置 iovec 数组：两块不连续内存
m_iv[0].iov_base = m_write_buf;          // 响应头（在栈/堆上）
m_iv[0].iov_len  = m_write_idx;

m_iv[1].iov_base = m_file_address;       // 文件内容（在 mmap 区域）
m_iv[1].iov_len  = m_file_stat.st_size;

m_iv_count = 2;

// 3. writev 一次系统调用发送两块内存
bytes_to_send = m_write_idx + m_file_stat.st_size;
writev(m_sockfd, m_iv, 2);
```

**`writev` 的优势：** 一次系统调用发送多块不连续内存，避免了"先发响应头，再发文件"的两次 `write` 调用。减少了系统调用次数和可能的 Nagle 算法延迟。

---

## 实现指南

### Step 1：http_conn 类设计

```cpp
class http_conn {
public:
    // === 常量 ===
    static const int FILENAME_LEN = 200;      // 文件路径最大长度
    static const int READ_BUFFER_SIZE = 2048;  // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 1024; // 写缓冲区大小

    // === 枚举：HTTP 方法、状态机状态、响应码、行状态 ===
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, ... };
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, ... };
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    // === 公共接口 ===
    void init(int sockfd, const sockaddr_in &addr, ...);  // 初始化连接
    void close_conn(bool real_close = true);               // 关闭连接
    void process();              // 主处理入口（读取 + 解析 + 响应）
    bool read_once();            // 从 socket 读数据（LT/ET 两种模式）
    bool write();                // 发送响应（writev）

    // === 静态成员 ===
    static int m_epollfd;        // 所有连接共享一个 epoll 实例
    static int m_user_count;     // 当前活跃连接数

    // === 同步标志 ===
    int timer_flag;              // 是否需要关闭（定时器标记）
    int improv;                  // 是否处理完成（改善标记）
    int m_state;                 // 0=读, 1=写（Reactor 模式用）

private:
    // 解析函数（三层状态机）
    HTTP_CODE process_read();                  // 主状态机
    HTTP_CODE parse_request_line(char* text);  // 解析请求行
    HTTP_CODE parse_headers(char* text);       // 解析请求头
    HTTP_CODE parse_content(char* text);       // 解析请求体
    HTTP_CODE do_request();                    // 处理请求（核心业务逻辑）

    // 响应构造
    bool process_write(HTTP_CODE ret);
    bool add_response(const char* format, ...);  // [语法] 可变参数
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content(const char* content);

    // 缓冲区管理
    LINE_STATUS parse_line();   // 从状态机：解析一行（处理 \r\n）
    char* get_line();           // 获取当前行指针
    void unmap();               // 释放 mmap 映射

    // === 核心成员变量 ===
    int m_sockfd;                          // 当前连接的 socket fd
    char m_read_buf[READ_BUFFER_SIZE];     // 读缓冲区
    long m_read_idx;                       // 已读数据的位置
    long m_checked_idx;                    // 已解析数据的位置
    int m_start_line;                      // 当前解析行的起始位置

    char m_write_buf[WRITE_BUFFER_SIZE];   // 写缓冲区
    int m_write_idx;                       // 已写入的位置

    CHECK_STATE m_check_state;  // 主状态机状态
    METHOD m_method;            // 请求方法（GET/POST）
    char m_real_file[FILENAME_LEN];  // 请求文件的真实路径
    char* m_url;                // 请求的 URL
    char* m_host;               // Host 头的值
    long m_content_length;      // Content-Length 的值
    bool m_linger;              // 是否 keep-alive

    char* m_file_address;       // mmap 映射的文件内容地址
    struct stat m_file_stat;    // 文件状态（大小、权限等）
    struct iovec m_iv[2];       // writev 的 iovec 数组
    int m_iv_count;             // iovec 数组有效长度（1 或 2）

    char* m_string;             // POST 请求体（用户名和密码）
    int cgi;                    // 是否是 CGI 请求（POST 登录/注册）
};
```

### Step 2：parse_line — 从状态机（最关键的基础函数）

```cpp
LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];

        if (temp == '\r') {
            // 情况1: \r 是最后一个字符 → 行还没读完
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;

            // 情况2: \r\n → 完整的一行！替换为 \0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';    // \r → \0
                m_read_buf[m_checked_idx++] = '\0';    // \n → \0
                return LINE_OK;
            }

            // 情况3: \r 后不是 \n → 格式错误
            return LINE_BAD;
        }
        else if (temp == '\n') {
            // 情况4: 单独的 \n（前一个字符是 \r）
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;  // 还没找到 \r\n，数据不完整
}
```

**⚠️ 为什么要替换为 `\0\0`？**
- C 字符串以 `\0` 结尾。把 `\r\n` 替换为 `\0\0` 后，`get_line()` 返回的指针就自动指向一个以 `\0` 结尾的 C 字符串
- 可以直接用 `strcasecmp`、`strpbrk` 等 C 字符串函数解析

### Step 3：read_once — LT vs ET 的读策略

```cpp
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE) return false;

    // --- LT 模式：读一次即可 ---
    if (0 == m_TRIGMode) {
        int bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                              READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) return false;
        return true;
    }
    // --- ET 模式：必须循环读到 EAGAIN ---
    else {
        while (true) {
            int bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                                  READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;      // 读完了
                return false;   // 真正的错误
            }
            else if (bytes_read == 0) {
                return false;   // 对端关闭
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}
```

### Step 4：write — 非阻塞写（处理短写）

```cpp
bool http_conn::write() {
    int temp = 0;

    // 所有数据已发送完
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();  // 重置状态，准备处理下一个请求
        return true;
    }

    while (1) {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) {
            // 发送缓冲区满 → 等待下次 EPOLLOUT
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 调整 iovec：第一部分（响应头）已发完
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 第一部分还没发完，调整起始位置
        else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 全部发完
        if (bytes_to_send <= 0) {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger) {
                init();     // keep-alive：重置连接，继续处理
                return true;
            } else {
                return false;  // 关闭连接
            }
        }
    }
}
```

**⚠️ 为什么要分段调整 iovec？**
- `writev` 不保证一次发完所有数据（非阻塞 socket）
- 已发送的字节可能落在 iov[0]（响应头）和 iov[1]（文件）的中间
- 每次 `writev` 返回后需要根据已发送字节数调整 iovec 数组的基址和长度

---

## 验证用例与预期结果

### 测试 1：验证 HTTP 请求解析（curl）

```bash
curl -v http://127.0.0.1:9006/
```

**预期响应头：**
```
HTTP/1.1 200 OK
Content-Length: xxx
Connection: close
```

### 测试 2：验证 404

```bash
curl -v http://127.0.0.1:9006/nonexistent.html
```

**预期：** `HTTP/1.1 404 Not Found`

### 测试 3：验证 POST 请求

```bash
curl -v -X POST -d "user=test&passwd=123" http://127.0.0.1:9006/2CGISQL.cgi
```

**预期：** `HTTP/1.1 200 OK`

---

## 踩坑记录

1. **ET 模式必须循环读** — 这是最常犯的错误。ET 下只读一次 `recv` 会导致剩余数据永远丢失。
2. **`writev` 的短写处理** — `writev` 返回的已发送字节数可能小于总长度。必须调整 iovec 指针。
3. **`mmap` 后要 `munmap`** — C++ 没有 GC。忘记 `munmap` 会导致内存泄漏。
4. **`\r\n` 解析的边界条件** — `\r` 是最后一个字符时返回 LINE_OPEN（等更多数据），不是 LINE_BAD。

---

## C++ 语法速查

| 语法 | 示例 | 说明 |
|------|------|------|
| `enum` | `enum METHOD { GET = 0, POST }` | 枚举类型 |
| 可变参数 | `bool add_response(const char* format, ...)` | C 风格可变参数（va_list/va_start/va_end） |
| `mmap` | `mmap(0, size, PROT_READ, MAP_PRIVATE, fd, 0)` | 文件映射到内存 |
| `writev` | `writev(fd, iov, count)` | 聚集写：一次发送多块不连续内存 |
| `struct iovec` | `struct iovec { void* iov_base; size_t iov_len; }` | writev 的数据结构 |
| `stat` | `stat(path, &st)` | 获取文件信息（大小、权限等） |

---

## 阶段小结

你实现了 `http_conn` — 整个项目最复杂的模块：

- ✅ 双模式读数据（LT/ET）
- ✅ 两层状态机解析 HTTP（主状态机 + 从状态机）
- ✅ GET 静态文件 + POST 表单处理
- ✅ mmap + writev 高效发送
- ✅ keep-alive 支持

下一阶段：**Phase 7 — 非阻塞 Socket + epoll**。
