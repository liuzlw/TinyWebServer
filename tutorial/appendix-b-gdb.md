# 附录 B：用 gdb 调试 TinyWebServer

> **什么时候读**：完成 [Stage 0](stage-00-environment.md) 的"gdb 初体验"之后、从 [Stage 3](stage-03-threadpool.md) 起**边做边查**。它是主教程里所有"用 gdb 调试"指引的**完整版**。
> **解决什么问题**：从"靠 printf 猜"升级到"用断点、堆栈、core dump 精确定位"。本附录用本项目真实代码做 6 个实战案例（HTTP 状态机、线程池、定时器、段错误、epoll 事件、死锁），带你独立调试这台服务器。
> 全程命令在 Ubuntu 22.04（gdb 12、g++ 11）下验证。所有调试都请在自己的 `my_tiny_webserver/` 工作区里做，仓库只作参考答案。

## 前置要求

- 完成 [Stage 0](stage-00-environment.md)（会启动 gdb、会 `break`/`next`/`print`/`quit`）。
- 编译出**带 `-g` 调试信息**的 `server`：`make` 默认 `DEBUG=1` 就会加 `-g`（见 [附录 A](appendix-a-make-cmake.md) 第四节）。用 `file server` 看到 `with debug_info` 即正确。
- **MySQL 已启动并建好库表**（见 [Stage 8](stage-08-mysql.md)：库 `qgydb`、表 `user`）。本项目在 `sql_pool()` 初始化连接池时，连不上 MySQL 会直接 `exit(1)`，所以没有 MySQL 就无法启动 `server`，后面的案例都做不了。
- 会用一个发 HTTP 请求的工具：`curl`（已装）；空闲连接案例额外需要 `nc`（`sudo apt install netcat-openbsd`）或 `telnet`。
- 理解本项目的并发模型主线（阻塞 → 线程池 → epoll），至少知道 [Stage 3](stage-03-threadpool.md) 的线程池和 [Stage 4](stage-04-epoll.md) 的事件循环在做什么。

> 一个贯穿全篇的提醒：本项目每 5 秒触发一次 `SIGALRM`（定时器心跳），gdb 默认会因此频繁打断。**进入 gdb 后第一件事**就是执行 `handle SIGALRM nostop noprint pass`，否则你会被每 5 秒一次的"Program received signal SIGALRM"烦到怀疑人生。细节见第五节。

## 一、调试心态与方法论

工具是次要的，方法才是关键。遇到 bug 按下面四步走，比乱设断点高效得多：

1. **先复现**：先让 bug 稳定出现，并记录"最简触发操作"。能复现，就成功了一半。
2. **再定位**：用二分法缩小范围——先判断是"编译期/链接期"还是"运行期"，再判断是哪一层（连接建立？读数据？解析？响应？）。在可疑函数的入口打断点，确认程序是否走到那、走到了哪一步。
3. **二分法缩小范围**：不要在 7 个文件里大海捞针。沿着调用链（`eventLoop → dealwithread → process_read → parse_request_line`）一层层断点，找到"上一站正常、这一站异常"的分界点，bug 就在中间。
4. **最小可复现样例**：如果本机项目太大，把可疑逻辑抽成一个几十行的小程序单独调，能更快看清问题，再回到项目里验证。

一个好习惯：**打断点前先想清楚"我要观察哪个变量、期望它是什么值"**。带着预期去看，比盯着屏幕瞎看高效得多。

## 二、gdb 基础命令速查表

进入调试：

```bash
gdb ./server            # 启动 gdb，加载可执行文件
gdb -q -x dbg.gdb ./server   # -q 少打印版权；-x 执行命令脚本（批处理）
gdb -p 12345            # attach 到正在运行的进程（见第六节）
```

常用命令（括号内是简写）：

| 命令 | 作用 | 本项目示例 |
|---|---|---|
| `run` / `r` | 运行程序（可在后面直接跟参数） | `run -p 9007` |
| `start` | 运行并在 `main` 第一行停下 | `start` |
| `break 文件:行号` | 按行打断点 | `break webserver.cpp:393` |
| `break 函数名` | 按函数打断点 | `break http_conn::process_read` |
| `break xxx if 条件` | 条件断点 | `break webserver.cpp:393 if number > 1` |
| `info breakpoints` | 列出所有断点 | — |
| `delete` / `disable` / `enable` | 删除 / 禁用 / 启用断点 | `delete 1` |
| `next` / `n` | 单步，**不进入**函数 | 在 `process_read` 里逐行看状态机 |
| `step` / `s` | 单步，**进入**函数 | 进入 `parse_request_line` |
| `finish` | 执行到当前函数返回 | 在 `parse_request_line` 里 `finish` 看结果 |
| `until 行号` | 执行到指定行（可跳出循环） | `until 391` |
| `continue` / `c` | 继续运行到下一个断点 | — |
| `print 表达式` / `p` | 打印值 | `print m_check_state`、`print/x events[0].events` |
| `display 表达式` | 每次停下自动打印 | `display m_check_state` |
| `watch 变量` | 变量被改写时停下 | `watch http_conn::m_user_count` |
| `backtrace` / `bt` | 打印调用栈 | 定位崩溃点 |
| `frame N` / `up` / `down` | 切换栈帧 | `frame 1` 回到调用者 |
| `info locals` / `info args` | 看局部变量 / 参数 | 在断点处看 `line_status`、`ret` |
| `list` / `l` | 显示源码 | `list` |
| `x/s 地址` | 按字符串查看内存 | `x/s m_read_buf` |
| `set args ...` | 预设程序参数 | `set args -p 9007 -c 1` |
| `layout src` | TUI 源码窗口 | `layout src`（退出用 `Ctrl-x a`） |
| `quit` / `q` | 退出 gdb | — |

关于**传参数**：本项目对应 `./server -p 9007`，在 gdb 里有两种等价写法：

```gdb
(gdb) set args -p 9007 -c 1
(gdb) run
```

或直接：

```gdb
(gdb) run -p 9007 -c 1
```

其中 `-c 1` 是关闭日志（减少干扰），调试时推荐带上；`-p 9007` 换端口，避免和已在跑的实例冲突。

## 三、core dump：定位段错误

段错误（Segmentation fault）发生时，程序直接崩掉、没有报错信息。`core dump` 就是操作系统在崩溃瞬间把进程的内存快照写成一个 `core` 文件，事后用 gdb 回放，`bt` 一眼看到崩溃点。

### 3.1 打开 core 开关并找到 core 文件

```bash
ulimit -c unlimited                       # 允许生成 core（默认可能是 0，即禁止）
cat /proc/sys/kernel/core_pattern         # 看 core 文件写到哪、叫什么
```

```text
core                                     # 表示 core 写到进程的工作目录，名字就叫 core
|/usr/share/apport/apport %p %s ...      # Ubuntu 默认：交给 apport 处理，不会在目录里留下 core
```

> **Ubuntu 22.04 的坑**：默认 `core_pattern` 常是 `|/usr/share/apport/apport ...`，core 会被 apport 吞掉，你 `ls` 找不到 `core` 文件。想让它老老实实落在当前目录，执行：

```bash
sudo sysctl -w kernel.core_pattern=core
```

### 3.2 用一个最小样例体验完整流程

先不碰项目，用一个 4 行小程序把"崩溃 → core → 定位"走一遍：

```bash
cat > seg.c <<'EOF'
#include <stdio.h>
int main(void) {
    int *p = 0;
    *p = 42;              /* 向空指针写入 → 段错误 */
    return 0;
}
EOF
gcc -g seg.c -o seg
ulimit -c unlimited
./seg
```

```text
Segmentation fault (core dumped)
```

`ls` 能看到 `core` 文件，然后：

```bash
gdb ./seg core
```

```gdb
(gdb) bt
#0  0x0000... in main () at seg.c:4
4           *p = 42;
```

`bt` 直接指到 `seg.c:4`——这就是崩溃点。`frame 0` 里 `print p` 会看到 `(int *) 0x0`。

### 3.3 在真实项目里制造并定位一次段错误

在你自己的 `my_tiny_webserver/webserver.cpp` 构造函数里，**临时**把第 6 行改成空指针（**永远不要改仓库文件**）：

```cpp
WebServer::WebServer()
{
    users = nullptr;      // 故意制造 bug：本来应是 new http_conn[MAX_FD]
    ...
}
```

重新编译、运行、发一个请求（`timer()` 里会访问 `users[connfd]`）：

```bash
make
./server -p 9007 -c 1 &
curl http://127.0.0.1:9007/     # 触发崩溃
```

```text
Segmentation fault (core dumped)
```

用 gdb 回放：

```bash
gdb ./server core
```

```gdb
(gdb) bt
#0  http_conn::init (this=0x..., sockfd=5, ...) at http/http_conn.cpp:113
#1  WebServer::timer (this=0x..., connfd=5, ...) at webserver.cpp:163
#2  WebServer::dealclientdata (this=0x...) at webserver.cpp:218
#3  WebServer::eventLoop (this=0x...) at webserver.cpp:398
#4  main (argc=..., argv=...) at main.cpp:38
(gdb) frame 1
(gdb) print users
$1 = (http_conn *) 0x0          # 铁证：users 是空指针
```

从 `#0` 到 `#4` 一路追下来，看到 `users = 0x0` 就真相大白。定位完记得把 `nullptr` 改回 `new http_conn[MAX_FD]`。

> 若崩溃能稳定复现，其实**不生成 core 也行**：直接 `gdb ./server` → `run` → 崩溃时 gdb 会自动停在 `SIGSEGV`，然后 `bt` 即可。core dump 的价值在于"崩溃发生在生产环境、无法现场起 gdb"时能事后分析。

## 四、多线程调试

本项目是典型多线程程序：1 个主线程（`eventLoop` 里的 `epoll_wait`）+ 8 个 worker 线程（线程池）。多线程调试有专属命令：

| 命令 | 作用 |
|---|---|
| `info threads` | 列出所有线程，`*` 号是当前线程 |
| `thread N` | 切换到线程 N |
| `thread apply all bt` | 打印**所有线程**的调用栈 |
| `set scheduler-locking step` | 单步（`step`/`next`）时**只**跑当前线程，其他线程停住 |
| `set scheduler-locking on` | 除当前线程外全部暂停（`continue` 时其他线程也不跑） |
| `set scheduler-locking off` | 恢复默认（`step` 时其他线程可能乱跑） |

**新手最困惑的问题：单步时其他线程"捣乱"**。gdb 里一个线程执行 `next` 时，其他线程默认也在跑，于是你会看到：单步走的不是你想走的那条线、断点莫名其妙命中、变量被别的线程改了。解决办法就是：

```gdb
(gdb) set scheduler-locking step
```

这样 `step`/`next` 期间其他线程被冻结，调试清晰可控。看完全部线程状态后，记得 `set scheduler-locking off` 恢复。

## 五、信号处理（重点：SIGALRM）

gdb 默认会**拦截并停下**几乎所有的信号，即使程序自己已经处理了这个信号。本项目在 `eventListen()` 里注册了信号处理并调用 `alarm(TIMESLOT)`：

```cpp
utils.addsig(SIGALRM, utils.sig_handler, false);
alarm(TIMESLOT);        // TIMESLOT = 5，每 5 秒一次 SIGALRM
```

于是每 5 秒 gdb 都会打断你一次：

```text
Program received signal SIGALRM, Alarm clock.
0x00007f... in __libc_write (...) at ...
```

**这是新手调本项目的第一大坑**。进入 gdb 后的第一件事就是告诉 gdb"这个信号别停、别打印、交给程序处理"：

```gdb
(gdb) handle SIGALRM nostop noprint pass
Signal        Stop      Print   Pass to program   Description
SIGALRM       No        No      Yes               Alarm clock
```

`nostop`（不停下）、`noprint`（不打印）、`pass`（交给程序自己的信号处理函数）三者缺一不可。同理，本项目的 `SIGPIPE`（客户端突然断开时会触发）已被程序忽略，调试时也一并放行：

```gdb
(gdb) handle SIGPIPE nostop noprint pass
```

> 如果你忘了放行 SIGALRM：现象是"我明明在断点等请求，gdb 却每 5 秒跳出来说收到 SIGALRM，`continue` 之后又打断"。原因就是 alarm 定时器在跑。补一条 `handle SIGALRM nostop noprint pass` 再 `continue` 即可，不用重启。

## 六、attach 到运行中的进程

服务器已经在跑、没法用 `gdb ./server` 重启它时，用 attach 把 gdb"挂"到正在运行的进程上：

```bash
pgrep server          # 先拿到进程号，例如 12345
gdb -p 12345
```

```text
Attaching to process 12345
...
(gdb) info threads
(gdb) thread apply all bt
(gdb) detach           # 调试完，让进程继续自己跑
(gdb) quit
```

**权限说明**：Ubuntu 默认 `kernel.yama.ptrace_scope=1`，只允许"父进程调试子进程"或 root。attach 一个无关进程通常会报 `ptrace: Operation not permitted`。两种解法：

```bash
sudo gdb -p 12345                                  # 用 root 权限 attach（最简单）
# 或临时放开限制（需 root，重启后恢复）
sudo sysctl -w kernel.yama.ptrace_scope=0
```

## 七、本项目实战案例

> 每个案例都遵循：**目标 → 命令序列 → 预期观察 → 结论**。请务必先做第七节开头的两件事：`handle SIGALRM nostop noprint pass` + `handle SIGPIPE nostop noprint pass`。

### 案例 a：跟踪 HTTP 状态机

**目标**：看一次 GET 请求如何被逐段解析——`m_check_state` 从"请求行"走到"头部"，`m_url`/`m_method`/`m_version` 依次被填好。

**步骤**（默认 `actor_model=0`，即 Proactor 模式，`process_read` 在 worker 线程里执行）：

```gdb
$ gdb ./server
(gdb) handle SIGALRM nostop noprint pass
(gdb) set args -p 9007 -c 1
(gdb) break http_conn::process_read
(gdb) break http_conn::parse_request_line
(gdb) run
```

在**另一个终端**发请求：

```bash
curl -s http://127.0.0.1:9007/ > /dev/null
```

回到 gdb，第一个断点命中：

```text
Thread 2 "server" hit Breakpoint 1, http_conn::process_read (this=0x...) at http/http_conn.cpp:342
342	    LINE_STATUS line_status = LINE_OK;
```

观察初始状态：

```gdb
(gdb) print m_check_state
$1 = CHECK_STATE_REQUESTLINE
(gdb) print m_read_idx
$2 = 78                          # 读到的字节数（具体值取决于 curl 发的请求头）
(gdb) x/s m_read_buf
0x...: "GET / HTTP/1.1\r\nHost: 127.0.0.1:9007\r\n..."
```

```gdb
(gdb) continue
```

第二个断点命中：

```text
Thread 2 "server" hit Breakpoint 2, http_conn::parse_request_line (this=0x..., text=0x...) at http/http_conn.cpp:242
242	    m_url = strpbrk(text, " \t");
```

```gdb
(gdb) next            # 多 next 几次，或直接 finish 跳到函数末尾
(gdb) print m_url
$3 = 0x... "/judge.html"     # curl 请求 "/"，被补成了 "/judge.html"
(gdb) print m_method
$4 = GET
(gdb) print m_version
$5 = 0x... "HTTP/1.1"
(gdb) print m_check_state
$6 = CHECK_STATE_HEADER       # 已进入"解析头部"阶段
```

**预期观察**：`m_check_state` 由 `CHECK_STATE_REQUESTLINE`（0）→ `CHECK_STATE_HEADER`（1）；`m_read_idx` 是读到的字节数；`m_url`/`m_method`/`m_version` 被逐段切分出来。

**结论**：状态机靠 `m_check_state` 驱动，`parse_request_line` 负责切出方法、URL、版本三要素，并把它推进到下一个状态。

> 提示：若想一路看到 `do_request()` 返回 `FILE_REQUEST`，可再 `break http_conn::do_request` 观察 `m_real_file` 如何拼出磁盘路径。

### 案例 b：跟踪线程池调度

**目标**：看清 8 个 worker 线程平时阻塞在哪、来任务时**哪个**线程抢到活。

**步骤**：

```gdb
(gdb) break threadpool.h:104      # 对应 run() 里的 m_queuestat.wait()
(gdb) run
```

> 模板类成员函数用函数名打断点常因模板实例化/重载写不对，本项目**最稳的写法是用行号** `threadpool.h:104`（第 104 行正是 `m_queuestat.wait();`）。

命中后观察全体线程：

```gdb
(gdb) info threads
(gdb) thread apply all bt
```

```text
  Id   Target Id                             Frame
* 1    Thread ... "server"                   epoll_wait () from /lib/x86_64-linux-gnu/libc.so.6
  2    Thread ... "server"                   sem_wait () from /lib/x86_64-linux-gnu/libc.so.6
  3    Thread ... "server"                   sem_wait () ...
  ...（共 9 个线程：1 个主线程 + 8 个 worker）
```

> `Frame` 列显示的是"最顶层栈帧"：主线程停在 libc 的 `epoll_wait` 里，对它 `bt` 一层就能看到调用者是 `WebServer::eventLoop` 的第 384 行（`epoll_wait` 调用处）；worker 停在 libc 的 `sem_wait` 里，`bt` 能看到 `threadpool<http_conn>::run`（`m_queuestat.wait()` 那一行）。

**预期观察**：主线程（Thread 1）卡在 `epoll_wait`（第 384 行），8 个 worker 都卡在 `sem_wait`（信号量 `m_queuestat` 上等任务）——这是**正常空闲状态**，不是死锁。

再看"谁抢到任务"：删掉当前断点，改在"取任务"那一行打断点：

```gdb
(gdb) delete 1
(gdb) break threadpool.h:111      # T *request = m_workqueue.front();
(gdb) continue
```

另一个终端发请求：

```bash
curl -s http://127.0.0.1:9007/ > /dev/null
```

```gdb
(gdb) info threads
(gdb) bt
```

**预期观察**：这次**只有一个** worker（比如 Thread 4）停在 `threadpool.h:111`（它从队列里取到了任务），其余 7 个 worker 仍停在 `sem_wait`。`bt` 显示 `worker() → run() → ...`。

**结论**：线程池用"信号量 + 互斥锁 + 任务队列"实现；空闲线程在 `sem_wait` 上睡觉，任务到来时恰好一个线程被唤醒、取走队首任务。

### 案例 c：跟踪定时器

**目标**：观察每 5 秒一次的心跳 tick，以及空闲连接约 15 秒后被自动清理。

**步骤**（务必先放行 SIGALRM，否则根本走不到这里）：

```gdb
(gdb) handle SIGALRM nostop noprint pass
(gdb) break sort_timer_lst::tick
(gdb) run -p 9007 -c 1
```

```text
Thread 1 "server" hit Breakpoint 1, sort_timer_lst::tick (this=0x...) at timer/lst_timer.cpp:96
96	void sort_timer_lst::tick()
(gdb) print head
$1 = (util_timer *) 0x0         # 暂无连接，定时器链表为空
(gdb) continue
```

**预期观察**：`sort_timer_lst::tick` 每约 5 秒命中一次（对应 SIGALRM → 管道 → `timer_handler()` → `tick()`）。没有客户端时 `head` 为空（`0x0`）。

再看清理空闲连接。先另开一个终端，用 `nc` 连上来但**不发任何数据**（保持空闲）：

```bash
nc 127.0.0.1 9007          # 连接后什么都不输入，保持挂起
```

回到 gdb，改在回调处打断点：

```gdb
(gdb) delete 1
(gdb) break cb_func         # 定时器到期、真正关闭连接的回调
(gdb) continue
```

约 15 秒后（`expire = 当前时间 + 3 * TIMESLOT`，TIMESLOT=5）：

```text
Thread 1 "server" hit Breakpoint 2, cb_func (user_data=0x...) at timer/lst_timer.cpp:218
(gdb) print user_data->sockfd
$2 = 5                       # 被关闭的空闲连接 fd
(gdb) print http_conn::m_user_count
$3 = 0                       # 连接数已减回 0
```

**预期观察**：空闲连接在 ~15 秒后被 `cb_func` 关闭，`m_user_count` 从 1 减到 0；此时 `nc` 终端会退出。

**结论**：定时器链表由 SIGALRM 每 5 秒驱动一次 `tick()`，到期节点的 `cb_func` 负责真正 `close`。这个"信号 → 管道 → epoll → tick"的链路就是本项目的定时器心脏。

> 相关断点：`break WebServer::deal_timer` 能观察"读写出错或对端 RDHUP 时主动关闭连接"的另一条清理路径；`break Utils::timer_handler` 能直接看到"重新 `alarm(5)`"的续命动作。

### 案例 d：构造段错误 → core dump → bt 定位

**目标**：完整走一遍"制造崩溃 → 生成 core → 定位"的排查流程。

**步骤**：见第三节 3.3。要点重述：

```bash
# 1) 在 webserver.cpp 构造函数里临时把 users 改成 nullptr（仅改你自己的工作区）
# 2) 重新编译并触发崩溃
make && ./server -p 9007 -c 1 &
curl http://127.0.0.1:9007/        # → Segmentation fault (core dumped)
# 3) 回放
gdb ./server core
(gdb) bt
```

**预期观察**：`bt` 最上面几帧是 `http_conn::init` → `WebServer::timer (webserver.cpp:163)` → `WebServer::dealclientdata` → `WebServer::eventLoop` → `main`；`frame 1` 后 `print users` 得到 `(http_conn *) 0x0`。

**结论**：core dump 让"崩溃时程序跑到哪、哪个变量是空"一目了然。养成"崩溃先 `bt`"的习惯，比盯着代码猜快十倍。

### 案例 e：观察 epoll 事件

**目标**：看清 `epoll_wait` 返回后，`number` 和 `events[i].events` 长什么样，区分"信号管道事件"和"客户端事件"。

**步骤**：

```gdb
(gdb) handle SIGALRM nostop noprint pass
(gdb) break webserver.cpp:393      # eventLoop 里 for 循环第一行：int sockfd = events[i].data.fd;
(gdb) run -p 9007 -c 1
```

启动后**先不发请求**，等 5 秒（信号管道事件），断点命中：

```gdb
(gdb) print number
$1 = 1
(gdb) print i
$2 = 0
(gdb) print events[0].data.fd
$3 = 7
(gdb) print m_pipefd[0]
$4 = 7                            # 和上面相等 → 这是信号管道
(gdb) print/x events[0].events
$5 = 0x1                          # EPOLLIN
```

再 `continue`，在另一个终端发请求：

```bash
curl -s http://127.0.0.1:9007/ > /dev/null
```

再次命中，这次：

```gdb
(gdb) print number
$6 = 1
(gdb) print events[0].data.fd
$7 = 3
(gdb) print m_listenfd
$8 = 3                            # 和上面相等 → 这是监听套接字，有连接进来
(gdb) print/x events[0].events
$9 = 0x1                          # EPOLLIN
```

**预期观察**：`events[i].events` 用 `print/x` 看十六进制——`EPOLLIN` 是 `0x1`、`EPOLLRDHUP` 是 `0x2000`。通过 `events[i].data.fd` 与 `m_pipefd[0]`/`m_listenfd` 比对，能判断这个事件来自哪里。（上面会话里的 fd 数值 `7`/`3` 只是示例——你的实际值取决于 MySQL 连接数等环境因素，判断要点是**相等关系**，不是具体数字。）

**结论**：epoll 把"有事件的文件描述符 + 事件类型"打包成 `epoll_event` 数组，`eventLoop` 靠 `events[i].events` 的位掩码分支到 `dealwithread`/`dealwithwrite` 等不同处理。用 `print/x` 才能直观看出这些位。

### 案例 f：死锁排查思路

**目标**：服务器"卡死没响应"时，用 attach + `bt` 判断每个线程卡在哪、是不是死锁。

**步骤**：

```bash
pgrep server            # 假设得到 12345
sudo gdb -p 12345
```

```gdb
(gdb) info threads
(gdb) thread apply all bt
```

**预期观察**：正常空闲时你会看到——

- 主线程卡在 `epoll_wait`（等事件，正常）；
- 8 个 worker 卡在 `sem_wait`（等任务，正常）。

这**不是死锁**。真正的死锁特征是：有线程永远卡在 `pthread_mutex_lock`（拿不到锁）或 `pthread_cond_wait`（等不到条件变量），且相互等待。排查思路是：对每个卡住的线程 `thread N` → `bt`，看它在等哪把锁（`info locals` 里的 `m_mutex` 地址），再找出"谁持有这把锁却不去释放"。

**结论**：多线程卡死先别急着改代码，`thread apply all bt` 把每个线程的"卡点"摊开看，两把锁的持有关系一对，死锁成因就浮出水面。

> 本项目当前线程池用 `sem`（信号量）做同步，所以空闲 worker 显示 `sem_wait`；仓库 `lock/locker.h` 里还有个 `cond` 类封装了 `pthread_cond_wait`，若你日后用条件变量重构，就要学会在 `bt` 里辨认 `pthread_cond_wait` 的卡点。

## 验收清单

逐条走通并记录观察结果，才算掌握 gdb：

- [ ] **放行信号**：进入 gdb 后执行 `handle SIGALRM nostop noprint pass`，再 `run`，等待 10 秒以上，gdb **不再**出现 "Program received signal SIGALRM"。
- [ ] **案例 a**：断点 `http_conn::process_read` 命中后，`print m_check_state` 得 `CHECK_STATE_REQUESTLINE`（或 0）、`print m_read_idx` 得大于 0 的字节数、`x/s m_read_buf` 看到 `GET / HTTP/1.1...`；`continue` 到 `parse_request_line` 后，`print m_url` 得 `"/judge.html"`、`print m_method` 得 `GET`、`print m_check_state` 得 `CHECK_STATE_HEADER`（或 1）。
- [ ] **案例 b**：断点 `threadpool.h:104` 命中后，`info threads` 看到 1 个主线程 + 8 个 worker；`thread apply all bt` 看到主线程在 `epoll_wait`、worker 在 `sem_wait`；改断点 `threadpool.h:111` 并发请求后，**只有一个** worker 停在该行。
- [ ] **案例 c**：`break sort_timer_lst::tick` 后约每 5 秒命中一次；用 `nc` 建立空闲连接后 `break cb_func`，约 15 秒后命中，`print user_data->sockfd` 得到被关闭的 fd，`print http_conn::m_user_count` 减 1。
- [ ] **案例 d**：`seg.c` 崩溃后目录里出现 `core` 文件，`gdb ./seg core` 的 `bt` 定位到 `seg.c:4`；把 `users` 置空后触发崩溃，`gdb ./server core` 的 `bt` 定位到 `webserver.cpp:163`，且 `print users` 得 `0x0`。
- [ ] **案例 e**：断点 `webserver.cpp:393` 命中后，`print number`、`print/x events[i].events` 可用；能区分信号管道事件（`events[i].data.fd == m_pipefd[0]`，events 为 `0x1`）与客户端事件（`events[i].data.fd == m_listenfd`）。
- [ ] **案例 f**：用 `sudo gdb -p <pid>` attach 到运行中的 server，`thread apply all bt` 能正确读出主线程在 `epoll_wait`、worker 在 `sem_wait`，并能解释这不是死锁。
- [ ] **独立排查**：不翻文档，自己用 core dump + `bt` 定位出一个自己制造的崩溃点。

## 常见问题

1. **gdb 提示 "No symbol table is loaded" 或看不到变量名**：二进制没有调试信息。确认是用 `make`（`DEBUG=1`）编译的、带 `-g`；`file server` 里应有 `with debug_info`。不带 `-g` 的二进制 gdb 只能看到地址，看不到源码和变量。

2. **变量显示 `<optimized out>`**：用了 `-O2` 优化，变量被编译器优化掉了。调试时用 `-g`（或 `-g -O0`），别用 `-O2`（见 [附录 A](appendix-a-make-cmake.md) 的 DEBUG 开关）。

3. **断点明明设了却不进**：源码和二进制**不一致**——你改了代码但没重新 `make`。gdb 是按行号/符号匹配二进制的，改了代码要重新编译再调试；否则会看到断点"漂移"或干脆不命中。

4. **无法断模板函数（如线程池的 `run`）**：模板实例化后符号名又长又乱，函数名断点常写不对。**直接用行号**：`break threadpool.h:104`。想试函数名，可用引号完整写法 `break 'threadpool<http_conn>::run()'`，但行号最省事。

5. **多线程单步乱跳、变量被别的线程改**：默认 `scheduler-locking` 是 `off`，单步时其他线程也在跑。执行 `set scheduler-locking step`，让单步期间冻结其他线程；用完 `set scheduler-locking off` 恢复。

6. **每 5 秒被 "SIGALRM" 打断，烦不胜烦**：本项目 `alarm(5)` 定时触发 SIGALRM。进入 gdb 第一件事：`handle SIGALRM nostop noprint pass`。同理把 `handle SIGPIPE nostop noprint pass` 也加上。

7. **`gdb -p <pid>` 报 `ptrace: Operation not permitted`**：Ubuntu 的 `ptrace_scope=1` 限制 attach。用 `sudo gdb -p <pid>`，或 `sudo sysctl -w kernel.yama.ptrace_scope=0`。

8. **崩溃了但找不到 `core` 文件**：两个原因——一是没 `ulimit -c unlimited`（默认 0 不生成）；二是 Ubuntu 默认 `core_pattern` 把 core 交给了 apport。检查 `cat /proc/sys/kernel/core_pattern`，若含 `apport`，用 `sudo sysctl -w kernel.core_pattern=core` 改回本地生成。

## 练习

1. **故意改错 `parse_line` 看 404 流**：在 `http_conn.cpp` 的 `parse_line()` 里，把判 `'\n'` 的分支条件临时改错（比如 `m_checked_idx > 1` 改成 `m_checked_idx > 100`），重新编译，用 curl 发一个请求，观察服务器是否走到 `BAD_REQUEST`、响应 404；用 gdb 断点 `http_conn::process_read` 定位到出错的那一行。

2. **观察 keep-alive 下 `process` 被调用两次**：`curl -v http://127.0.0.1:9007/ http://127.0.0.1:9007/fans.html`（同一个连接连发两个请求），断点 `http_conn::process`，观察它是否被调用两次，`print m_read_idx` 每次是如何从新请求重新累加的。

3. **把线程数改成 1 看调度**：`run -p 9007 -t 1 -c 1`，用案例 b 的方法断点 `threadpool.h:111`，观察是否永远只有一个 worker 线程在抢任务。

4. **用 `watch` 盯连接数**：`watch http_conn::m_user_count`，然后建立/断开连接，观察 gdb 每次在连接数变化时自动停下，并 `bt` 看是谁改的。

5. **写一个 gdb 批处理脚本**：把 `handle SIGALRM nostop noprint pass`、`set args -p 9007 -c 1`、`break http_conn::parse_request_line`、`run` 写进 `dbg.gdb`，用 `gdb -q -x dbg.gdb ./server` 一键跑起来，验证脚本调试可用。

6. **制造并排查一次死锁**：临时在 `lock/locker.h` 的某个 `lock()` 后注释掉对应的 `unlock()`（仅改自己的工作区），让第二个线程拿不到锁，用案例 f 的 attach + `thread apply all bt` 找出卡在 `pthread_mutex_lock` 的线程和它想拿的那把锁。

---

> 配套阅读：构建工具链见 [附录 A](appendix-a-make-cmake.md)；阶段依赖与全貌见 [主索引](README.md)。
