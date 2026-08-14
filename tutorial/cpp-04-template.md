# C4 模板与泛型

> Part 1 第四章。模板让一段代码"对多种类型生效"。项目里的 `threadpool<T>`(线程池)和 `block_queue<T>`(阻塞队列)都是类模板——这一章是它们的前置。

## 1. 本课目标

- [ ] 理解"模板是让类型成为参数"
- [ ] 掌握函数模板写法
- [ ] 掌握类模板写法,并理解"为什么模板实现要放头文件"
- [ ] 运行示例程序 + 完成 1 道练习

**铺路说明:** Stage 3 的 `threadpool.h` 用 `template<typename T>` 让同一个线程池处理不同类型的任务;Stage 7 的 `block_queue.h` 用模板做通用队列。看懂模板,后面两处就不会卡。

## 2. 为什么需要模板

假设要一个"求最大值"函数,只支持 int:

```cpp
int my_max(int a, int b) {
    return (a > b) ? a : b;
}
```

要支持 double 呢?再写一个。要支持 string?又写一个。**为每种类型抄一遍代码 = 模板出现前的痛苦。**

模板把"类型"也变成参数:

```cpp
template <typename T>      // T 是"类型参数",调用时确定
T my_max(T a, T b) {
    return (a > b) ? a : b;
}
```

用的时候不需要写类型,编译器自动推断:

```cpp
my_max(3, 7);       // T 推断为 int
my_max(3.5, 1.2);   // T 推断为 double
```

> **核心:** 模板 = "一次编写,任意类型使用"。编译器根据你实际用到的类型,自动生成对应的版本。

## 3. 类模板

类和函数一样可以是模板。项目里 `threadpool<T>` 就是这个套路:

```cpp
template <typename T>
class MyQueue {
private:
    std::vector<T> data;        // 元素类型是 T
public:
    void push(const T &v) { data.push_back(v); }
    void pop() { data.erase(data.begin()); }
    T front() const { return data[0]; }
    bool empty() const { return data.empty(); }
    size_t size() const { return data.size(); }
};
```

用的时候**必须显式写出类型**(不像函数模板能自动推断):

```cpp
MyQueue<int> qi;             // 元素是 int 的队列
qi.push(10);

MyQueue<std::string> qs;     // 元素是 string 的队列
qs.push("hello");
```

同一个类,装 int 装 string 都行——**类模板让"容器"与"元素类型"解耦**。你已经在用两个现成的模板了:`std::vector<int>`、`std::vector<std::string>`——`vector` 就是标准库的类模板!

## 4. 为什么模板实现要放在头文件

普通类:声明放 `.h`,实现放 `.cpp`,链接时找到。**模板不行**——因为模板在"用到的那个翻译单元"里才被实例化,编译器必须看到完整定义才能生成代码。

所以**模板类的实现直接写在头文件里**(`.h` 里既有声明又有实现),项目里的 `threadpool.h`、`block_queue.h` 都是这样——整个文件就叫 `.h`,因为它本身就是模板实现。

```cpp
// myqueue.h —— 模板实现放头文件
#ifndef MYQUEUE_H
#define MYQUEUE_H
#include <vector>

template <typename T>
class MyQueue {
    std::vector<T> data;
public:
    void push(const T &v) { data.push_back(v); }
    // ... 完整实现
};

#endif
```

> `#ifndef ... #endif` 是**头文件守卫**:防止同一个头文件被 `#include` 两次导致重复定义。项目里所有头文件都有,后面看代码时认得出。

## 5. 示例程序

在 `~/c4_demo` 下建 `demo.cpp`:

```cpp
#include <iostream>
#include <string>
#include <vector>

// 类模板:模板参数 T 可以是任意类型
template <typename T>
class MyQueue {
private:
    std::vector<T> data;      // 内部用 vector 存数据
public:
    void push(const T &v) { data.push_back(v); }
    void pop() { data.erase(data.begin()); }   // 移除第一个
    T front() const { return data[0]; }
    bool empty() const { return data.empty(); }
    size_t size() const { return data.size(); }
};

// 函数模板
template <typename T>
T my_max(T a, T b) {
    return (a > b) ? a : b;
}

int main() {
    // 用 int 实例化
    MyQueue<int> qi;
    qi.push(10);
    qi.push(20);
    std::cout << "int 队列 front=" << qi.front() << " size=" << qi.size() << std::endl;

    // 用 string 实例化
    MyQueue<std::string> qs;
    qs.push("hello");
    qs.push("world");
    std::cout << "string 队列 front=" << qs.front() << " size=" << qs.size() << std::endl;
    qs.pop();
    std::cout << "pop 后 front=" << qs.front() << std::endl;

    // 函数模板
    std::cout << "max(3,7)=" << my_max(3, 7) << std::endl;
    std::cout << "max(3.5,1.2)=" << my_max(3.5, 1.2) << std::endl;

    return 0;
}
```

编译运行:

```bash
cd ~/c4_demo
g++ -std=c++11 -Wall -o demo demo.cpp
./demo
```

**预期输出:**

```text
int 队列 front=10 size=2
string 队列 front=hello size=2
pop 后 front=world
max(3,7)=7
max(3.5,1.2)=3.5
```

注意:同一个 `MyQueue` 类,既装了 `int` 又装了 `string`,行为完全一样——这就是模板的价值。

## 6. 项目里的模板:预告

```cpp
// threadpool.h 里的类模板(贴近真实,Stage 3 会讲;原版 append 还带第二个参数 int state,
// 这里先看主干)
template <typename T>
class threadpool {
public:
    bool append(T *request);       // 往任务队列里放任务
    ...
private:
    std::list<T *> work_queue;     // 任务队列,元素是 T*
};

// 使用时,传入任务类型,例如
// threadpool<http_conn> pool(8);   ← 8 个线程处理 http_conn 类型的任务
```

> 现在只需要知道:`threadpool<http_conn>` 就是"能处理 http_conn 任务的线程池"。`<T>` 这个尖括号,后面你会天天见。

## 7. 练习

**练习:写 `my_swap` 函数模板。** 交换两个同类型的值,要求 int 和 string 都能用。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <string>

template <typename T>
void my_swap(T &a, T &b) {
    T t = a;
    a = b;
    b = t;
}

int main() {
    int x = 1, y = 2;
    my_swap(x, y);
    std::cout << "x=" << x << " y=" << y << std::endl;        // x=2 y=1

    std::string s1 = "hello", s2 = "world";
    my_swap(s1, s2);
    std::cout << s1 << " " << s2 << std::endl;                // world hello
    return 0;
}
```

预期输出:

```text
x=2 y=1
world hello
```

> 注意:参数用了引用 `T &`,否则交换的只是拷贝。可以试试去掉 `&`,看两个输出会不会变。

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行示例程序 | 输出与 5 节预期完全一致 | ☐ |
| 2 | 编译运行练习 | `x=2 y=1` 和 `world hello` | ☐ |
| 3 | 在示例里加一行 `MyQueue<char> qc; qc.push('a');` 并打印 | 能编译运行 | ☐ |
| 4 | 把练习的 `T &` 去掉 `&` 再编译运行 | 交换失败(输出不变),理解引用的必要性 | ☐ |

## 9. 下一步

进入 **[C5 线程与同步](cpp-05-thread.md)**——并发编程的三大件:线程、互斥锁、条件变量。项目从 Stage 3 开始全面并发。
