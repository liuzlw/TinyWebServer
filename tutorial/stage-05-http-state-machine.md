# Stage 5 完整 HTTP 状态机

> 🔬 **项目最值得反复看的部分**。用状态机彻底解决"粘包/半包"问题,用 `writev` 高效发送大文件。`http_conn` 是原项目的核心类。

## 1. 本阶段目标

- [ ] 理解主状态机 + 从状态机的设计
- [ ] 看懂 `parse_line`(从状态机)、`process_read`(主状态机)
- [ ] 理解 `mmap` 映射文件 + `writev` 一次性发送头部和文件
- [ ] 理解 `EPOLLONESHOT` 的作用
- [ ] 服务器支持 GET/POST、keep-alive、大文件

**最终效果:** 在浏览器里访问 `/welcome.html`、`/0`(注册页)、`/1`(登录页)、`/5`(图片页),6MB 大图也能完整显示。

## 2. 前置知识

- C2:`std::string`、类;S4:epoll/LT/ET
- 新增:状态机、内存映射 `mmap`、聚合写 `writev`

## 3. 问题:S4 的 echo 为什么还不够?

S4 的服务器只会"原样回显",而 HTTP 服务器要:
1. **解析请求**:从字节流里拆出方法、路径、头部、body
2. **解决粘包/半包**:TCP 是流,一次 `read` 可能只读到请求的一半(半包),也可能一次读到多个请求(粘包)

**朴素做法(比如 S2)**:假设一次 read 就是完整请求——但网络环境下这个假设是错的。

**状态机的解法**:不假设数据一次到齐。**每次 read 到什么就解析什么,数据不完整就"挂起",等更多数据再来继续**。这正是状态机存在的意义。

## 4. 状态机设计

`http_conn` 用了**两层状态机**:

### 主状态机:`m_check_state` —— 现在在解析哪一部分

```text
CHECK_STATE_REQUESTLINE  →  CHECK_STATE_HEADER  →  CHECK_STATE_CONTENT
     (请求行)                   (请求头)               (请求体,POST才有)
```

- 请求行解析完 → 跳到 HEADER
- 头部解析完,发现没有 body(`Content-Length: 0`)→ 结束,得到完整请求
- 头部解析完,发现**有** body → 跳到 CONTENT,等 body 到齐

### 从状态机:`parse_line` —— 当前行读完整了没有

它扫描缓冲区,找一行(`\r\n` 结尾)的边界:

```text
返回 LINE_OK     → 这一行完整了,可以交给主状态机解析
返回 LINE_OPEN   → 这一行还没读完(半包!),主状态机先别动
返回 LINE_BAD    → 行格式错误(没有 \r\n)
```

**两层配合的过程**(以 GET 请求为例):

```text
read 到一段数据 "GET /welcome.html HTTP/1.1\r\nHost: 1..."
                       ↓ parse_line 找到第一个 \r\n → LINE_OK
主状态机在 REQUESTLINE → parse_request_line("GET /welcome.html HTTP/1.1")
                       ↓ 解析完,切到 HEADER
                       ↓ parse_line 找下一行 ...
主状态机在 HEADER → parse_headers("Host: ...")
                    ...
主状态机遇到空行 → 请求解析完成 → do_request() 返回文件
```

**半包场景:** 如果 read 到的数据只有 `"GET /welcome.html HTTP"`(没有 `\r\n`),`parse_line` 返回 `LINE_OPEN`,`process_read` 的 while 循环退出,返回 `NO_REQUEST`——服务器**不着急**,等下一轮 read 到更多数据再继续。这就是"状态机天然处理半包"。

## 5. http_conn.h:连接类定义

在 `my_tiny_webserver/` 下新建 `http/http_conn.h`(**S5 简化版**:去掉了原项目的 MySQL/定时器/日志依赖,差异见第 12 节):

```cpp
#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <string>

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH };
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    enum HTTP_CODE
    {
        NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE,
        FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION
    };
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

public:
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
    char *get_line() { return m_read_buf + m_start_line; };
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
    static int m_epollfd;      // 所有 http_conn 共享同一个 epoll 实例
    static int m_user_count;   // 当前连接总数

private:
    int m_sockfd;                      // 连接的文件描述符
    sockaddr_in m_address;             // 客户端地址
    char m_read_buf[READ_BUFFER_SIZE]; // 读缓冲区(存请求)
    long m_read_idx;                   // 已读数据末尾下标
    long m_checked_idx;                // 已解析数据末尾下标
    int m_start_line;                  // 当前解析行的起点
    char m_write_buf[WRITE_BUFFER_SIZE]; // 写缓冲区(存响应头)
    int m_write_idx;                   // 已写内容末尾下标
    CHECK_STATE m_check_state;         // 主状态机
    METHOD m_method;                   // 请求方法
    char m_real_file[FILENAME_LEN];    // 映射文件的真实路径
    char *m_url;                       // 请求的 URL
    char *m_version;                   // HTTP 版本
    char *m_host;                      // 请求的 Host
    long m_content_length;             // POST 的 body 长度
    bool m_linger;                     // 是否 keep-alive
    char *m_file_address;              // mmap 映射出的文件地址
    struct stat m_file_stat;           // 文件信息(大小等)
    struct iovec m_iv[2];              // writev 的散列数组(头 + 文件)
    int m_iv_count;                    // 用了几块
    int cgi;                           // 是否 POST
    char *m_string;                    // POST body 数据
    int bytes_to_send;                 // 还差多少字节没发
    int bytes_have_send;               // 已发送多少字节
    char *doc_root;                    // 网站根目录
    int m_TRIGMode;                    // LT(0) / ET(1)
};

#endif
```

**核心成员速记:**

| 成员 | 一句话 |
|---|---|
| `m_read_buf[m_read_idx][m_checked_idx][m_start_line]` | 读缓冲的三个"指针":已读 / 已解析 / 当前行起点 |
| `m_check_state` | 主状态机:请求行→头→内容 |
| `m_iv[2]` + `m_iv_count` | writev 要发的东西:`[0]`=响应头,`[1]`=文件内容 |
| `m_linger` | 请求头里 `Connection: keep-alive` 则 true,响应后保持连接 |
| `bytes_to_send` / `bytes_have_send` | 大文件分次发送的进度 |

## 6. http_conn.cpp:实现

在 `my_tiny_webserver/http/` 下新建 `http_conn.cpp`(**S5 简化版**):

```cpp
#include "http_conn.h"

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
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

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
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

//关闭连接，关闭一个连接，客户总量减一
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

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode)
{
    m_sockfd = sockfd;
    m_address = addr;

    doc_root = root;
    m_TRIGMode = TRIGMode;              // 先赋值,再拿去注册(见下方注释)

    init();
    addfd(m_epollfd, sockfd, true, m_TRIGMode);   // 注册进 epoll
    m_user_count++;
}
// 注意:把 addfd 挪到 m_TRIGMode 赋值之后——原版顺序是反的(addfd 在赋值之前,
// 用的是未初始化的成员,垃圾值不是 1 时 ET 连接会被错按 LT 注册),这里修正了。

//初始化新接受的连接
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
    cgi = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;              // 行尾了但还没见到 \n,可能半包
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';   // 用 \0 替换 \r
                m_read_buf[m_checked_idx++] = '\0';   // 用 \0 替换 \n
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
        return false;
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0)
            return false;
        return true;
    }
    //ET读数据:循环读到 EAGAIN
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;                       // 读完了
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;                    // 对端关闭
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");       // 找第一个空格,把"GET"和"/path"分开
    if (!m_url)
        return BAD_REQUEST;
    *m_url++ = '\0';                    // 空格处放 \0,text 就成了 "GET"
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");  // 再找第二个空格,得到路径
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)   // 处理完整 URL 形式
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    if (strlen(m_url) == 1)             // URL 是 "/",给默认首页
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER; // 下一阶段:解析请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')                // 空行 = 头部结束
    {
        if (m_content_length != 0)      // 有 body,进入 CONTENT 状态
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;             // 没有 body,请求解析完成
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
            m_linger = true;
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
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';  // 把 body 结尾封口
        m_string = text;                // body 内容(注册/登录时用)
        return GET_REQUEST;
    }
    return NO_REQUEST;                  // body 还没到齐,继续等
}

//主状态机:驱动整个解析流程
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();              // 取当前行
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
                return do_request();    // 请求完整,去映射文件
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;    // body 没到齐,挂起
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;                  // 半包,等更多数据
}

//把 URL 映射到真实文件路径
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');

    // S5 简化版:原项目在这里有 cgi 注册/登录的数据库操作,Stage 8 补上
    // 这里只保留 URL → 静态文件的映射
    if (*(p + 1) == '0')                // /0 → 注册页
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')           // /1 → 登录页
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5')           // /5 → 图片页
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')           // /6 → 视频页
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '7')           // /7 → 粉丝页
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)      // 文件不存在
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))         // 没读权限
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))             // 是目录
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

//发送响应(用 writev 一次发"头 + 文件")
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)             // 没东西要发(keep-alive 后的空转)
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
            if (errno == EAGAIN)        // 缓冲区满,等下次可写再发
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
            // 头发完了,开始发文件部分
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            // 头还没发完,继续发头的剩余部分
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)         // 全部发完
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
            if (m_linger)
            {
                init();                 // keep-alive:清空状态,继续服务下一个请求
                return true;
            }
            else
                return false;           // 短连接:通知主循环关闭
        }
    }
}

//以下是拼响应头的辅助函数,全部往 m_write_buf 里写
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
    return add_content_length(content_len) && add_linger() && add_blank_line();
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

//根据解析结果拼出响应
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
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;      // 第一块:响应头
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;   // 第二块:文件内容
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

//对外入口:解析 → 拼响应 → 注册可写事件
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)         // 半包,继续等读事件
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
        close_conn();
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);   // 准备好响应,等可写
}
```

> 💡 **`EPOLLONESHOT` 是干什么的?**
>
> **作用**:一个 fd 触发一次事件被处理后,内核就把它从就绪队列里"摘下来",在重新注册(EPOLL_CTL_MOD)之前**不再上报该 fd 的任何事件**。
>
> **为什么连接 fd 需要它?** 防止**多个线程同时处理同一个连接**。设想 S3 的线程池场景:某个连接同时可读又可写(EPOLLIN + EPOLLOUT 都就绪),`epoll_wait` 返回后如果没有 ONESHOT,可能有两个工作线程先后被同一个 fd 唤醒,各自去 read/write 同一个连接 → 数据错乱、状态机打架。加了 ONESHOT,第一个拿到该 fd 的线程处理完、用 `modfd` 重新注册后,别人才可能再碰它——**一个 fd 一次只归一个线程处理**。
>
> **注意**:监听 socket 与信号用的 `pipefd` **不开启** ONESHOT(它们由主线程独占,不需要防并发);只有会被工作线程轮流转的连接 fd 才开。
>
> 本阶段服务器还是单线程,ONESHOT 的作用要到 S9 把线程池装回来才真正体现,但代码从现在就按"线程池安全"的写法准备了。

> 💡 **注意首页变了**:S2 的默认首页是 `welcome.html`;这里照抄原版后,访问 `/` 会跳 `judge.html`(一个判断"你是新用户还是老用户"的引导页)。这是原项目行为,不是 bug。

### 三个核心函数走读(必看)

**① `parse_line`——把字节流切成"一行"**

它做的事:从 `m_checked_idx` 扫到 `m_read_idx`,找一行的结束符 `\r\n`,找到就把 `\r` 和 `\n` 都替换成 `\0`,返回 `LINE_OK`。替换成 `\0` 很关键:这样每一行就成了一个以 `\0` 结尾的 C 字符串,后面 `parse_request_line(text)` 拿 `text` 直接用 `strpbrk/strspn` 等字符串函数就能处理。

四种返回:

| 返回值 | 含义 | 场景 |
|---|---|---|
| `LINE_OK` | 完整找到一行,`\r\n` 已变 `\0` | 正常 |
| `LINE_OPEN` | 数据不够成行(行尾了还没看到 `\n`,或扫完都没找到 `\r\n`) | **半包**,等下一轮 read |
| `LINE_BAD` | 格式错误(裸 `\n` 前面不是 `\r`) | 非法请求 |

**② `process_read`——主状态机循环**

```cpp
while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK)
       || ((line_status = parse_line()) == LINE_OK))
```

循环条件解释了为什么叫"两层状态机":普通请求头按"行"解析,所以靠 `parse_line()` 返回 `LINE_OK` 继续;但 POST 的 **body 不是按行切的**,所以到了 `CHECK_STATE_CONTENT` 阶段改看 `line_status`(由 `parse_content` 自己决定 body 是否读完)。

循环体按 `m_check_state` 分派:

- `CHECK_STATE_REQUESTLINE` → `parse_request_line(text)`。解析完把状态切到 `CHECK_STATE_HEADER`。
- `CHECK_STATE_HEADER` → `parse_headers(text)`。`\r\n` 空行表示头部结束;若 `ret == GET_REQUEST` 说明请求完整,调 `do_request()` 把 URL 映射成文件。
- `CHECK_STATE_CONTENT` → `parse_content(text)`(只处理 POST body)。body 读完返回 `GET_REQUEST` → `do_request()`;没读完把 `line_status = LINE_OPEN`,整个 while 退出,返回 `NO_REQUEST` 挂起。

**③ `write`——writev 发送 + 字节记账**

`writev` 一次发两块:头(`m_iv[0]`,在 `m_write_buf` 里)+ 文件(`m_iv[1]`,mmap 出来的)。难点是**记清楚发到哪了**,因为非阻塞 socket 一次 `writev` 可能只发出去一部分:

1. `bytes_to_send == 0`:keep-alive 空转,`modfd` 改回 `EPOLLIN`、`init()` 复位,等下一个请求。
2. `writev` 返回 `EAGAIN`:发送缓冲满了,改注册 `EPOLLOUT`,等下次可写再继续发(数据不会丢,还在缓冲区里)。
3. `bytes_have_send >= m_iv[0].iov_len`:头已经发完,`m_iv[0].iov_len = 0`,`m_iv[1]` 指向文件里剩下的部分(从 `bytes_have_send - m_write_idx` 起)。这个偏移有点绕:头发完时 `bytes_have_send == m_write_idx`(写缓冲写了多少就发了多少),所以 `m_file_address + 0` 正好是文件开头。
4. `bytes_to_send <= 0`:全部发完。keep-alive → `init()` 继续服务下一请求;否则返回 `false`,主循环关连接。

**配合 gdb 理解**:在 `parse_line` 断点单步,观察 `m_checked_idx/m_read_idx` 怎么前进、`\r\n` 怎么变 `\0`;在 `process_read` 断点,看 `m_check_state` 从 0(REQUESTLINE)走到 2(CONTENT);在 `write` 断点,看 `bytes_have_send/bytes_to_send` 两个计数器如何驱动 `m_iv` 变化。

## 7. main.cpp:把 http_conn 接入 epoll 循环

**替换 `my_tiny_webserver/main.cpp`**:

```cpp
// main.cpp —— epoll + http_conn 状态机(Stage 5)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http/http_conn.h"

const int PORT = 9006;
const int MAX_FD = 65535;             // 最大连接数(受 fd 上限约束)
const int MAX_EVENT_NUMBER = 10000;
const int TRIGMode = 1;               // 0 = LT, 1 = ET

// 连接对象数组:每个 fd 对应一个 http_conn 对象
http_conn *users = new http_conn[MAX_FD];

// http_conn.cpp 里定义的 epoll 辅助函数
extern void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
extern void removefd(int epollfd, int fd);
extern void modfd(int epollfd, int fd, int ev, int TRIGMode);

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

    // epoll 实例,并让 http_conn 类记住它
    int epollfd = epoll_create(5);
    http_conn::m_epollfd = epollfd;
    addfd(epollfd, listenfd, false, TRIGMode);   // 监听 fd 不开启 ONESHOT

    char root[] = "root";              // 静态文件根目录(需在 my_tiny_webserver/ 下运行)
    printf("HTTP 服务器已启动, 监听端口 %d\n", PORT);

    epoll_event events[MAX_EVENT_NUMBER];
    while (true)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) { perror("epoll_wait"); break; }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                // 新连接:ET 模式一次 accept 干净
                while (true)
                {
                    struct sockaddr_in client_address;
                    socklen_t client_addrlength = sizeof(client_address);
                    int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
                    if (connfd < 0) break;
                    if (connfd >= MAX_FD) { close(connfd); continue; }
                    users[connfd].init(connfd, client_address, root, TRIGMode);
                    printf("accept, 当前连接数: %d\n", http_conn::m_user_count);
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();   // 对端关闭或异常
            }
            else if (events[i].events & EPOLLIN)
            {
                // 可读:先读数据,再交给状态机处理
                if (users[sockfd].read_once())
                    users[sockfd].process();   // 解析 + 拼响应,内部把事件改成 EPOLLOUT
                else
                    users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLOUT)
            {
                // 可写:发送响应;keep-alive 时内部把事件改回 EPOLLIN
                if (!users[sockfd].write())
                    users[sockfd].close_conn();
            }
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    return 0;
}
```

**主循环的"事件 → 动作"对应:**

| 事件 | 动作 |
|---|---|
| `listenfd` 可读 | accept,`users[connfd].init(...)`(init 内部把 connfd 注册进 epoll) |
| `EPOLLIN` | `read_once()` → `process()`(解析 + 拼响应,改注册为 EPOLLOUT) |
| `EPOLLOUT` | `write()`(writev 发送;keep-alive 改回 EPOLLIN,否则返回 false → close) |
| `EPOLLRDHUP/HUP/ERR` | `close_conn()` |

## 8. 编译与运行

更新 `CMakeLists.txt`(加上了 `http/http_conn.cpp`):

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp http/http_conn.cpp)
target_link_libraries(server pthread)
```

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

## 9. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `curl -s -o /dev/null -w "%{http_code}" http://127.0.0.1:9006/welcome.html` | `200` | ☐ |
| 2 | `curl http://127.0.0.1:9006/0` 和 `/1` | 返回注册页 / 登录页 HTML | ☐ |
| 3 | `curl -X POST -d "user=alice&passwd=123" http://127.0.0.1:9006/welcome.html` | `200`(证明 body 被状态机完整解析,不挂起) | ☐ |
| 4 | **keep-alive**:`curl` 同一 URL 两次(或 `curl --next` 两个 URL) | 两次都成功,服务器日志显示同一连接处理两次 | ☐ |
| 5 | **6MB 大文件**:`curl -s -o /dev/null -w "%{http_code} %{size_download}" http://127.0.0.1:9006/picture.gif` | `200 6053895` | ☐ |
| 6 | **浏览器**:打开 `/welcome.html`、`/5`(图片页) | 页面正常,图片完整显示 | ☐ |

> ⚠️ **关于缺失文件**:访问 `/nope.html` 时 `curl` 会报 `curl: (52) Empty reply from server`(此时 `%{http_code}` 是 `000`),而不是 404——**这是原项目的行为**,不是 bug。原代码 `process_write` 没有处理 `NO_RESOURCE`(文件不存在),直接关闭连接、不返回响应。想让它返回 404 页面,把 `process_write` 里 `case BAD_REQUEST` 和 `default` 之间的逻辑补上即可,但为了和"参考答案"一致,我们先保留原样。这个"坑"在面试里也常被问。
>
> 注意区分:**52 是 curl 的错误号**(对方关闭连接、没回任何报文),**不是 HTTP 状态码 502**——服务器压根没发状态行,自然没有 502 一说。

## 10. 调试技巧

### gdb 观察状态机流转

```bash
gdb ./build/server
```

```text
(gdb) break http_conn.cpp:360    ← 断在 process_read 的 switch 处(以实际行号为准)
(gdb) run
(gdb) next                        ← 另开终端 curl 访问后,单步
(gdb) print m_check_state
$1 = CHECK_STATE_HEADER           ← 已经推进到"解析头"阶段
(gdb) print m_url
$2 = 0x... "/welcome.html"
```

### 打印 m_read_buf 看原始请求

```text
(gdb) print m_read_idx
(gdb) x/80c m_read_buf           ← 以字符形式看读缓冲前 80 字节
```

## 11. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 大文件(6MB)显示不完整 | `write` 没处理"一次 writev 发不完" | 代码里有 `bytes_have_send`/`bytes_to_send` 循环和 EAGAIN 分支,已处理 |
| 页面反复刷新后连接越来越多不释放 | keep-alive 连接不关闭 | 正常现象;Stage 6 定时器专门清理空闲连接 |
| `curl: (52) Empty reply from server`(http_code 是 000) | 原项目 `process_write` 未处理 `NO_RESOURCE`(文件不存在直接关连接,不回响应) | 见第 9 节说明,是原项目行为 |
| 浏览器显示"连接被重置" | 网站根目录路径不对或响应格式出错 | 确认在 `my_tiny_webserver/` 下运行(有 `root/` 目录) |
| `/0`、`/1`、`/5` 页面打不开(连接被重置) | `root/` 里没有 `register.html`、`picture.gif` 等静态文件 | 确认做过 Stage 2 的 `cp -r ../root/* root/`,`ls root/` 应能看到这些文件 |
| POST 请求挂起不返回 | body 没读全,`parse_content` 一直返回 NO_REQUEST | 检查 `Content-Length` 是否正确传给服务器 |

## 12. 与原项目对照

| 本阶段(S5 简化版) | 原项目 |
|---|---|
| `http_conn.h` / `http_conn.cpp` 主体 | **与 `http/` 目录基本一致** |
| 去掉的部分 | `#include "sql_connection_pool.h"`、`MYSQL *mysql`、`initmysql_result`、`do_request` 的 cgi 注册/登录分支(Stage 8 补) |
| 去掉的部分 | `LOG_INFO/LOG_ERROR` 日志调用(Stage 7 补) |
| 去掉的部分 | `timer_flag`、`improv` 定时器相关字段(Stage 9 换原版时补——Stage 6 的定时器只加到主循环层面,不碰 http_conn 内部) |
| `addfd/modfd/removefd` | 原项目定义在 `http_conn.cpp`(与这里一致) |
| `process()` 里没有线程池 | 原项目由 `threadpool<http_conn>` 的工作线程调用 `process()`(Stage 9 补) |

> **diff 对照**(在原仓库根目录运行,会看到差异集中在"去掉的部分"):
> ```bash
> diff my_tiny_webserver/http/http_conn.h http/http_conn.h
> diff my_tiny_webserver/http/http_conn.cpp http/http_conn.cpp
> ```

**刻意简化的原因**:原版 `http_conn.cpp` 同时用了日志、定时器、MySQL 三样还没学的东西。本阶段先把**状态机核心**吃透;S6/S7/S8 逐个补上;S9 整合时换成原版完整代码,到时候 `diff` 一下,所有差异你都看得懂。

## 13. 下一步

进入 **[Stage 6 定时器](stage-06-timer.md)**——keep-alive 连接挂着一大堆不关,会拖垮服务器。定时器定期清理"非活动连接"。
