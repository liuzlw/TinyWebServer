# Stage 2：单线程 HTTP 静态服务器

> 🎯 **本阶段目标**：把 Stage 1 的 echo 服务器改造成「浏览器能访问」的 HTTP 服务器。
> 在浏览器输入 `http://127.0.0.1:9006/` 能看到网页 —— 这是第一个"有模有样"的里程碑。

## 📚 理论铺垫

### 2.1 HTTP 协议：就是约定好格式的文本

HTTP 是建立在 TCP 之上的**文本协议**。浏览器连上服务器后，发出的原始报文长这样：

```http
GET /index.html HTTP/1.1\r\n
Host: 127.0.0.1:9006\r\n
User-Agent: Mozilla/5.0 ...\r\n
Accept: text/html, ...\r\n
\r\n
```

- 第一行是**请求行**：方法 + URL + 协议版本
- 接下来若干行是**请求头**：键值对
- 一个空行（`\r\n`）标志头部结束
- 空行之后是**消息体**（GET 通常没有，POST 有）

服务器要回：

```http
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 138\r\n
Connection: close\r\n
\r\n
<html>...网页内容...</html>
```

- 第一行是**状态行**：版本 + 状态码 + 短语
- 然后是响应头、空行、响应体（网页内容）

**`\r\n` 是 HTTP 的行分隔符**，不是 `\n`！这一点 Stage 5 的状态机解析会反复用到。

### 2.2 本阶段的朴素思路

```
接受连接 → read 整个请求（先不解析，打印出来看看）
         → 不管请求什么，都返回固定的 index.html 内容
         → 关闭连接
```

然后再升级一步：从请求行里抠出 URL，把 `root/` 目录下对应的文件读出来返回；
文件不存在就返回 404。这就是 TinyWebServer `do_request()` 的最简化版本。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| `std::string` 操作（find、substr、+=） | 解析请求行、拼响应 |
| C 风格字符串函数 `strstr`/`strncpy` | 原始代码风格，了解即可 |
| `snprintf` 格式化输出到 buffer | 拼 HTTP 响应头 |
| `FILE*`/`fopen`/`fread` 或 `open`/`read` | 读取静态文件 |
| `struct stat` | 判断文件是否存在、获取文件大小 |

## 🔨 动手实现

```bash
cd /mnt/c/Users/liuzl/Documents/projects/TinyWebServer/my_tiny_webserver
mkdir -p stage2_http && cd stage2_http
```

### 2.1 准备网页文件

直接复用原始项目的静态资源（里面有 welcome 页、图片、视频）：

```bash
cp -r ../../root ./root
ls root        # 应看到 index.html、welcome.html、log.html 等
```

### 2.2 单线程 HTTP 服务器 `simple_http.cpp`

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <string>

const int PORT = 9006;
const int BUF_SIZE = 4096;
const char* DOC_ROOT = "./root";   // 静态文件根目录

// 根据文件后缀决定 Content-Type
const char* get_content_type(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html";
    if (path.find(".jpg")  != std::string::npos) return "image/jpeg";
    if (path.find(".png")  != std::string::npos) return "image/png";
    if (path.find(".mp4")  != std::string::npos) return "video/mp4";
    return "text/plain";
}

// 发送一个完整响应：状态行 + 头部 + 文件内容
void send_response(int fd, int status, const char* status_text,
                   const std::string& file_path) {
    std::string body;
    struct stat st;
    bool found = (stat(file_path.c_str(), &st) == 0) && S_ISREG(st.st_mode);

    if (found) {
        FILE* fp = fopen(file_path.c_str(), "rb");
        body.resize(st.st_size);
        fread(&body[0], 1, st.st_size, fp);
        fclose(fp);
    } else {
        status = 404;
        status_text = "Not Found";
        body = "<html><body><h1>404 Not Found</h1></body></html>";
    }

    char header[BUF_SIZE];
    snprintf(header, BUF_SIZE,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             status, status_text,
             found ? get_content_type(file_path) : "text/html",
             body.size());

    write(fd, header, strlen(header));
    write(fd, body.data(), body.size());
}

void handle_request(int connfd) {
    char buf[BUF_SIZE] = {0};
    int n = read(connfd, buf, BUF_SIZE - 1);
    if (n <= 0) return;

    printf("---- request ----\n%s\n-----------------\n", buf);  // 观察原始报文！

    // 只解析请求行：GET /xxx HTTP/1.1
    std::string request(buf);
    size_t sp1 = request.find(' ');
    size_t sp2 = request.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return;

    std::string method = request.substr(0, sp1);
    std::string url    = request.substr(sp1 + 1, sp2 - sp1 - 1);
    if (url == "/") url = "/index.html";          // 默认首页

    printf("method=%s url=%s\n", method.c_str(), url.c_str());

    std::string file_path = std::string(DOC_ROOT) + url;
    if (method == "GET") {
        send_response(connfd, 200, "OK", file_path);
    } else {
        // POST 等暂不支持，返回 501
        const char* msg = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 0\r\n\r\n";
        write(connfd, msg, strlen(msg));
    }
}

int main() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }
    printf("simple http server on port %d, doc root: %s\n", PORT, DOC_ROOT);

    while (true) {
        sockaddr_in client;
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd, (sockaddr*)&client, &len);
        if (connfd < 0) continue;
        printf("new client: %s\n", inet_ntoa(client.sin_addr));
        handle_request(connfd);
        close(connfd);
    }
    return 0;
}
```

### 2.3 编译运行

```bash
g++ -g -Wall -o simple_http simple_http.cpp
./simple_http
```

## ✅ 验证

**验证 1：浏览器访问**

在 **Windows 的浏览器**里打开（WSL2 会自动转发 localhost）：

```
http://127.0.0.1:9006/
```

应看到 TinyWebServer 的 welcome 页面（此时点"登录/注册"还没用，那是 Stage 8 的事）。

**验证 2：观察服务器终端**

浏览器每请求一次，终端打印完整的 HTTP 请求报文 —— **仔细读一遍**，
找到请求行、Host 头、Accept 头，和 2.1 的理论对照。

**验证 3：用 curl 精确验证**（新开 WSL 终端）

```bash
curl -v http://127.0.0.1:9006/index.html
# 期望：< HTTP/1.1 200 OK，且能看到 Content-Length

curl -v http://127.0.0.1:9006/no_such_file.html
# 期望：< HTTP/1.1 404 Not Found，页面显示 404 Not Found

curl -v http://127.0.0.1:9006/xxx.jpg -o test.jpg
# 请求一张真实存在的图片（先看 root/ 里有什么），下载后文件能正常打开
```

**验证 4：请求头对照实验**

```bash
curl -v -X POST http://127.0.0.1:9006/ -d "user=test"
# 期望：501 Not Implemented（POST 要到 Stage 8 才支持）
```

> 🔑 本阶段遗留问题（后面逐个解决）：
> ① 一次只能服务一个客户端（Stage 3、4）
> ② 一次 read 可能读不完整个请求，解析会出错（Stage 5 状态机）
> ③ 大文件先整个读进内存再发送，浪费内存（Stage 5 mmap + writev）
> ④ 每个连接处理完就关闭，无法 keep-alive（Stage 5）

## 🐛 常见问题

**Q1: 浏览器打不开页面，一直转圈？**
可能是 `handle_request` 里的 `read` 在等更多数据。curl 能通而浏览器转圈时，
检查你是否在响应头里写对了 `Content-Length`（写大了浏览器会一直等剩下的内容）。

**Q2: 页面能开但图片裂了？**
`Content-Type` 不对，或文件路径拼错。用 `curl -v` 看返回头，在服务器端打印 `file_path` 排查。

**Q3: 第二次请求没有响应？**
正常！我们是单线程短连接。浏览器对同一域名会并行开多个连接，后面的要排队。
Stage 3 之后这个现象消失。

## 🤔 思考与练习

1. 在 `handle_request` 开头故意把 buf 只读 100 字节（`read(connfd, buf, 100)`），
   用 curl 发一个带很多自定义头的请求，观察解析失败 —— 体会「一次 read 读不完请求」，
   理解为什么 Stage 5 需要状态机。
2. 用 `struct stat` 打印 `st.st_size`，和 `ls -l` 看到的文件大小对比。
3. 把响应头里的 `Connection: close` 改成 `keep-alive` 但不改其他代码，用 curl -v 观察
   会发生什么（提示：服务器 close 了，但浏览器以为还能复用）—— 为 Stage 5 埋下伏笔。
4. 打开原始项目的 `http/README.md` 和 `http_conn.cpp` 的 `do_request()`，
   看看标准答案是怎么做 URL 到文件映射的（不用看懂全部，有个印象即可）。

---

➡️ 下一阶段：[Stage 3：线程同步与线程池](stage-03-threadpool.md)
