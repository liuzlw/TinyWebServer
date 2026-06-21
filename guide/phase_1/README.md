# Phase 1 —— 线程同步原语

## 本阶段目标

用 C++ 类封装 pthread 的三种同步原语，实现一个**生产者-消费者**测试程序。

**可见结果：**

```
=== Producer-Consumer Test ===
Producers: 2, each produces 50 items
Consumers: 3
Buffer size: 100

[Producer 1] produced item 1000 (buffer: 1/100)
[Consumer 1] consumed item 1000 (buffer: 0/100)
...
[Consumer 2] exiting (all produced items consumed)

=== Results ===
Total produced:  100
Total consumed:  100
Buffer remaining: 0
✅ PASS: All items consumed exactly once!
```

**验收标准：**

- [ ] 程序多次运行，每次都输出 `✅ PASS`
- [ ] 用 `ThreadSanitizer` 检查无数据竞争（`g++ -fsanitize=thread`）
- [ ] 理解为什么 `while` 而不是 `if` 检查条件

---

## 理论与机制

### 1. 为什么需要线程同步？

当多个线程访问同一块内存时，如果至少有一个线程在写，就会出现**数据竞争（Data Race）**。

**一个经典的数据竞争例子：**

```cpp
// 线程 A 和 线程 B 同时执行 counter++
// 这一个操作在 CPU 层面是三条指令：
//   1. LOAD  counter → register   (从内存读取)
//   2. ADD   register, 1           (在寄存器中加 1)
//   3. STORE register → counter   (写回内存)

// 可能的交错执行顺序（导致结果错误）：
//   A: LOAD counter(0)     B: LOAD counter(0)    ← 都读到 0
//   A: ADD  → 1            B: ADD  → 1
//   A: STORE counter=1     B: STORE counter=1    ← counter 应该是 2，实际是 1！
```

**三种同步原语各解决不同的问题：**

| 原语 | 解决的问题 | 现实类比 |
|------|-----------|---------|
| **互斥锁（mutex）** | 互斥访问：同一时刻只有一个线程进入临界区 | 厕所的门锁——一次一个人 |
| **信号量（semaphore）** | 控制资源数量：最多 N 个线程同时访问 | 停车场——只有 N 个车位 |
| **条件变量（cond）** | 等待条件：线程 A 干完活了通知线程 B | 餐厅叫号器——"第 42 号，您的菜好了" |

### 2. 条件变量的底层原理

条件变量最难理解，但它本质上是把三个操作打包成了**一个原子操作**：

```
"释放锁 + 睡觉 + (被叫醒后)重新拿锁"
```

**为什么必须是原子的？** 如果释放锁和睡觉不是原子的：

```
线程 A（消费者）:
  释放锁
     ← 线程 B（生产者）在这里拿到锁，broadcast，但线程 A 还没睡！
       线程 B 的 broadcast 发送到了一个还没开始等待的线程 → 丢失唤醒！
  睡觉
  → 线程 A 永远睡下去，没人来叫醒它 → 死锁
```

这就是为什么 `pthread_cond_wait` 需要传入**已经锁住的 mutex**——它内部会原子地完成释放+等待。

### 3. RAII 的哲学（Resource Acquisition Is Initialization）

本项目用类封装 pthread API 的核心思想是 RAII：

- **构造函数** = 获取资源（`sem_init` / `pthread_mutex_init`）
- **析构函数** = 释放资源（`sem_destroy` / `pthread_mutex_destroy`）
- **好处**：你永远不会忘记释放资源——C++ 编译器保证对象离开作用域时自动调用析构函数

---

## 实现指南

### Step 1：封装信号量（sem）

完整代码见 `src/locker.h`。

**核心数据结构：**

```cpp
class sem {
public:
    sem();           // 默认构造：初值 0（用于事件通知）
    sem(int num);    // 带参构造：初值 num（用于资源池）
    ~sem();          // 析构：销毁信号量
    bool wait();     // P 操作：等待（值减 1，值 < 0 时阻塞）
    bool post();     // V 操作：释放（值加 1，唤醒等待者）
private:
    sem_t m_sem;     // POSIX 信号量
};
```

**⚠️ 注意：**
- `sem_init` 的第二个参数为 0 表示不在进程间共享（本项目的线程都在同一进程内）
- 信号量与互斥锁的区别：信号量的 `post` 可以不在同一个线程中调用，但互斥锁的 `unlock` 必须在同一个线程

### Step 2：封装互斥锁（locker）

```cpp
class locker {
public:
    locker();
    ~locker();
    bool lock();
    bool unlock();
    pthread_mutex_t* get();  // ⚠️ 暴露原始指针供条件变量使用
private:
    pthread_mutex_t m_mutex;
};
```

**⚠️ `get()` 方法的设计取舍：**
条件变量的 `wait` 需要 `pthread_mutex_t*`。这里选择暴露内部指针而不是让 `cond` 持有 `locker` 引用——这是为了保持类的简洁和低耦合。代价是破坏了封装性，使用者需要遵守"拿到指针后只在 cond 中使用"的约定。

### Step 3：封装条件变量（cond）

```cpp
class cond {
public:
    cond();
    ~cond();
    bool wait(pthread_mutex_t* m_mutex);          // 无限等待
    bool timewait(pthread_mutex_t* m_mutex, struct timespec t); // 带超时
    bool signal();      // 唤醒一个
    bool broadcast();   // 唤醒全部
private:
    pthread_cond_t m_cond;
};
```

### Step 4：生产者-消费者模型验证

完整代码见 `src/test_producer_consumer.cpp`。核心模式：

```cpp
// === 生产者 ===
m_mutex.lock();
while (buffer_full) {              // ⚠️ while 不是 if！
    m_cond.wait(m_mutex.get());    // 释放锁 + 阻塞 + 被唤醒后重拿锁
}
produce();
m_cond.broadcast();                // 通知消费者
m_mutex.unlock();

// === 消费者 ===
m_mutex.lock();
while (buffer_empty) {
    // 检查终止条件（所有生产者都结束了）
    if (all_done) { m_mutex.unlock(); return; }
    m_cond.wait(m_mutex.get());
}
consume();
m_cond.broadcast();                // 通知生产者有空位了
m_mutex.unlock();
```

**⚠️ 为什么必须用 `while` 而不是 `if`？**

两个原因：
1. **虚假唤醒（Spurious Wakeup）**：POSIX 允许 `pthread_cond_wait` 在没有 `signal`/`broadcast` 的情况下返回（极少见，但标准允许）
2. **多个消费者竞争**：消费者 A 被唤醒后，消费者 B 也醒了并且抢先消费了最后一个数据。消费者 A 拿到锁后，发现队列又空了

### Step 5：gdb 多线程调试

```bash
gdb ./build/test_sync

# 查看所有线程
(gdb) info threads
  Id   Target Id         Frame
* 1    Thread ...         main ()
  2    Thread ...         producer ()
  3    Thread ...         consumer ()

# 切换到线程 2
(gdb) thread 2

# 查看所有线程的调用栈
(gdb) thread apply all bt

# 在所有线程的 locker::lock 设断点
(gdb) break locker::lock
(gdb) thread apply all break locker::lock
```

---

## 验证用例与预期结果

### 测试 1：编译运行

```bash
cd guide/phase_1/src
mkdir -p build && cd build
cmake ..
make
./test_sync
```

**预期：** 输出结束显示 `✅ PASS: All items consumed exactly once!`

### 测试 2：用 ThreadSanitizer 检查数据竞争

```bash
cd guide/phase_1/src
g++ -g -fsanitize=thread -pthread -o test_sync_tsan test_producer_consumer.cpp
./test_sync_tsan
```

**预期：** 无任何 "data race" 警告。如果出现，说明 lock 的封装有问题。

### 测试 3：多次运行验证确定性

```bash
for i in $(seq 1 10); do
    ./build/test_sync
    if [ $? -ne 0 ]; then
        echo "FAILED on run $i"
        break
    fi
done
echo "All 10 runs passed ✅"
```

### 失败排查

| 症状 | 可能原因 | 检查方法 |
|------|---------|---------|
| 程序永远不结束 | 死锁 | gdb attach，`thread apply all bt` 看每个线程卡在哪里 |
| `❌ FAIL` | 消费者提前退出或 count 计算错误 | 打印中间状态 |
| ThreadSanitizer 报 data race | 某处访问共享变量没加锁 | 看 TSan 报告的具体行号 |
| 输出顺序混乱 | 正常！多线程输出不保证顺序 | 每个线程的 produce/consume 逻辑应该是对的 |

---

## C++ 语法速查（本阶段涉及）

| 语法 | 示例 | 说明 |
|------|------|------|
| `class` | `class sem { ... };` | C++ 类定义（默认访问是 private） |
| `public:` / `private:` | `public: sem();` | 访问控制标签 |
| 构造函数 | `sem() { ... }` | 对象创建时自动调用 |
| 析构函数 | `~sem() { ... }` | 对象销毁时自动调用 |
| 成员初始化列表 | `sem() : val(0) { }` | 构造函数冒号后初始化成员（更高效） |
| `throw` | `throw std::exception()` | 抛出异常 |
| `void*` | `void* arg` | 通用指针（C 风格） |
| `static_cast<T*>` | `static_cast<int*>(arg)` | 安全的类型转换 |

---

## 阶段小结

你实现了：

- ✅ `sem` — POSIX 信号量的 C++ 封装
- ✅ `locker` — POSIX 互斥锁的 C++ 封装
- ✅ `cond` — POSIX 条件变量的 C++ 封装
- ✅ 生产者-消费者模型验证（2 生产者 + 3 消费者，无竞态）
- ✅ gdb 多线程调试基础（`info threads`、`thread apply all bt`）

**`locker.h` 将在后续每个阶段中使用。** 它是整个项目并发安全的地基。

下一阶段：**Phase 2 — 阻塞队列与日志系统**，基于本阶段的同步原语，构建线程安全的阻塞队列和单例日志系统。
