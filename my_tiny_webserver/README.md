# my_tiny_webserver —— 你的复现工程

这是教程的**复现工作区**:随 [tutorial/](../tutorial/README.md) 各阶段逐步填充,最终结构与原仓库一致。原仓库代码是你的"参考答案"。

当前骨架目录(每阶段往里填代码):

| 目录 | 阶段 | 内容 |
|---|---|---|
| `root/` | Stage 2 | 静态文件(网页/图片/视频) |
| `lock/` | Stage 3 | `locker.h` 锁封装 |
| `threadpool/` | Stage 3 → 9 | `threadpool.h` 线程池 |
| `http/` | Stage 5 | `http_conn.h/.cpp` 连接处理 |
| `timer/` | Stage 6 | `lst_timer.h/.cpp` 定时器 |
| `log/` | Stage 7 | `block_queue.h` + `log.h/.cpp` 日志 |
| `CGImysql/` | Stage 8 | `sql_connection_pool.h/.cpp` 连接池 |

`CMakeLists.txt` 从 Stage 1 起在这里随阶段成长。

> 如果你克隆的仓库里没有这些目录,先执行:
> ```bash
> mkdir -p my_tiny_webserver/{lock,threadpool,http,timer,log,CGImysql,root}
> ```
