# C5 线程与同步

> Part 1 第五章。并发编程的三件套:线程、互斥锁、条件变量。项目从 Stage 3 起全面并发——线程池、日志、信号处理都离不开这一章。

## 1. 本课目标

- [ ] 会用 `std::thread` 创建线程、`join` 等待
- [ ] 理解数据竞争,会用 `mutex` / `lock_guard` 保护共享数据
- [ ] 会用 `condition_variable` 实现生产者/消费者
- [ ] 认识 POSIX 信号量 `sem_t`(项目自己封装的锁用它)
- [ ] 运行 3 个示例程序 + 完成 1 道练习

**铺路说明:** 项目 `lock/locker.h` 把 `pthread_mutex_t`、`pthread_cond_t`、`sem_t` 各封装成类(RAII);`threadpool.h` 里 `sem_wait` 用来等任务队列有活干;`log` 的异步日志就是生产者/消费者。这一章全部是前置。

## 2. 线程:创建与等待

`std::thread` 创建线程,C++11 标准库:

```cpp
#include <thread>

void worker(int id) {
    std::cout << "线程 " << id << " 在工作" << std::endl;
}

int main() {
    std::thread t(worker, 7);   // 创建线程,参数 7 传给 worker
    t.join();                   // 等待线程 t 结束
    return 0;
}
```

- `std::thread t(func, args...)`:新开一个线程,从 `func` 开始执行
- `t.join()`:当前线程**阻塞等待** t 结束。忘记 join 可能导致程序异常退出
- 传参:直接写在函数名后面,`std::thread t(worker, 7)` 等价于调用 `worker(7)`

> 编译多线程程序必须加链接参数:`g++ -std=c++11 -pthread ...`。`-pthread` 链接 pthread 库(Stage 0 的 hello 不需要,但从这里开始都需要)。

## 3. 数据竞争与互斥锁

### 问题:共享数据被同时改

两个线程同时对同一个变量 `counter++`:

```cpp
int counter = 0;
void worker() {
    for (int i = 0; i < 100000; i++) {
        counter++;       // 三个动作:读 counter → 加 1 → 写回
    }
}
// 两个线程同时跑 worker
```

`counter++` 在底层是"读、加、写"三步。两个线程可能同时"读到同一个值,各自加 1 再写回"——于是一次更新被覆盖,**结果偏小**。这叫**数据竞争(Data Race)**。

### 加锁保护

互斥锁保证**同一时刻只有一个线程**能进入临界区:

```cpp
#include <mutex>

std::mutex mtx;              // 互斥锁
int counter = 0;

void worker() {
    for (int i = 0; i < 100000; i++) {
        mtx.lock();          // 拿锁:没抢到就阻塞等待
        counter++;           // 临界区:此刻只有我一个线程
        mtx.unlock();        // 放锁:放走下一个
    }
}
```

### 推荐写法:lock_guard(RAII 再登场)

手写 `lock/unlock` 有个老问题:临界区里 `return` 或抛异常就忘了 `unlock`。用 RAII 包装:

```cpp
void worker() {
    for (int i = 0; i < 100000; i++) {
        std::lock_guard<std::mutex> guard(mtx);  // 构造时 lock
        counter++;                               // 出作用域自动 unlock
    }
}
```

> 这就是 C3 学的 RAII:`lock_guard` 构造拿锁、析构放锁——**锁永远不会忘放**。项目里的 `locker.h` 就是给 `pthread_mutex_t` 做的同一个封装。

## 4. 条件变量:等待某个条件

锁解决"互斥",但有时线程需要**等待某个条件成立**(比如"队列里要有数据才能取")。条件变量就是干这个的:

```cpp
#include <condition_variable>

std::mutex mtx;
std::condition_variable cv;
std::queue<int> q;

void consumer() {
    std::unique_lock<std::mutex> lk(mtx);       // 必须是 unique_lock
    cv.wait(lk, []{ return !q.empty(); });      // 条件不满足就睡,满足才继续
    int v = q.front();
    q.pop();
}
```

- `cv.wait(lk, 条件)`:先释放锁、睡下去;被唤醒后重新抢锁、再检查条件。**条件为假就继续睡**,防止"假唤醒"
- `cv.notify_one()`:唤醒一个等在 `cv` 上的线程
- `std::unique_lock`:和 `lock_guard` 功能相同,但**允许中途 unlock**(wait 需要),所以这里必须用它

> **为什么必须用 unique_lock?** `wait` 要让出锁、睡、醒了再拿锁——这一套"释放+重拿"只有 `unique_lock` 支持。`lock_guard` 只能锁到底。

这就是**生产者/消费者模型**:一个线程往队列放,一个线程从队列取,条件变量让消费者"没有货就睡,有货被叫醒"。

## 5. 信号量(POSIX sem)

项目没用 C++ 标准库的信号量,而是用了 Linux 原生的 `sem_t`(在 `semaphore.h` 里)。它是个**计数器**:计数 > 0 就能继续,等于 0 就阻塞。

```cpp
#include <semaphore.h>

sem_t sem;
sem_init(&sem, 0, 2);      // 初始计数值 2

sem_wait(&sem);            // P 操作:计数-1,若为 0 则阻塞等待
// 临界区……
sem_post(&sem);            // V 操作:计数+1,唤醒等待者

sem_destroy(&sem);         // 销毁
```

信号量和互斥锁的区别:

| | 互斥锁 mutex | 信号量 sem |
|---|---|---|
| 本质 | 一把锁,只能一个线程进 | 一个计数器,允许多个(计数上限内) |
| 典型用途 | 保护一份共享数据 | 控制"允许同时访问的资源数量" |

> 项目 `threadpool.h` 里:`sem_wait` 用来"等任务队列里有任务",`sem_post` 在放入任务后通知——信号量当"任务计数器"用。S3 会看到真实代码。

## 6. 示例程序

三个程序,在 `~/c5_demo` 下分别建文件。

### 示例 1:互斥锁累加计数器(`demo1.cpp`)

```cpp
#include <iostream>
#include <thread>
#include <mutex>

std::mutex mtx;
int counter = 0;
const int TIMES = 100000;

void worker(int id) {
    for (int i = 0; i < TIMES; i++) {
        mtx.lock();        // 进入临界区
        counter++;         // 需要保护的操作
        mtx.unlock();      // 离开临界区
    }
}

int main() {
    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    t1.join();
    t2.join();
    std::cout << "counter = " << counter << " (期望 " << 2 * TIMES << ")" << std::endl;
    return 0;
}
```

```bash
g++ -std=c++11 -Wall -pthread -o demo1 demo1.cpp
./demo1
```

**预期输出:**

```text
counter = 200000 (期望 200000)
```

### 示例 2:生产者 / 消费者(`demo2.cpp`)

```cpp
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>

std::mutex mtx;
std::condition_variable cv;
std::queue<int> q;
const int TOTAL = 10;

void producer() {
    for (int i = 1; i <= TOTAL; i++) {
        std::unique_lock<std::mutex> lk(mtx);
        q.push(i);
        std::cout << "[生产] " << i << std::endl;
        cv.notify_one();       // 通知等待的消费者
    }
}

void consumer() {
    int count = 0;
    while (count < TOTAL) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, []{ return !q.empty(); });   // 等队列非空
        int v = q.front();
        q.pop();
        count++;
        std::cout << "[消费] " << v << std::endl;
    }
}

int main() {
    std::thread p(producer);
    std::thread c(consumer);
    p.join();
    c.join();
    std::cout << "=== 消费完成, 共 " << TOTAL << " 个 ===" << std::endl;
    return 0;
}
```

```bash
g++ -std=c++11 -Wall -pthread -o demo2 demo2.cpp
./demo2
```

**预期输出(生产/消费行的先后可能不同,但必须凑齐 10 个):**

```text
[生产] 1
[生产] 2
...
[生产] 10
[消费] 1
...
[消费] 10
=== 消费完成, 共 10 个 ===
```

**为什么消费行可能插在生产行中间?** 两个线程真的在并行执行,谁先跑是操作系统调度的,不确定。但**消费者绝不会早于生产者取到货**——因为队列空时 `cv.wait` 会睡觉。这就是"等待条件"的意义。

### 示例 3:信号量限流(`demo3.cpp`)

```cpp
#include <iostream>
#include <thread>
#include <semaphore.h>

sem_t sem;                     // 信号量
const int N = 5;

void worker(int id) {
    sem_wait(&sem);            // P 操作:拿一个信号量,拿不到就阻塞等待
    std::cout << "线程 " << id << " 进入临界区" << std::endl;
    sem_post(&sem);            // V 操作:归还一个信号量
}

int main() {
    sem_init(&sem, 0, 2);      // 初始信号量值为 2(最多 2 个线程同时进入)
    std::thread ts[N];
    for (int i = 0; i < N; i++) ts[i] = std::thread(worker, i);
    for (int i = 0; i < N; i++) ts[i].join();
    sem_destroy(&sem);
    std::cout << "=== 全部线程结束 ===" << std::endl;
    return 0;
}
```

```bash
g++ -std=c++11 -Wall -pthread -o demo3 demo3.cpp
./demo3
```

**预期输出(进入顺序不定,但 5 个线程全部执行完):**

```text
线程 0 进入临界区
线程 2 进入临界区
线程 1 进入临界区
线程 3 进入临界区
线程 4 进入临界区
=== 全部线程结束 ===
```

信号量初始值 2,意味着最多同时放行 2 个;其余线程在 `sem_wait` 处排队,前面的 `sem_post` 才放它们进来。

## 7. 练习

**练习:加锁累加。** 两个线程各对共享 `counter` 累加 50000 次,要求用 `std::lock_guard`,最终输出必须等于 100000。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <thread>
#include <mutex>

std::mutex mtx;
int counter = 0;
const int TIMES = 50000;

void worker() {
    for (int i = 0; i < TIMES; i++) {
        std::lock_guard<std::mutex> guard(mtx);  // 构造上锁,析构自动解锁
        counter++;
    }
}

int main() {
    std::thread t1(worker), t2(worker);
    t1.join(); t2.join();
    std::cout << "counter = " << counter << " (期望 " << 2 * TIMES << ")" << std::endl;
    return 0;
}
```

预期输出:

```text
counter = 100000 (期望 100000)
```

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行 demo1 | `counter = 200000` | ☐ |
| 2 | 编译运行 demo2 | 10 个生产 + 10 个消费 + 结束提示 | ☐ |
| 3 | 编译运行 demo3 | 5 个线程全部进入 + 结束提示,程序不卡死 | ☐ |
| 4 | 编译运行练习 | `counter = 100000` | ☐ |
| 5 | **破坏性实验**:把 demo1 的 `mtx.lock()/unlock()` 注释掉,用 `-O0` 编译跑几次 | 输出不再是 200000,而是某个偏小的数 | ☐ |

> **第 5 条是本章最有价值的实验。** 去掉锁 = 数据竞争 = 未定义行为。我实测时:用 `-O0` 编译,40 次里有 40 次结果偏小(比如 2145854);但用 `-O2` 编译,40 次又全部碰巧正确。**同一个 bug,换个优化选项表现完全不同**——这就是数据竞争最可怕的地方:错的时候难以复现,对的时候你以为很安全。它靠的全是运气,而正确性永远不能靠运气。

## 9. 下一步

进入 **[C6 现代特性](cpp-06-modern.md)**——补齐项目代码里常见的 C++11 写法,nullptr/auto/范围 for/lambda。
