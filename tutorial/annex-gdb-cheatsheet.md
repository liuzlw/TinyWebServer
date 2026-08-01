# 附录 A2:gdb 速查表

> 常用命令 + 三个本项目实战例子。**记住:调试的前提是编译时加 `-g`**(CMake 默认就是 Debug)。

## 1. 常用命令速查

| 命令 | 简写 | 作用 |
|---|---|---|
| `gdb ./server` | | 启动 gdb,加载程序 |
| `gdb ./server <args>` | | 加载并传启动参数 |
| `break 文件名:行号` | `b` | 断点:在文件某行 |
| `break 函数名` | `b` | 断点:在函数入口 |
| `info breakpoints` | `i b` | 列出所有断点 |
| `delete 断点编号` | `d` | 删除断点 |
| `run` | `r` | 开始运行(可带参数 `run -m 3`) |
| `continue` | `c` | 继续运行到下一个断点 |
| `next` | `n` | 单步执行,**不进入**函数 |
| `step` | `s` | 单步执行,**进入**函数 |
| `finish` | | 执行完当前函数,返回调用处 |
| `print 变量` | `p` | 打印变量值,如 `p connfd`、`p m_read_idx` |
| `print 变量 = 值` | `p var = 值` | 修改变量值 |
| `x/20c buf` | | 以字符形式看内存(`x/20d` 看整数) |
| `backtrace` | `bt` | 查看调用栈(定位崩溃) |
| `info threads` | `i th` | 列出所有线程 |
| `thread N` | | 切换到线程 N |
| `info locals` | | 查看当前函数的局部变量 |
| `list` | `l` | 显示当前行附近的源码 |
| `watch 变量` | | 变量一变就停 |
| `attach PID` | | 附加到正在运行的进程(调试线上服务器) |
| `detach` | | 从进程分离(attach 之后) |
| `quit` | `q` | 退出 |

**gdb 交互小技巧:**
- 直接按回车 = 重复上一条命令
- `set debuginfod enabled off` 关掉联网下载调试符号的提示(Stage 0 讲过)

## 2. 实战例子 1:观察 accept 阻塞(Stage 1)

```bash
gdb ./server
```

```text
(gdb) break accept
Breakpoint 1 at 0x1180
(gdb) run
... 程序跑到 accept 处停住(阻塞等待连接)
(gdb) next            ← 另开终端: nc 127.0.0.1 9006 连一下,再回来回车
(gdb) print connfd    ← 看到 accept 返回的连接 fd
$1 = 5
(gdb) continue
```

**学到的**:`accept` 阻塞时,断点停在那,`next` 一执行就"醒"了。

## 3. 实战例子 2:观察 epoll 事件(Stage 4)

```bash
gdb ./build/server
```

```text
(gdb) break main.cpp:66        ← 断在 epoll_wait 那行(以实际行号为准)
(gdb) run
(gdb) next                     ← 阻塞在 epoll_wait;另开终端 curl,再回车
(gdb) print n                  ← epoll_wait 返回的就绪事件数
$1 = 1
(gdb) print events[0].data.fd  ← 就绪的 fd
$2 = 5
(gdb) continue
```

**学到的**:`epoll_wait` 返回后,`events` 数组里就是就绪的 fd。

## 4. 实战例子 3:附加到运行中的服务器(Stage 9 压测)

压测时服务器崩溃/卡住?附加 gdb 看它当时在干嘛:

```bash
# 终端 1:先启动服务器
./build/server -m 3 -a 0

# 终端 2:找到 PID 并附加
pgrep -f "build/server"          # 得到 PID
sudo gdb -p <PID>                # 附加(需要权限,可能要 sudo)
```

```text
(gdb) bt                         ← 看它现在卡在哪个函数(崩溃原因在这)
(gdb) print http_conn::m_user_count   ← 当前连接数
(gdb) info threads               ← 看工作线程都在干什么
(gdb) detach                     ← 看完分离,服务器继续跑
```

**学到的**:`attach` 不需要重启程序,直接看运行中的进程状态——排查线上问题的利器。

## 5. 常见报错

| 现象 | 原因 | 解决 |
|---|---|---|
| `No symbol table is loaded` | 编译没加 `-g` | CMake 用 Debug 构建;或 `g++ -g` |
| `Breakpoint 1 at 0x...`(没有行号) | 没有源码行号信息 | 同上的 `-g` 问题 |
| `ptrace: Operation not permitted` | attach 需要权限 | 加 `sudo`,或临时 `sudo sysctl kernel.yama.ptrace_scope=0` |
| 断点打不上(提示 pending) | 函数是模板/还没实例化 | 先 `run` 到那一步再打断点 |
| 中文乱码 | 终端编码 | gdb 里 `set print pretty on` 并保证 UTF-8 终端 |
| `Cannot find bounds of current function` | 优化编译导致 | 用 `-O0` 或 Debug 构建调试 |

## 6. 一句话心法

**gdb 三板斧:打断点(`break`)→ 单步(`next`/`step`)→ 看变量(`print`)。** 加上 `bt` 看崩溃栈,就覆盖了日常调试的 90% 场景。更多的命令等你在实战中遇到问题再查——本表足够用了。
