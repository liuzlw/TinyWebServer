/**
 * Phase 7 — 非阻塞 Socket + epoll Echo 服务器
 *
 * 这是一个完整的单线程 echo 服务器：
 * - telnet 连接后，输入任意文本，服务器原样返回
 * - 支持 ET 模式 + EPOLLONESHOT
 * - 多个客户端同时连接时各自独立交互
 *
 * [C++ 语法要点]
 * - struct sockaddr_in: C 结构体，表示 IPv4 地址
 * - C 风格字符串操作：strlen、bzero 等，因为 socket API 是 C 的
 * - #define: 宏定义常量（C++ 更推荐用 constexpr，但这里遵循项目风格）
 */

#include <sys/socket.h>   // socket / bind / listen / accept / send / recv
#include <netinet/in.h>   // sockaddr_in / htonl / htons / INADDR_ANY
#include <arpa/inet.h>    // inet_ntoa
#include <sys/epoll.h>    // epoll_create / epoll_ctl / epoll_wait
#include <fcntl.h>        // fcntl (设置非阻塞)
#include <unistd.h>       // close
#include <stdio.h>        // printf
#include <string.h>       // strlen / bzero
#include <errno.h>        // errno / EAGAIN
#include <assert.h>       // assert

#define MAX_EVENTS 1024   // [语法] 宏定义。C++11 更推荐的写法: constexpr int MAX_EVENTS = 1024;
#define BUF_SIZE 4096

/**
 * 设置文件描述符为非阻塞模式
 *
 * [理论]
 * 默认 socket 是阻塞的：recv 在无数据时会一直等。
 * 非阻塞 socket：recv 在无数据时立即返回 -1，errno = EAGAIN。
 *
 * fcntl(fd, F_GETFL):  获取当前的文件描述符标志
 * fcntl(fd, F_SETFL):  设置新的文件描述符标志
 * O_NONBLOCK:           非阻塞标志
 */
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;  // [语法] | 是位或运算
    fcntl(fd, F_SETFL, new_option);
    return old_option;  // 返回旧标志，调用者可能需要恢复
}

/**
 * 把文件描述符注册到 epoll
 *
 * [理论] EPOLLET vs 默认（LT）：
 * - LT（水平触发）：如果数据没读完，下次 epoll_wait 还会通知
 * - ET（边缘触发）：只在"从无到有"那一刻通知一次。必须循环读到 EAGAIN
 *
 * [理论] EPOLLONESHOT：
 * - 触发一次后自动脱钩，直到 epoll_ctl(MOD) 重新注册
 * - 多线程中保证同一个 fd 只被一个线程处理
 *
 * [理论] EPOLLRDHUP：
 * - 对端关闭连接（半关闭）时触发
 * - 比检查 recv 返回 0 更早得到通知
 */
void addfd(int epollfd, int fd, bool one_shot, bool use_et) {
    epoll_event event;
    event.data.fd = fd;     // data.fd 存储文件描述符

    event.events = EPOLLIN;          // 监视可读事件
    if (use_et) event.events |= EPOLLET;       // 边缘触发
    event.events |= EPOLLRDHUP;     // 对端半关闭通知
    if (one_shot) event.events |= EPOLLONESHOT; // 一次性触发

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

int main(int argc, char* argv[]) {
    int port = 9006;
    if (argc > 1) port = atoi(argv[1]);

    printf("=== Phase 7: Echo Server ===\n");
    printf("Listening on port %d (ET mode + EPOLLONESHOT)\n", port);
    printf("Test: telnet 127.0.0.1 %d\n", port);

    // ========== Step 1: 创建 socket ==========
    // PF_INET: IPv4 协议族
    // SOCK_STREAM: TCP（流式套接字）
    // 0: 自动选择协议（对 TCP 就是 IPPROTO_TCP）
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // 端口复用：避免重启时 "Address already in use"
    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // ========== Step 2: 绑定地址 ==========
    struct sockaddr_in address;                 // C 风格结构体
    bzero(&address, sizeof(address));           // 清零（bzero 是 memset 的 BSD 等价）
    address.sin_family = AF_INET;               // IPv4
    address.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡（0.0.0.0）
    // htonl = Host TO Network Long（字节序转换）
    address.sin_port = htons(port);             // htons = Host TO Network Short

    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    // ========== Step 3: 监听 ==========
    // backlog = 5: 已完成三次握手的队列最大长度
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // ========== Step 4: 创建 epoll ==========
    // epoll_create 的参数在 Linux 2.6.8+ 被忽略（>0 即可）
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    // listenfd 不用 EPOLLONESHOT（会不断有新连接来）
    addfd(epollfd, listenfd, false, true);

    // ========== Step 5: 事件循环 ==========
    epoll_event events[MAX_EVENTS];    // 就绪事件数组
    char buf[BUF_SIZE];                // 读缓冲区

    while (true) {
        // epoll_wait: 阻塞等待事件。timeout = -1 表示无限等待
        int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds < 0 && errno != EINTR) {
            printf("epoll_wait error: %s\n", strerror(errno));
            break;
        }

        for (int i = 0; i < nfds; ++i) {
            int sockfd = events[i].data.fd;

            // -- 新连接 --
            if (sockfd == listenfd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                // ET 模式下必须循环 accept！可能有多个连接同时排队
                while (true) {
                    int connfd = accept(listenfd,
                                       (struct sockaddr*)&client_addr,
                                       &client_len);
                    if (connfd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // 没有更多连接了
                        }
                        printf("accept error: %s\n", strerror(errno));
                        break;
                    }
                    printf("New connection: fd=%d from %s:%d\n",
                           connfd,
                           inet_ntoa(client_addr.sin_addr),  // inet_ntoa: 网络地址 → 字符串
                           ntohs(client_addr.sin_port));      // ntohs: 网络字节序 → 主机字节序

                    // connfd 加 EPOLLONESHOT：多线程环境下安全
                    addfd(epollfd, connfd, true, true);
                }
            }
            // -- 可读事件 --
            else if (events[i].events & EPOLLIN) {
                // ET 模式：必须循环读直到 EAGAIN
                while (true) {
                    int n = recv(sockfd, buf, BUF_SIZE - 1, 0);
                    if (n == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;  // 读完了
                        }
                        // 真正的错误
                        printf("recv error on fd=%d: %s\n", sockfd, strerror(errno));
                        close(sockfd);
                        break;
                    } else if (n == 0) {
                        // recv 返回 0 = 对端关闭连接
                        printf("Connection closed: fd=%d\n", sockfd);
                        close(sockfd);
                        break;
                    } else {
                        buf[n] = '\0';
                        printf("Received from fd=%d: %s", sockfd, buf);

                        // Echo: 原样返回
                        // ⚠️ 注意：这里简化了。生产代码需要处理"短写"。
                        int sent = send(sockfd, buf, n, 0);
                        if (sent == -1 && errno == EAGAIN) {
                            printf("  [send would block on fd=%d]\n", sockfd);
                        }
                    }
                }

                // EPOLLONESHOT: 处理完后重新注册
                epoll_event ev;
                ev.data.fd = sockfd;
                ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
                epoll_ctl(epollfd, EPOLL_CTL_MOD, sockfd, &ev);
            }
            // -- 错误 / 挂断 --
            else if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                printf("Error/Hangup on fd=%d (events=0x%x)\n",
                       sockfd, events[i].events);
                close(sockfd);
            }
        }
    }

    close(listenfd);
    close(epollfd);
    return 0;
}
