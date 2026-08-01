# C3 RAII 资源管理

> Part 1 第三章。**RAII**(Resource Acquisition Is Initialization,资源获取即初始化)是 C++ 最核心的工程思想——它是项目里锁封装 `locker.h` 和数据库连接池的实现基础。

## 1. 本课目标

- [ ] 理解栈对象生命周期与析构自动触发
- [ ] 理解"资源获取即初始化"的含义
- [ ] 会用 `std::unique_ptr` 和 `std::shared_ptr`
- [ ] 运行示例程序 + 完成 2 道练习

**铺路说明:** 项目里 `locker.h` 把互斥锁包进一个类,构造时 `lock()`、析构时 `unlock()`——这样锁**绝不会忘记释放**;`sql_connection_pool` 用一个连接守护对象,离开作用域自动把连接还回连接池。这就是 RAII 的实战。

## 2. 问题:资源必须手动释放

C 语言里,`malloc` 了就要 `free`,打开了文件就要 `close`。手动释放的问题:

```cpp
void process() {
    int *p = new int(42);        // 手动申请资源
    if (something_went_wrong()) {
        return;                  // ❌ 忘了 delete,内存泄漏!
    }
    delete p;                    // 正常路径才释放
}
```

只要中间有一个 `return`、一个异常,`delete` 就被跳过。**手动管理资源 = 容易漏。**

## 3. 解法:让析构函数负责释放

回想 C2 学过的:对象离开作用域,析构函数**自动**被调用。那把资源释放写进析构函数,不就永远不会漏了吗?

```cpp
class FileGuard {
public:
    FileGuard(const std::string &p) : path(p), opened(true) {
        // 构造:获取资源(打开文件)
    }
    ~FileGuard() {
        // 析构:释放资源(关闭文件)—— 无论怎么离开作用域都会执行!
    }
};
```

**这就是 RAII**:把资源的"获取"放在构造函数、"释放"放在析构函数。资源跟着对象走,对象死了资源就释放。

> **核心一句话**:让资源像栈上对象一样,自己管好自己的生死。

## 4. 智能指针

`new` / `delete` 太容易出错,C++11 标准库提供**智能指针**,替你做 `delete`。

### std::unique_ptr(独占指针)

```cpp
#include <memory>

{
    std::unique_ptr<int> up(new int(7));   // 拥有这块内存
    std::cout << *up;                        // 7
}   // ← 作用域结束,up 析构,自动 delete
```

- 同一个时刻只能有一个 `unique_ptr` 指向这块内存(独占)
- 不能复制,但可以 `std::move` 转移所有权

### std::shared_ptr(共享指针)

```cpp
{
    std::shared_ptr<int> sp1(new int(10));
    std::cout << sp1.use_count();          // 1

    {
        std::shared_ptr<int> sp2 = sp1;    // 复制 → 共享
        std::cout << sp1.use_count();      // 2
    }   // ← sp2 析构,计数减 1

    std::cout << sp1.use_count();          // 1
}   // ← sp1 析构,计数归 0,自动 delete
```

- 内部用**引用计数**:每复制一份计数 +1,每析构一份 -1
- 计数归 0 时,最后一个 `shared_ptr` 负责 `delete`
- 项目里 `sql_connection_pool` 就是用 `shared_ptr` 管理连接归还

## 5. 示例程序

在 `~/c3_demo` 下建 `demo.cpp`:

```cpp
#include <iostream>
#include <string>
#include <memory>

// 一个"资源守卫"类:构造时获取资源,析构时释放资源
class FileGuard {
private:
    std::string path;
    bool opened;
public:
    // 模拟打开文件(构造时获取资源)
    FileGuard(const std::string &p) : path(p), opened(true) {
        std::cout << "[打开] " << path << std::endl;
    }
    // 模拟读文件
    void read() const {
        if (!opened) {
            std::cout << "[错误] " << path << " 已关闭!" << std::endl;
            return;
        }
        std::cout << "[读取] " << path << " 内容..." << std::endl;
    }
    // 析构时释放资源(模拟关闭文件)
    ~FileGuard() {
        std::cout << "[关闭] " << path << std::endl;
    }
};

int main() {
    // 1. RAII:对象在栈上,离开作用域自动析构
    {
        FileGuard f("a.txt");
        f.read();
    }  // 这里 f 析构,自动关闭文件
    std::cout << "--- 第一个作用域结束,文件已自动关闭 ---" << std::endl;

    // 2. 智能指针 unique_ptr:自动 delete
    {
        std::unique_ptr<int> up(new int(7));
        std::cout << "unique_ptr 值: " << *up << std::endl;
    }  // up 析构,自动 delete

    // 3. shared_ptr:引用计数
    {
        std::shared_ptr<int> sp1(new int(10));
        std::cout << "引用计数: " << sp1.use_count() << std::endl;
        {
            std::shared_ptr<int> sp2 = sp1;  // 复制一份
            std::cout << "引用计数(复制后): " << sp1.use_count() << std::endl;
        }  // sp2 析构,计数减 1
        std::cout << "引用计数(sp2 销毁后): " << sp1.use_count() << std::endl;
    }  // sp1 析构,计数归 0,自动 delete

    return 0;
}
```

编译运行:

```bash
cd ~/c3_demo
g++ -std=c++11 -Wall -o demo demo.cpp
./demo
```

**预期输出:**

```text
[打开] a.txt
[读取] a.txt 内容...
[关闭] a.txt
--- 第一个作用域结束,文件已自动关闭 ---
unique_ptr 值: 7
引用计数: 1
引用计数(复制后): 2
引用计数(sp2 销毁后): 1
```

**看第 4 行的 `[关闭] a.txt`**:它出现在"作用域结束"打印的**前面**——因为 `f` 在大括号结束的那一刻就析构了,而后面的 `std::cout` 才执行。析构时机是精确的、确定的,这正是 RAII 可靠的原因。

## 6. 项目里的 RAII:预告

先留个印象,后面会看到两个典型应用:

```cpp
// locker.h 里锁的 RAII 封装(简化示意):
class LockGuard {
    pthread_mutex_t *m;
public:
    LockGuard(pthread_mutex_t *mtx) : m(mtx) { pthread_mutex_lock(m); }
    ~LockGuard() { pthread_mutex_unlock(m); }   // 永远记得解锁
};

// sql_connection_pool 里连接归还(简化示意):
class ConnectionGuard {
    ... 
public:
    ~ConnectionGuard() { pool->ReleaseConnection(conn); }  // 自动还回池子
};
```

> 现在看不懂没关系,记住"**构造拿资源、析构还资源**"这个模式,S3 和 S8 会用上。

## 7. 练习

**练习 1:RAII 计时器。** 写 `TimerGuard` 类,构造时记下开始时间,析构时打印耗时。用它测一个累加循环。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <ctime>

// RAII 计时器:构造开始计时,析构打印耗时
class TimerGuard {
private:
    clock_t start;
public:
    TimerGuard() : start(clock()) {}
    ~TimerGuard() {
        double sec = double(clock() - start) / CLOCKS_PER_SEC;
        std::cout << "耗时 " << sec << " 秒" << std::endl;
    }
};

int main() {
    TimerGuard tg;                       // 开始计时
    long sum = 0;
    for (long i = 0; i < 100000000L; i++) sum += i;
    std::cout << "sum=" << sum << std::endl;
    return 0;                            // tg 析构,打印耗时
}
```

预期输出(耗时值每次不同,但格式一致):

```text
sum=4999999950000000
耗时 0.14 秒
```

> 看到 `sum` 打印完、`main` 即将返回时,`tg` 自动析构并打印耗时了吗?这段"结束时要做的收尾工作"完全没写进 `main` 的逻辑——**析构函数替你做了**。

</details>

**练习 2:智能指针管理数组。** 用 `std::unique_ptr<int[]>` 管理一个动态数组,填充平方数并打印。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <memory>

int main() {
    std::unique_ptr<int[]> arr(new int[5]);   // 动态数组
    for (int i = 0; i < 5; i++) arr[i] = i * i;
    for (int i = 0; i < 5; i++) std::cout << arr[i] << " ";
    std::cout << std::endl;
    return 0;                                // arr 自动释放
}
```

预期输出:

```text
0 1 4 9 16
```

> 注意数组用的是 `unique_ptr<int[]>`(带 `[]`)。这行代码`new` 出来的内存全程没有一句 `delete`——由 `unique_ptr` 负责,这就是智能指针的意义。

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行示例程序 | 输出与 5 节预期完全一致(4 个代码块) | ☐ |
| 2 | 编译运行练习 1 | 打印 `sum=4999999950000000` 和"耗时 X 秒" | ☐ |
| 3 | 编译运行练习 2 | 输出 `0 1 4 9 16` | ☐ |
| 4 | 在示例 `main` 里写 `int *p = new int(1);` 不加 delete 再运行 | 程序照常结束,但这段内存泄漏(用 `valgrind` 可验证,可选) | ☐ |

## 9. 下一步

进入 **[C4 模板](cpp-04-template.md)**——`threadpool<T>`、`block_queue<T>` 都是类模板,这是并发和日志阶段的基石。
