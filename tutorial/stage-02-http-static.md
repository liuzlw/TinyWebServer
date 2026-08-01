# Stage 2 单线程 HTTP 静态服务器

> 🏆 **本教程第一个大里程碑**:学完这一阶段,你能在浏览器里看到自己服务器吐出的网页。

## 1. 本阶段目标

- [ ] 理解 HTTP 请求/响应报文格式
- [ ] 会解析请求行,把 URL 映射到文件
- [ ] 拼出合法的 HTTP 响应(200 / 404)
- [ ] **在浏览器里打开自己的网页**

**最终效果:**

```text
浏览器访问 http://127.0.0.1:9006/welcome.html  → 看到 Welcome 页面
curl 访问 http://127.0.0.1:9006/nope.html      → 收到 404
```

## 2. 前置知识

- C1:指针、`std::string`;C2:`std::ifstream` 读文件(新增)
- S1:socket 全流程——本阶段就是在 S1 的基础上,把"原样回显"换成"解析请求、返回网页"
- 新增:**HTTP 协议基础**(下面讲)

## 3. HTTP 协议:你要听懂的两段话

HTTP 是浏览器(客户端)和服务器之间"你一句、我一句"的协议。

### 请求(浏览器 → 服务器)

```text
GET /welcome.html HTTP/1.1\r\n      ← 请求行:方法 + 路径 + 版本
Host: 127.0.0.1:9006\r\n            ← 请求头(可有多个)
\r\n                                ← 空行:头部结束
                                     ← 这里是 body(GET 请求一般没有)
```

- 第一行叫**请求行**,三部分:方法(`GET`/`POST`)、路径(`/welcome.html`)、版本(`HTTP/1.1`)
- 之后是**请求头**,每行 `名字: 值`,以空行结束

### 响应(服务器 → 浏览器)

```text
HTTP/1.1 200 OK\r\n                 ← 状态行:版本 + 状态码 + 说明
Content-Type: text/html\r\n         ← 响应头:告诉浏览器内容类型
Content-Length: 98\r\n              ← 响应头:body 字节数
\r\n                                ← 空行
<html>...</html>                    ← body:真正的网页内容
```

- 第一行叫**状态行**,`200` 表示成功、`404` 表示找不到
- **`Content-Length` 必须写对**,浏览器靠它知道 body 有多长

> **注意结尾的 `\r\n`**:HTTP 的行分隔符是 `\r\n`(回车+换行),不是只有一个 `\n`。拼响应时漏掉 `\r` 很多解析器会出问题。

## 4. 设计思路

我们的服务器流程(对比 S1 的回显):

```text
accept 连接
   │
   ▼
read() 读整个请求          ← S1 是"边读边回",这里先读完
   │
   ▼
解析请求行,取出路径        ← 例:GET /welcome.html → "/welcome.html"
   │
   ▼
拼接文件路径 root/ + 路径   ← root/welcome.html
   │
   ▼
打开文件?
 ├─ 能 → 200 + 文件内容
 └─ 不能 → 404 + 错误页
   │
   ▼
write() 发回响应 → close
```

## 5. 完整代码

**第一步:准备静态文件目录。**

在 `my_tiny_webserver/` 下建 `root/` 目录,新建一个简单的 `welcome.html`:

```html
<html><head><title>welcome</title></head>
<body><h1>Welcome to TinyWebServer!</h1></body>
</html>
```

> 想测试图片、视频等更多文件?可以把原仓库的静态文件拷过来:`cp -r ../root/* root/`(在原仓库里运行,`../root` 就是原项目的资源目录)。本阶段先保证文本文件跑通。

**第二步:替换 `my_tiny_webserver/main.cpp`** 为:

```cpp
// main.cpp —— 单线程 HTTP 静态服务器(Stage 2)
#include <iostream>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

const int PORT = 9006;
const std::string ROOT_DIR = "root";   // 静态文件根目录

// 根据扩展名返回 Content-Type
std::string get_content_type(const std::string &path) {
    if (path.size() >= 5 && path.substr(path.size() - 5) == ".html") return "text/html";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".css")  return "text/css";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".jpg")  return "image/jpeg";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".gif")  return "image/gif";
    return "text/plain";
}

int main() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listenfd, 5) < 0) { perror("listen"); return 1; }
    std::cout << "HTTP 服务器已启动, 监听端口 " << PORT << std::endl;

    while (true) {
        struct sockaddr_in client;
        socklen_t len = sizeof(client);
        int connfd = accept(listenfd, (struct sockaddr *)&client, &len);
        if (connfd < 0) { perror("accept"); continue; }

        // 1. 读请求
        char buf[4096];
        ssize_t n = read(connfd, buf, sizeof(buf) - 1);
        if (n <= 0) { close(connfd); continue; }
        buf[n] = '\0';
        std::string request(buf);

        // 2. 解析请求行,提取 URL 路径
        // 请求行格式: GET /welcome.html HTTP/1.1
        std::string path = "/";
        size_t sp1 = request.find(' ');
        size_t sp2 = request.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            path = request.substr(sp1 + 1, sp2 - sp1 - 1);
        }
        std::cout << "请求: " << path << std::endl;

        // 3. 默认首页
        if (path == "/") path = "/welcome.html";

        // 4. 拼出文件路径 root + path,读文件
        std::string file_path = ROOT_DIR + path;
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            // 404
            std::string body = "<html><body><h1>404 Not Found</h1></body></html>";
            std::string response = "HTTP/1.1 404 Not Found\r\n"
                                   "Content-Type: text/html\r\n"
                                   "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                   "\r\n" + body;
            write(connfd, response.c_str(), response.size());
            std::cout << "  404: " << file_path << std::endl;
        } else {
            // 读文件内容
            std::ostringstream oss;
            oss << file.rdbuf();
            std::string body = oss.str();

            // 拼 HTTP 响应
            std::string response = "HTTP/1.1 200 OK\r\n"
                                   "Content-Type: " + get_content_type(path) + "\r\n"
                                   "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                   "Connection: close\r\n"
                                   "\r\n" + body;
            write(connfd, response.c_str(), response.size());
            std::cout << "  200: " << file_path << " (" << body.size() << " 字节)" << std::endl;
        }

        close(connfd);
    }
    close(listenfd);
    return 0;
}
```

**逐段讲解:**

| 代码 | 作用 | 细节 |
|---|---|---|
| `std::ifstream file(path, std::ios::binary)` | 以二进制方式打开文件 | `binary` 让 jpg 等二进制文件不被转换 |
| `oss << file.rdbuf()` | 一次性读出整个文件内容 | `rdbuf()` 返回文件缓冲,写入 `oss` 再转成 string |
| `request.find(' ')` | 在请求里找空格位置 | 请求行 `GET /xx HTTP/1.1`,两个空格夹着路径 |
| `request.substr(sp1+1, sp2-sp1-1)` | 截取两个空格之间的路径 | 得到 `/welcome.html` |
| `path == "/"` 时换默认页 | 访问根路径给首页 | 浏览器访问 `http://ip:9006/` 时,`path` 是 `/` |
| `std::to_string(body.size())` | 数字转字符串拼进响应头 | `Content-Length` 必须是文本 |
| `!file` | 文件打不开(不存在/权限) | 返回 404 |

## 6. 编译与运行

更新 `CMakeLists.txt`(还是原来的,main.cpp 换内容即可):

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp)
```

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
./build/server
```

**预期输出:**

```text
HTTP 服务器已启动, 监听端口 9006
```

**保持运行,在另一个 WSL 终端测试:**

```bash
curl -v http://127.0.0.1:9006/welcome.html
```

**预期输出(重点看 `< HTTP` 开头的响应头和 body):**

```text
> GET /welcome.html HTTP/1.1
> Host: 127.0.0.1:9006
...
< HTTP/1.1 200 OK
< Content-Type: text/html
< Content-Length: 98
< Connection: close
<
<html><head><title>welcome</title></head>
<body><h1>Welcome to TinyWebServer!</h1></body>
</html>
```

`<` 开头的是服务器回的东西。**这就是你自己拼出来的合法 HTTP 响应。**

### 在浏览器里看

打开 Windows 浏览器,地址栏输入:

```text
http://127.0.0.1:9006/welcome.html
```

> WSL2 会把本机 localhost 自动转发到 WSL 里,所以 Windows 浏览器能直接访问。看到 Welcome 页面就成功了!

## 7. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | `curl -v http://127.0.0.1:9006/welcome.html` | `HTTP/1.1 200 OK` + 页面内容 | ☐ |
| 2 | 浏览器打开 `http://127.0.0.1:9006/welcome.html` | 看到 Welcome 页面 | ☐ |
| 3 | `curl -v http://127.0.0.1:9006/` | 自动跳转 `welcome.html`,200 | ☐ |
| 4 | `curl http://127.0.0.1:9006/nope.html` | 404 状态码 + 404 页面 | ☐ |
| 5 | 服务器终端 | 能看到每次请求的日志(路径 + 状态码 + 字节数) | ☐ |
| 6 | 拷入一张 jpg 后 `curl -o /dev/null http://127.0.0.1:9006/test1.jpg` | 200,字节数与文件一致 | ☐ |

## 8. 调试技巧

### 用 gdb 看解析结果

想知道解析出来的路径对不对,在解析处打断点看变量:

```bash
gdb ./build/server
```

```text
(gdb) break main.cpp:70     ← 断在解析路径那行(以你实际行号为准)
(gdb) run
(gdb) next                  ← 另开终端 curl 访问后,单步
(gdb) print path
$1 = "/welcome.html"
(gdb) continue
```

### 用 curl -v 排查响应问题

`curl -v` 会打印完整的请求(`>`)和响应(`<`),是调试 HTTP 的利器。看到 `curl: (52) Empty reply from server` 通常是响应头格式不对(比如忘了 `\r\n` 空行)。

## 9. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 浏览器显示"无法访问此网站" | 服务器没启动 / 端口不对 | 确认 `./build/server` 在运行 |
| 浏览器显示 404 | `root/` 目录下没有这个文件 | 确认 `root/welcome.html` 存在,路径拼写一致 |
| `curl` 收到空响应 | 响应头没以空行 `\r\n` 结尾 | 检查拼响应字符串,`\r\n\r\n` 那行 |
| 图片显示破图 | 没按二进制读/写 | 打开文件用 `std::ios::binary`(代码已处理) |
| 中文字符乱码 | 没声明 charset | 响应头可加 `Content-Type: text/html; charset=utf-8` |
| 一次请求没问题,连续刷新偶尔出错 | **read 一次可能没读全** | 本阶段的已知局限,S5 状态机解决 |

> **关于"read 一次没读全":** 本阶段我们假设请求一次 `read` 就能读完。对本地小请求基本成立,但严格来说 TCP 是流式传输,可能"半包"或"粘包"。S4/S5 会用循环读取 + 状态机彻底解决。现在先接受这个不完美,继续往前走。

## 10. 与原项目对照

| 本阶段 | 原项目对应 |
|---|---|
| 请求行解析(两个空格夹路径) | `http_conn.cpp` 的 `parse_request_line()` |
| `root/` 静态文件映射 | `http_conn.cpp` 的 `do_request()`(含注册登录分支,Stage 8) |
| 拼 200/404 响应 | `http_conn.cpp` 的 `process_write()` |
| 发送响应 | `http_conn.cpp` 的 `write()`(用 `writev`,Stage 5) |

> 本阶段是最朴素的实现:一次 read、一次 write、单线程。原项目在 S4(epoll)和 S5(状态机)里把它改造成高并发、健壮的版本——但你写的核心逻辑(路径→文件→响应)和原项目是同一个骨架。

## 11. 下一步

进入 **[Stage 3 锁 + 线程池](stage-03-threadpool.md)**——解决"一个连接处理完才能接下一个"的问题,让服务器真正并发起来。
