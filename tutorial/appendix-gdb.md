# 附录 B：GDB 调试实战

> 目标：能调试单线程/多线程程序，能分析崩溃现场。网络程序没法靠 printf 调一切，
> gdb 是服务器开发的核心技能。

## 1. 准备：编译必须带 -g

```bash
# CMake 项目
cmake -DCMAKE_BUILD_TYPE=Debug .. && make
# 或手动
g++ -g -o server main.cpp ...
```

> ⚠️ Release（-O2）会优化掉变量、内联函数，gdb 里显示 `<optimized out>`，调试请用 Debug。

## 2. 核心命令速查

### 启动与运行

```gdb
gdb ./server              # 启动调试
gdb ./server core         # 分析 core dump
gdb -p 12345              # 附加到运行中的进程（调试已启动的服务器！）

(gdb) run                 # 运行（简写 r）
(gdb) run -p 9007 -m 3    # 带命令行参数运行
(gdb) kill                # 停止被调试程序
(gdb) quit                # 退出 gdb（q）
```

### 断点

```gdb
(gdb) break main                  # 按函数名
(gdb) break http_conn.cpp:120     # 按文件:行号
(gdb) break parse_line if m_checked_idx > 100   # 条件断点（神器）
(gdb) info breakpoints            # 查看所有断点（i b）
(gdb) delete 2                    # 删除 2 号断点
(gdb) disable 1 / enable 1        # 临时禁用/启用
(gdb) watch m_user_count          # 观察点：变量变化时停下
```

### 单步与查看

```gdb
(gdb) next        # 下一行，不进函数（n）
(gdb) step        # 下一行，进函数（s）
(gdb) finish      # 跑完当前函数
(gdb) until 150   # 跑到 150 行（跳出循环神器）
(gdb) continue    # 继续到下一个断点（c）

(gdb) print m_read_idx          # 打印变量（p）
(gdb) print/x m_method          # 十六进制打印
(gdb) ptype http_conn           # 查看类型定义
(gdb) info locals               # 所有局部变量
(gdb) info args                 # 函数参数
(gdb) display m_checked_idx     # 每步自动显示
(gdb) list                      # 查看附近源码（l）
(gdb) backtrace                 # 调用栈（bt）
(gdb) frame 2                   # 切到栈帧 2（看上层函数的变量）
```

## 3. 多线程调试（本项目必备）

```gdb
(gdb) info threads              # 列出所有线程，* 号是当前线程
(gdb) thread 3                  # 切换到 3 号线程
(gdb) thread apply all bt       # ★ 所有线程的调用栈（排查死锁第一步）
(gdb) break threadpool.h:80 thread 2   # 只在 2 号线程里断
(gdb) set scheduler-locking on  # 单步时锁定其他线程（不让它们跑）
(gdb) set scheduler-locking off # 恢复
```

**排查死锁的标准动作**：程序卡死 → `gdb -p <pid>` 附加 →
`thread apply all bt` → 看所有线程都卡在哪个锁上 → 拼出循环等待的环。

## 4. 崩溃分析：core dump

服务器崩了、现场已经消失怎么办？让系统把崩溃瞬间的内存完整存下来：

```bash
ulimit -c unlimited                              # 允许生成 core
echo 'core.%p' | sudo tee /proc/sys/kernel/core_pattern   # core 文件名带 pid

./server                                         # 跑到崩溃
gdb ./server core.12345                          # 加载现场
(gdb) bt                                         # 崩溃点的调用栈
(gdb) frame 0                                    # 看崩溃那行的变量
(gdb) info registers                             # 寄存器（段错误时看地址）
```

配合 AddressSanitizer 更省力（不用等 core，出错当场打印详细信息）：

```cmake
add_compile_options(-fsanitize=address -g)
add_link_options(-fsanitize=address)
```

## 5. 本项目典型调试场景

### 场景 1：HTTP 解析出错，想看状态机怎么走的

```gdb
(gdb) break http_conn::process_read
(gdb) commands                    # 给断点挂自动动作
>silent
>printf "state=%d, line=[%s]\n", m_check_state, get_line()
>continue
>end
(gdb) run
# 浏览器访问一次，状态机的每一步自动打印出来，不用手动 step
```

### 场景 2：定时器没踢人

```gdb
(gdb) break sort_timer_lst::tick
(gdb) run
# 等 alarm 触发后断下：
(gdb) print head->expire
(gdb) print time(0)             # 对比当前时间
(gdb) next                      # 看 while 循环走没走进去
```

### 场景 3：压测时偶发崩溃（最难的一类）

偶发问题 printf 调不动（一加日志时序就变了）。正确姿势：

```bash
ulimit -c unlimited
./server &                       # 跑压测直到崩溃
gdb ./server core.*              # 事后分析
(gdb) bt                         # 哪个函数崩的
(gdb) info threads               # 当时有哪些线程
(gdb) thread apply all bt        # 各线程当时在干什么 → 找竞态
```

再配合 ASan/TSan（`-fsanitize=thread` 检测数据竞争）基本能定位 90% 的并发 bug。

## 6. TUI 模式：边看代码边调

```bash
gdb -tui ./server
# 或 gdb 内按 Ctrl+X A 切换
```

上窗口源码、下窗口命令，`layout regs` 还能看寄存器。

## 🤔 练习

1. 在 `parse_line` 设条件断点 `m_checked_idx == 0`，浏览器访问后单步走完整个状态机。
2. 制造一个死锁（故意在持有锁 A 时申请锁 B，另一处反过来），用
   `thread apply all bt` 把它找出来。
3. 故意写个空指针解引用崩溃，用 core dump 定位到具体行号。
