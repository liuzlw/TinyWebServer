# Phase 6 —— HTTP 解析器

## 目标

实现一个 HTTP 连接处理类，能：
1. 从 socket 读取原始 HTTP 请求
2. 用有限状态机解析请求行、请求头、请求体
3. 构造正确的 HTTP 响应（状态行 + 头 + Body）
4. 用 `mmap` + `writev` 高效发送静态文件

**可见结果：** 给定一个原始 HTTP 请求字符串（如 `"GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n"`），程序输出正确的 HTTP 响应头和文件内容。

---

## 前置知识

- HTTP/1.1 协议的基本格式
- C 字符串操作（`strpbrk`、`strcasecmp`、`strspn`）
- `mmap` 的零拷贝概念

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | `CMAKE_BUILD_TYPE` — Debug 和 Release 的区别 |

---

## 分步实现

### Step 1：HTTP 请求格式回顾

一个典型的 HTTP GET 请求：

```
GET /index.html HTTP/1.1\r\n         ← 请求行
Host: localhost:9006\r\n             ← 请求头
Connection: keep-alive\r\n
\r\n                                 ← 空行（头结束）
```

一个 POST 请求（登录/注册）：

```
POST /2 CGI HTTP/1.1\r\n
Host: localhost:9006\r\n
Content-Length: 24\r\n
\r\n
user=admin&passwd=123456            ← 请求体
```

### Step 2：状态机设计

```
  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐
  │ CHECK_STATE  │────→│ CHECK_STATE  │────→│ CHECK_STATE  │
  │ _REQUESTLINE │     │ _HEADER      │     │ _CONTENT     │
  └──────────────┘     └──────────────┘     └──────────────┘
   解析请求方法、       解析 Host、         解析 POST body
   URL、版本号         Content-Length、     （用户名/密码）
                      Connection
```

对应的枚举：

```cpp
enum CHECK_STATE {
    CHECK_STATE_REQUESTLINE = 0,
    CHECK_STATE_HEADER,
    CHECK_STATE_CONTENT
};

enum HTTP_CODE {
    NO_REQUEST,
    GET_REQUEST,
    BAD_REQUEST,
    NO_RESOURCE,
    FORBIDDEN_REQUEST,
    FILE_REQUEST,
    INTERNAL_ERROR,
    CLOSED_CONNECTION
};

enum LINE_STATUS {
    LINE_OK = 0,
    LINE_BAD,
    LINE_OPEN
};
```

### Step 3：`http_conn` 类声明

```cpp
// http_conn.h
class http_conn {
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;

    enum METHOD { GET = 0, POST, HEAD };

    void init(int sockfd, const sockaddr_in& addr, char* root,
              int TRIGMode, int close_log,
              std::string user, std::string passwd, std::string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();            // 从 socket 读取数据
    bool write();                // 向 socket 发送响应

    sockaddr_in* get_address() { return &m_address; }
    void initmysql_result(connection_pool* connPool);

    int timer_flag;              // 标记是否需要清理定时器
    int improv;                  // 标记任务是否处理完成
    int m_state;                 // 0=读, 1=写（Reactor 模式用）

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL* mysql;

private:
    // --- 解析 ---
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);

    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    LINE_STATUS parse_line();

    // --- 响应构造 ---
    bool add_response(const char* format, ...);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_content_type();
    bool add_linger();
    bool add_blank_line();
    bool add_content(const char* content);

    void unmap();
    void init();  // 重置内部状态

    // --- 成员变量 ---
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];   // 读缓冲区
    long m_read_idx;                      // 已读入的字节数
    long m_checked_idx;                   // 已解析到的位置
    int m_start_line;                     // 当前解析行的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int m_write_idx;

    CHECK_STATE m_check_state;   // 当前状态机状态
    METHOD m_method;             // GET / POST
    char m_real_file[FILENAME_LEN];
    char* m_url;
    char* m_version;
    char* m_host;
    long m_content_length;
    bool m_linger;               // keep-alive
    char* m_file_address;        // mmap 映射的文件地址
    struct stat m_file_stat;
    struct iovec m_iv[2];        // writev 用的 iovec 数组
    int m_iv_count;
    int cgi;                     // 是否启用 CGI（=1 表示 POST）
    char* m_string;              // POST body
    int bytes_to_send;
    int bytes_have_send;
    char* doc_root;

    std::map<std::string, std::string> m_users;  // 用户名→密码
    int m_TRIGMode;
    int m_close_log;
};
```

### Step 4：读取 socket 数据

```cpp
// LT 模式：读一次
// ET 模式：循环读到 EAGAIN
bool http_conn::read_once() {
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;

    if (0 == m_TRIGMode) {
        // --- LT 模式 ---
        int bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                              READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) return false;
        return true;
    } else {
        // --- ET 模式：必须读到返回 EAGAIN 为止 ---
        while (true) {
            int bytes_read = recv(m_sockfd, m_read_buf + m_read_idx,
                                  READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            } else if (bytes_read == 0) {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}
```

### Step 5：按行解析 + 状态机主循环

```cpp
LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if (m_checked_idx + 1 == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        } else if (temp == '\n') {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;  // 还没收到完整的一行
}

HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
           || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;

        switch (m_check_state) {
        case CHECK_STATE_REQUESTLINE:
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            break;
        case CHECK_STATE_HEADER:
            ret = parse_headers(text);
            if (ret == BAD_REQUEST) return BAD_REQUEST;
            else if (ret == GET_REQUEST) return do_request();
            break;
        case CHECK_STATE_CONTENT:
            ret = parse_content(text);
            if (ret == GET_REQUEST) return do_request();
            line_status = LINE_OPEN;
            break;
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;  // 数据不完整，等下次再读
}
```

**关键点：** `m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK` ——当解析到请求体时，不再按行切分（`parse_line`），因为 POST body 没有 `\r\n` 行分隔。

### Step 6：请求行和请求头解析

```cpp
HTTP_CODE http_conn::parse_request_line(char* text) {
    // "GET /index.html HTTP/1.1"
    m_url = strpbrk(text, " \t");  // 找第一个空格
    if (!m_url) return BAD_REQUEST;
    *m_url++ = '\0';

    // method = "GET"
    char* method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        cgi = 1;
    } else
        return BAD_REQUEST;

    // 跳过空格/tab
    m_url += strspn(m_url, " \t");

    // version = "HTTP/1.1"
    m_version = strpbrk(m_url, " \t");
    if (!m_version) return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;

    // 处理 http:// 前缀
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    if (strlen(m_url) == 1)           // url 是 "/"
        strcat(m_url, "judge.html");  // 默认首页

    m_check_state = CHECK_STATE_HEADER;  // 状态切换
    return NO_REQUEST;
}

HTTP_CODE http_conn::parse_headers(char* text) {
    if (text[0] == '\0') {
        // 空行 → 请求头结束
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;  // GET 请求：头结束 = 请求完整
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
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
    return NO_REQUEST;
}
```

### Step 7：`do_request` —— URL 路由与登录/注册

```cpp
HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char* p = strrchr(m_url, '/');

    // --- POST 注册/登录 ---
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) {
        char name[100] = {0}, password[100] = {0};

        // 解析 POST body："user=xxx&passwd=yyy"
        // 用通用 key-value 方式解析，而不是硬编码偏移量
        char *cur = m_string, *next;
        while (cur && *cur) {
            // 跳过开头的空白
            while (*cur == ' ' || *cur == '\t') cur++;

            // 找下一个 & 或结束
            next = strchr(cur, '&');
            if (!next) next = cur + strlen(cur);

            // 找 = 号分隔 key 和 value
            char* eq = strchr(cur, '=');
            if (eq && eq < next) {
                size_t key_len = eq - cur;
                size_t val_len = next - eq - 1;

                if (key_len >= 100 || val_len >= 100) {
                    // 安全保护：长度超限跳过（生产环境应返回 BAD_REQUEST）
                    cur = (*next) ? next + 1 : NULL;
                    continue;
                }

                if (strncmp(cur, "user", key_len) == 0) {
                    strncpy(name, eq + 1, val_len);
                    name[val_len] = '\0';
                } else if (strncmp(cur, "passwd", key_len) == 0) {
                    strncpy(password, eq + 1, val_len);
                    password[val_len] = '\0';
                }
            }
            cur = (*next) ? next + 1 : NULL;
        }

        // 注意：生产环境中应对 name/password 做 URL 解码（把 %XX 转成原始字符）
        // 以及用 mysql_real_escape_string 防止 SQL 注入

        if (*(p + 1) == '3') {
            // 注册
            if (m_users.find(name) == m_users.end()) {
                // 插入到 MySQL（略，见原项目）
                strcpy(m_url, "/log.html");
            } else {
                strcpy(m_url, "/registerError.html");
            }
        } else if (*(p + 1) == '2') {
            // 登录
            if (m_users.find(name) != m_users.end()
                && m_users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    // --- URL 路由：特定数字 → 特定页面 ---
    if (*(p + 1) == '0')      strcpy(m_url, "/register.html");
    else if (*(p + 1) == '1') strcpy(m_url, "/log.html");
    else if (*(p + 1) == '5') strcpy(m_url, "/picture.html");
    else if (*(p + 1) == '6') strcpy(m_url, "/video.html");

    // 拼接完整路径：doc_root + url
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    // 检查文件是否存在、是否有读权限
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    // mmap 映射文件到内存（零拷贝）
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size,
                                  PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
```

### Step 8：HTTP 响应构造

```cpp
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
    case INTERNAL_ERROR:
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        add_content(error_500_form);
        break;
    case BAD_REQUEST:
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        add_content(error_404_form);
        break;
    case FILE_REQUEST:
        add_status_line(200, ok_200_title);
        add_headers(m_file_stat.st_size);
        // 使用 writev 聚集写：头部 + 文件内容一次发送
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len  = m_write_idx;
        m_iv[1].iov_base = m_file_address;
        m_iv[1].iov_len  = m_file_stat.st_size;
        m_iv_count = 2;
        bytes_to_send = m_write_idx + m_file_stat.st_size;
        return true;
    // ...
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len  = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 辅助方法
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("HTTP/1.1 %d %s\r\n", status, title);
}
bool http_conn::add_headers(int content_len) {
    return add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}
bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n",
                        m_linger ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}
```

### Step 9：`write` —— 用 writev 发送响应

传统的 `write` 需要把响应头和文件内容拷贝到一个大 buffer。`writev` 可以一次性发送多个不连续的缓冲区：

```cpp
bool http_conn::write() {
    if (bytes_to_send == 0) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();  // 重置状态，准备处理下一个请求
        return true;
    }

    while (1) {
        int temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;  // 缓冲区满，等下次 EPOLLOUT
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 更新 iovec：调整已发送部分
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0) {
            unmap();  // 解除 mmap 映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
            if (m_linger) {
                init();  // keep-alive：重置连接，等待下一个请求
                return true;
            }
            return false;  // close：关闭连接
        }
    }
}
```

### Step 10：`process` —— 解析+响应

```cpp
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;  // 数据不完整，继续等
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret) close_conn();
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
```

### Step 11：Debug / Release 构建

```cmake
# CMakeLists.txt
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall -Wextra")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Debug 构建：包含调试符号，未优化
cmake -B build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build_debug

# Release 构建：优化性能，去掉断言
cmake -B build_release -DCMAKE_BUILD_TYPE=Release
cmake --build build_release
```

在 Phase 9 的压测中，你会看到 Release 和 Debug 的性能差异可以达到 5-10 倍。

---

## 验证方法

- [ ] 用 curl 发送 GET 请求 → 返回正确的 HTML 内容和状态码 200
- [ ] 用 curl 发送 POST 请求 → 注册/登录逻辑正确
- [ ] 请求不存在的文件 → 返回 404
- [ ] 用字符串直接调用 `process_read` 测试状态机

```bash
# 快速测试
curl -v http://127.0.0.1:9006/
curl -v http://127.0.0.1:9006/xxx.html    # 应返回 404
```

---

## 踩坑记录

1. **`\r\n` vs `\n`。** HTTP 标准要求 `\r\n`，但有些客户端只发 `\n`。`parse_line` 两种都兼容处理。

2. **读缓冲区溢出。** `m_read_idx >= READ_BUFFER_SIZE` 时直接返回 false 断开连接。生产环境可以更优雅——返回 413 Payload Too Large。

3. **`mmap` 后不要忘记 `munmap`。** 每次 `write` 完成后调用 `unmap()`，否则内存泄漏。

4. **`STATIC` 静态成员初始化。** `m_epollfd` 和 `m_user_count` 是静态成员，必须在 `.cpp` 文件中单独定义：

   ```cpp
   int http_conn::m_epollfd = -1;
   int http_conn::m_user_count = 0;
   ```

5. **POST body 解析不要硬编码偏移量。** 用 `user=xxx&passwd=yyy` 格式时，不要假设 key 的长度固定（如 `for (i = 5; ...)`）。应当用 `strchr('=')` + `strncmp` 做通用 key-value 解析（本教程已采用改进版本）。另外，生产环境还需做 URL 解码（`%XX` → 原始字符）。

6. **`Content-Length` 缺失时怎么办。** HTTP/1.1 还支持 `Transfer-Encoding: chunked`（分块传输），本教程未实现。如果收到没有 `Content-Length` 的 POST 请求，`m_content_length` 默认为 0，解析会直接进入 GET 分支。生产环境需要处理 `chunked` 编码。

7. **不支持 HTTP Pipelining。** HTTP/1.1 keep-alive 允许在同一个连接上连续发送多个请求而不等响应（Pipelining）。本项目的 keep-alive 实现是串行的——必须等当前请求的响应完成后才能处理下一个请求。这是简化设计，生产级服务器需要支持请求队列和乱序响应。

8. **路径穿越风险。** `do_request` 中把 URL 直接拼到文件路径上：
   ```cpp
   strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
   ```
   恶意请求 `GET /../../../etc/passwd HTTP/1.1` 可能读到系统文件。**防护方法：** 在拼接前检查 `m_url` 中是否包含 `..`，或使用 `realpath()` 规范化路径后验证前缀。

---

## 阶段小结

你完成了整个项目中最复杂的模块——HTTP 解析器：

- 有限状态机（请求行→请求头→请求体）
- LT / ET 两种读模式
- `mmap` + `writev` 高效文件传输
- GET / POST 请求路由、注册/登录逻辑

下一阶段：**非阻塞 Socket + epoll**——把 HTTP 解析和网络收发对接起来。
