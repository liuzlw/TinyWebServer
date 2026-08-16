# Stage 1：C++ 快速上手

本阶段是一堂「为读懂本项目而生」的最小 C++ 语法课：只讲 `TinyWebServer` 源码里真正用到的那部分，不追求面面俱到。学完本阶段，你要能用 C++ 写出并编译运行 12 个贴近项目场景的迷你练习——从引用、`new/delete`、STL 容器，到类、模板、异常、RAII、C 风格字符串解析——从而在进入 Stage 2 写 socket 服务器之前，先把「看代码不犯怵」的基本功补上。

> **重要约定：本项目源码是 C++98/03 风格。** 源码里**不用** `auto`、智能指针（`shared_ptr`/`unique_ptr`）、范围 `for`（`for (x : vec)`）、`nullptr` 等 C++11 及以后的新特性；用 `NULL`、`new`/`delete`、原始指针、`for (int i = 0; ...)`。为了让你能无缝对照源码，**本教程的所有代码也保持这一风格**。个别地方会顺带提一句「C++11 之后可以怎么写」，你了解即可，不要混用。

## 前置要求

- 已完成 [Stage 0：环境准备](stage-00-environment.md)：能用 `g++` 编译运行 hello world、会进 gdb。
- 工作区 `my_tiny_webserver/` 已建好（与仓库 `TinyWebServer/` 同级）。
- 本阶段不需要网络、MySQL 相关知识；用到的源码片段都来自本仓库，只读对照即可。

## 理论学习

本阶段没有新的协议或 OS 原理，它的「理论」就是**编程模型**：程序是一组数据 + 操作数据的函数；C++ 通过「类」把数据和函数捆在一起（封装），通过「模板」让同一份代码适配不同类型（泛化），通过「RAII」让对象的生命周期自动管理资源（栈上对象离开作用域必被析构）。这三件事贯穿整个项目——`http_conn` 是类，`threadpool<T>`/`block_queue<T>` 是类模板，`connectionRAII`/`locker` 是 RAII 的活例子。

```text
        你的 C++ 程序
   ┌─────────────────────────┐
   │  数据（变量/成员）          │
   │  ├─ 基本类型 int/char/...  │
   │  ├─ 栈上变量 / 堆上 new     │
   │  └─ STL 容器 string/vector/ │
   │        map/list            │
   │                            │
   │  函数（操作数据）            │
   │  ├─ 自由函数（重载/默认参数） │
   │  ├─ 成员函数（封装 + this）  │
   │  └─ 模板（一份代码适配多类型） │
   └─────────────────────────┘
   用「类 + RAII」组织起来，就是本项目源码的样子
```

## 本阶段 C++ 知识点

下面 12 个知识点，每个先讲概念、再贴本项目真实源码（节选），最后指出对应的动手练习。练习文件全部放在 `my_tiny_webserver/stage01/`。

### 1. 程序结构与编译流程回顾

一个 C++ 程序的最小结构：`#include` 引入头文件 → `using namespace std;` 免写 `std::` → `main` 函数作为入口 → `return 0;` 表示正常结束。函数可以先「声明」后「定义」，这样 `main` 能写在文件靠前的位置。本项目 `main.cpp` 就是这个结构：

```cpp
// main.cpp（节选）
#include "config.h"                 // 引入自定义头文件

int main(int argc, char *argv[])    // 带命令行参数的入口
{
    Config config;
    config.parse_arg(argc, argv);   // 解析命令行
    // ...
    return 0;
}
```

编译四步（预处理/编译/汇编/链接）在 Stage 0 已演示，本阶段所有练习都用 `g++ 文件名.cpp -o 文件名` 一条命令完成。

> 练习：`ex01_structure.cpp`（函数声明与定义分离）。

### 2. 基本类型、const、引用（引用 vs 指针）

常用基本类型：`int`/`long`/`char`/`bool`/`double`，以及无符号 `unsigned`（socket 描述符、缓冲区大小常用 `unsigned`/`size_t`）。`const` 表示「只读，不许改」：`const int MAX_FD` 在编译期就挡住误改。

**引用（`&`）是「别名」，指针（`*`）是「存地址」**。引用一旦绑定就不能再指向别处，且不能为空；指针可以指向任意地址、可以为 `NULL`。做函数参数时，引用不用拷贝原对象，适合传大对象；`const` 引用表示「只读地传，不拷贝」。项目里大量这样用：

```cpp
// http_conn.h（节选）
void init(int sockfd, const sockaddr_in &addr, char *, int, int,
          string user, string passwd, string sqlname);
```

这里 `const sockaddr_in &addr` 就是「传入一个地址结构体的只读引用」，避免整块拷贝。

> 练习：`ex02_ref.cpp`。

### 3. 函数：默认参数、重载、内联

- **默认参数**：调用时可省略尾部参数，省略时用默认值。项目 `http_conn.h` 里 `void close_conn(bool real_close = true);`，调用 `close_conn()` 就等于 `close_conn(true)`。
- **重载**：同名函数、参数列表不同，编译器按实参类型挑选。项目 `locker.h` 的 `sem` 类里，`sem()` 和 `sem(int num)` 就是构造函数重载（信号量初值不同）。
- **内联 `inline`**：把函数体「展开」到调用处，省去函数调用开销，适合极短函数。项目 `http_conn.h` 里 `char *get_line() { return m_read_buf + m_start_line; }` 就是内联。

> 练习：`ex03_func.cpp`。

### 4. new/delete 与内存模型（栈/堆）

内存分几块：**栈**（函数局部变量，自动分配回收，快但空间小）、**堆**（`new` 手动申请、`delete` 手动释放，慢但大）、**全局/静态区**（全局变量、静态成员）。本项目为 `MAX_FD`（最大文件描述符数）准备的数组、线程池的线程数组、阻塞队列的底层数组，都在堆上用 `new[]` 分配：

```cpp
// threadpool.h（节选）
m_threads = new pthread_t[m_thread_number];   // 申请数组
// ...
delete[] m_threads;                           // 数组必须用 delete[]
```

```cpp
// block_queue.h（节选）
m_array = new T[max_size];                    // 模板类型数组
```

`new` 配 `delete`，`new[]` 配 `delete[]`，千万别混用；释放后把指针置 `NULL` 避免悬空指针。

> 练习：`ex04_new.cpp`。

### 5. string / vector / map 三个 STL 容器

- `string`：可动态增长的字符串，比 `char[]` 安全省心。项目 `main.cpp` 里 `string user = "root";`。
- `vector`：动态数组，`push_back` 追加、`size()` 取个数、`[i]` 随机访问。
- `map`：键值对（字典），`[key]` 读写、`find(key)` 判断是否存在。**这是注册登录校验的核心**，项目 `http_conn.cpp` 全局有一个 `map<string, string> users;`，把数据库里的用户名密码装进去，然后：

```cpp
// http_conn.cpp（节选，注册校验：用户名已存在则拒绝）
if (users.find(name) == users.end())    // 找不到 = 可以注册
// http_conn.cpp（节选，登录校验：存在且密码一致才通过）
if (users.find(name) != users.end() && users[name] == password)
```

`find` 返回一个「迭代器」，找不到时等于 `end()`，所以用 `== users.end()` 判断「不存在」。

> 练习：`ex05_stl.cpp`。

### 6. 类与封装（构造/析构/拷贝构造/this/静态成员）

类把数据和操作它的函数捆在一起：`public` 是对外接口，`private` 是内部实现。四个常客：

- **构造函数**：对象创建时自动调用，负责初始化；
- **析构函数**（`~类名`）：对象销毁时自动调用，负责清理；
- **拷贝构造函数**（`类名(const 类名 &)`）：用一个已有对象「复制」出新的同型对象；
- **`this` 指针**：成员函数内部指向「当前对象」的指针。

**静态成员**属于整个类而非某个对象，所有对象共享一份，用 `类名::成员` 访问。项目 `http_conn.h` 用它统计连接数、共享 epoll 句柄：

```cpp
// http_conn.h（节选）
public:
    static int m_epollfd;    // 所有连接共享同一个 epoll 句柄
    static int m_user_count; // 当前连接总数
```

> 练习：`ex06_class.cpp`（类内静态成员计数）。

### 7. 模板（函数模板 + 类模板）

模板让「一份代码」适配「多种类型」，编译时由编译器按你实际用的类型「生成」具体版本。函数模板如 `template <typename T> T bigger(T a, T b)`；类模板如项目的线程池和阻塞队列：

```cpp
// threadpool.h（节选）
template <typename T>
class threadpool { /* ... */ };

// block_queue.h（节选）
template <class T>
class block_queue { /* ... */ };
```

`threadpool<http_conn>` 和 `threadpool<其他类型>` 就是编译器用同一份模板生成的两个不同类。注意：**类模板的成员函数通常要和类写在同一个头文件里**（编译器要看到完整定义才能实例化），这也是 `threadpool.h` 把实现都写在头文件里的原因。

> 练习：`ex07_template.cpp`（mini 模板栈）。

### 8. 异常（throw / try-catch）

出错时用 `throw` 抛出异常，调用方用 `try-catch` 接住，避免程序直接崩。项目里习惯「失败就 `throw std::exception()`」，例如 `locker.h` 初始化互斥锁失败时：

```cpp
// locker.h（节选）
locker()
{
    if (pthread_mutex_init(&m_mutex, NULL) != 0)
    {
        throw std::exception();   // 初始化失败：抛异常
    }
}
```

> 练习：`ex08_exception.cpp`。

### 9. RAII 思想（用对象生命周期管理资源）

RAII =「资源获取即初始化」：把「获取资源」写在构造函数里，把「释放资源」写在析构函数里；因为栈上对象**离开作用域时析构函数一定会被调用**，资源就绝不会漏释放——即使函数提前 `return` 或抛异常。项目最典型的例子是数据库连接：

```cpp
// sql_connection_pool.h（节选）
class connectionRAII {
public:
    connectionRAII(MYSQL **con, connection_pool *connPool); // 取一条连接
    ~connectionRAII();                                      // 自动归还连接
};
```

线程池 `run()` 里 `connectionRAII mysqlcon(&request->mysql, m_connPool);` 声明一个局部对象，函数走完这条连接自动还给连接池，不用手动配对「取/还」。`locker`/`sem` 用构造函数初始化 pthread 资源、析构函数销毁资源，也是同样的思想。

> 练习：`ex09_raii.cpp`（RAII 计时器 + RAII 锁包装）。

### 10. 头文件保护 #ifndef 与多文件编译

多个 `.cpp` 可能重复 `#include` 同一个头文件，若头文件里定义了类，重复定义会报错。解决办法是**头文件保护**：

```cpp
// locker.h 开头（节选）
#ifndef LOCKER_H
#define LOCKER_H
// ... 类定义 ...
#endif
```

这样同一个头文件被第二次包含时，`LOCKER_H` 已定义，中间内容被整段跳过。项目里每个头文件都有：`THREADPOOL_H`、`HTTPCONNECTION_H`、`BLOCK_QUEUE_H` 等。多文件项目分开编译再链接：

```bash
g++ main.cpp math_utils.cpp -o multi
```

> 练习：`ex10/`（三文件：`math_utils.h` + `math_utils.cpp` + `main.cpp`）。

### 11. C 风格字符串函数族

项目解析 HTTP 请求时大量操作 `char[]` 而非 `string`，用的是一组 `<cstring>` 函数。先记住一个核心事实：**C 字符串以 `'\0'`（ASCII 0）结尾**，`strlen` 数到 `'\0'` 为止。本项目 `http_conn.cpp` 的 `parse_request_line` 就是它们的实战舞台：

```cpp
// http_conn.cpp（节选）
m_url = strpbrk(text, " \t");        // 找第一个空格/制表符
if (!m_url) return BAD_REQUEST;
*m_url++ = '\0';                     // 用 '\0' 截断出 method
m_url += strspn(m_url, " \t");       // 跳过空白
m_version = strpbrk(m_url, " \t");   // 再找 URL 末尾
// ...
if (strncasecmp(m_url, "http://", 7) == 0)  // 忽略大小写比较前缀
    m_url += 7;
    m_url = strchr(m_url, '/');      // 找 '/' 首次出现
```

常用函数速记：

| 函数 | 作用 |
|---|---|
| `strlen(s)` | 求长度（不含结尾 `'\0'`） |
| `strcpy(d, s)` / `strncpy(d, s, n)` | 复制字符串（后者限长） |
| `strcat(d, s)` | 把 `s` 拼接到 `d` 末尾 |
| `strcmp(a, b)` / `strcasecmp(a, b)` | 比较（后者忽略大小写） |
| `strncasecmp(a, b, n)` | 忽略大小写比较前 n 个字符 |
| `strpbrk(s, set)` | 找 `set` 中任意字符首次出现的位置 |
| `strspn(s, set)` | 返回开头连续属于 `set` 的字符个数 |
| `strchr(s, c)` / `strrchr(s, c)` | 找字符 `c` 首次 / 最后一次出现 |
| `snprintf(buf, n, fmt, ...)` | 把格式化结果写入缓冲区 |
| `memset(p, v, n)` | 把 n 字节内存都设为 v（常用于清零） |

> 练习：`ex11_cstring.cpp`（解析一行请求）。

### 12. printf / 格式串

`printf`（及写缓冲区的 `snprintf`、变参版 `vsnprintf`）用「格式串 + 占位符」拼字符串，是日志系统的基石。项目 `log.cpp` 写日志行、拼日志文件名全靠它：

```cpp
// log.cpp（节选）
snprintf(log_full_name, 255, "%d_%02d_%02d_%s", year, mon, day, file_name);
// ...
int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                 year, mon, day, hour, min, sec, usec, log_style);
int m = vsnprintf(m_buf + n, m_log_buf_size - n - 1, format, valst);
```

常用占位符：`%d` 整数、`%s` 字符串、`%c` 字符、`%f` 浮点、`%p` 指针地址、`%x` 十六进制、`%%` 百分号本身；`%02d` 表示整数右对齐占 2 位、不足补 0（生成 `01`、`09` 这类）。

> 练习：`ex12_printf.cpp`。

## 动手实现

在 Ubuntu 终端建立目录：

```bash
mkdir -p ~/projects/my_tiny_webserver/stage01
cd ~/projects/my_tiny_webserver/stage01
```

下面 12 个练习按编号创建文件（文件内容均为**完整可编译**代码，直接照抄再编译）。建议逐字手敲，不要复制粘贴。

### 练习 1：程序结构 `ex01_structure.cpp`

```cpp
#include <iostream>
using namespace std;

// 前置声明：告诉编译器后面会用到 square
int square(int n);

int main()
{
    int n = 9;
    cout << n << " 的平方是 " << square(n) << endl;
    return 0;
}

// 函数定义：真正的实现
int square(int n)
{
    return n * n;
}
```

关键行：第 5 行是「声明」，第 15～18 行是「定义」；`main` 里调用 `square(n)` 时，编译器已通过声明知道它的存在。

### 练习 2：类型/const/引用 `ex02_ref.cpp`

```cpp
#include <iostream>
using namespace std;

// 引用参数：不拷贝，直接改原变量（等价于传入「别名」）
void add_one(int &x)
{
    x = x + 1;
}

// const 引用：只读传入，避免拷贝大对象
void show(const int &x)
{
    cout << "value = " << x << endl;
    // x = 0; // 取消注释会编译报错：不能修改 const 引用
}

int main()
{
    const int MAX_FD = 10000;  // 本项目 webserver.cpp 有 const int MAX_FD = 65536
    int a = 10;

    int &ref = a;   // ref 是 a 的别名
    ref = 20;
    cout << "a = " << a << endl;   // 20

    int *p = &a;    // p 是指针，存 a 的地址
    *p = 30;        // 解引用，改 a
    cout << "a = " << a << endl;   // 30

    add_one(a);
    cout << "a = " << a << endl;   // 31

    show(a);
    cout << "MAX_FD = " << MAX_FD << endl;
    return 0;
}
```

关键行：第 5 行 `int &x` 让 `add_one` 直接改原变量；第 11 行 `const int &x` 只读传参；第 22 行 `int &ref = a` 说明引用必须在定义时就绑定。

### 练习 3：函数 `ex03_func.cpp`

```cpp
#include <iostream>
using namespace std;

// 默认参数：delay 不传时用 0（本项目 close_conn(bool real_close = true) 同理）
void log_msg(const char *msg, int level = 0)
{
    cout << "[level " << level << "] " << msg << endl;
}

// 重载：同名函数，参数类型不同
int twice(int x)        { return x * 2; }
double twice(double x)  { return x * 2.0; }

// 内联：短函数展开到调用处（本项目 http_conn.h 的 get_line() 同理）
inline int add(int a, int b)
{
    return a + b;
}

int main()
{
    log_msg("server started");     // level 用默认值 0
    log_msg("bad request", 3);     // level 显式传 3

    cout << twice(3) << endl;      // 调用 int 版本
    cout << twice(3.5) << endl;    // 调用 double 版本

    cout << add(1, 2) << endl;
    return 0;
}
```

关键行：第 5 行 `int level = 0` 是默认参数；第 11～12 行是重载；第 15 行 `inline` 是内联。

### 练习 4：new/delete 与内存模型 `ex04_new.cpp`

```cpp
#include <iostream>
using namespace std;

int global_var = 1;   // 全局/静态区

int main()
{
    int stack_var = 2; // 栈

    // 堆：new 一个数组（本项目 threadpool.h 里 new pthread_t[n] 同理）
    int n = 5;
    int *heap_arr = new int[n];
    for (int i = 0; i < n; ++i)
        heap_arr[i] = i * 10;

    cout << "global_var 地址: " << &global_var << endl;
    cout << "stack_var  地址: " << &stack_var << endl;
    cout << "heap_arr   地址: " << heap_arr << endl;

    for (int i = 0; i < n; ++i)
        cout << "heap_arr[" << i << "] = " << heap_arr[i] << endl;

    delete[] heap_arr;  // 数组必须用 delete[]
    heap_arr = NULL;    // 置空，避免悬空指针

    // 单个对象用 new / delete
    int *single = new int(42);
    cout << "*single = " << *single << endl;
    delete single;

    return 0;
}
```

关键行：第 12 行 `new int[n]` 在堆上申请数组，第 23 行 `delete[]` 释放；第 27 行 `new int(42)` 申请单个对象并初始化为 42。三处地址打印出来会落在不同区间，直观感受栈/堆/全局区。

### 练习 5：string/vector/map `ex05_stl.cpp`

```cpp
#include <iostream>
#include <string>
#include <vector>
#include <map>
using namespace std;

int main()
{
    // string：本项目 main.cpp 里 user/passwd 就是 string
    string user = "root";
    string passwd = "root";
    cout << "user = " << user << ", passwd = " << passwd << endl;
    cout << "user 长度 = " << user.size() << endl;

    // vector：动态数组
    vector<int> ports;
    ports.push_back(9006);
    ports.push_back(9007);
    ports.push_back(9008);
    for (size_t i = 0; i < ports.size(); ++i)
        cout << "port[" << i << "] = " << ports[i] << endl;

    // map：键值对，本项目 http_conn.cpp 用 map<string,string> 做注册登录校验
    map<string, string> users;
    users["qinyi"] = "123456";
    users["root"]  = "root";

    // find 判断「是否存在」——注册时判断用户名是否已被占用
    string name = "qinyi";
    if (users.find(name) == users.end())
        cout << name << " 不存在（可以注册）" << endl;
    else
        cout << name << " 已存在（不能重复注册）" << endl;

    // [] 取值——登录时校验密码
    string pwd = "123456";
    if (users.find(name) != users.end() && users[name] == pwd)
        cout << "登录成功" << endl;
    else
        cout << "用户名或密码错误" << endl;

    return 0;
}
```

关键行：第 25～26 行用 `[]` 写入键值；第 30 行 `find(...) == end()` 判断键不存在；第 37 行 `users[name]` 读取键对应的值。`find` 和 `[]` 的组合正是项目登录校验的翻版。

### 练习 6：类与封装 `ex06_class.cpp`

```cpp
#include <iostream>
using namespace std;

// 计数器：用静态成员统计「当前存活的对象个数」
// 本项目 http_conn::m_user_count 用静态成员统计连接数，思想相同
class Counter
{
public:
    Counter()                          // 构造函数
    {
        ++s_count;
        cout << "构造，当前个数 = " << s_count << endl;
    }
    Counter(const Counter &other)      // 拷贝构造函数
    {
        ++s_count;
        cout << "拷贝构造，当前个数 = " << s_count << endl;
    }
    ~Counter()                         // 析构函数
    {
        --s_count;
        cout << "析构，当前个数 = " << s_count << endl;
    }

    Counter &set_id(int id)            // 返回引用支持链式调用；this 指向当前对象
    {
        this->m_id = id;
        return *this;
    }
    int id() const { return m_id; }

    static int count() { return s_count; }  // 静态成员函数：不依赖具体对象

private:
    int m_id;
    static int s_count;                // 静态成员：类内声明
};

// 静态成员必须在类外定义并初始化，且只定义一次
int Counter::s_count = 0;

int main()
{
    Counter a;
    Counter b;
    a.set_id(1);
    b.set_id(2);
    cout << "a.id = " << a.id() << ", b.id = " << b.id() << endl;
    cout << "当前对象个数 = " << Counter::count() << endl;

    Counter c = a;                     // 触发拷贝构造
    cout << "c.id = " << c.id() << endl;
    cout << "当前对象个数 = " << Counter::count() << endl;
    return 0;
} // 离开作用域，c/b/a 依次析构，计数归零
```

关键行：第 40 行 `int Counter::s_count = 0;` 是静态成员的类外定义；第 27 行 `this->m_id` 展示 `this`；第 51 行 `Counter c = a;` 触发拷贝构造。注意对象析构顺序与构造顺序相反。

### 练习 7：模板 `ex07_template.cpp`

```cpp
#include <iostream>
using namespace std;

// 函数模板：类型参数 T
template <typename T>
T bigger(T a, T b)
{
    return a > b ? a : b;
}

// 类模板：mini 栈，铺垫本项目 threadpool<T> / block_queue<T>
template <typename T>
class Stack
{
public:
    Stack(int cap = 10) : m_top(0), m_cap(cap)
    {
        m_data = new T[cap];
    }
    ~Stack()
    {
        delete[] m_data;
    }
    void push(const T &v)
    {
        if (m_top < m_cap)
            m_data[m_top++] = v;
    }
    bool empty() const { return m_top == 0; }
    void pop() { if (m_top > 0) --m_top; }
    T top() const { return m_data[m_top - 1]; }

private:
    T *m_data;
    int m_top;
    int m_cap;
};

int main()
{
    cout << bigger(3, 8) << endl;      // T = int
    cout << bigger(2.5, 1.5) << endl;  // T = double

    Stack<int> s(4);                   // 用 int 实例化
    s.push(10);
    s.push(20);
    s.push(30);
    while (!s.empty())
    {
        cout << "pop " << s.top() << endl;
        s.pop();
    }

    Stack<char> cs(3);                 // 用 char 实例化，同一套代码
    cs.push('a');
    cs.push('b');
    cout << "top char = " << cs.top() << endl;

    return 0;
}
```

关键行：第 5 行函数模板；第 12 行类模板；第 45、55 行分别用 `int`、`char` 实例化同一个 `Stack`。模板会在编译期自动「生成」两个独立的类。

### 练习 8：异常 `ex08_exception.cpp`

```cpp
#include <iostream>
#include <exception>
using namespace std;

int divide(int a, int b)
{
    if (b == 0)
        throw std::exception();   // 本项目 locker.h/threadpool.h 里正是这样抛的
    return a / b;
}

int main()
{
    try
    {
        cout << divide(10, 2) << endl;
        cout << divide(10, 0) << endl;  // 这里抛出异常
        cout << "这行不会执行" << endl;
    }
    catch (std::exception &e)           // 接住异常
    {
        cout << "捕获到异常" << endl;
    }
    cout << "程序继续执行" << endl;
    return 0;
}
```

关键行：第 8 行 `throw std::exception();` 抛出异常；第 14～23 行 `try-catch` 接住，避免程序崩溃；第 18 行因异常被跳过。

### 练习 9：RAII `ex09_raii.cpp`

```cpp
#include <iostream>
#include <ctime>
using namespace std;

// 模拟一把互斥锁（本项目 lock/locker.h 的简化版）
class Mutex
{
public:
    void lock()   { cout << "  [lock]  加锁" << endl; }
    void unlock() { cout << "  [lock]  解锁" << endl; }
};

// RAII 锁包装：构造加锁、析构解锁（本项目 connectionRAII 同理）
class LockGuard
{
public:
    LockGuard(Mutex &m) : m_mutex(m)
    {
        m_mutex.lock();
    }
    ~LockGuard()
    {
        m_mutex.unlock();   // 无论函数怎么退出都会执行
    }
private:
    Mutex &m_mutex;
};

// RAII 计时器：构造记开始时间，析构打印耗时
class Timer
{
public:
    Timer(const char *name) : m_name(name)
    {
        m_start = clock();
    }
    ~Timer()
    {
        double ms = double(clock() - m_start) / CLOCKS_PER_SEC * 1000.0;
        cout << "[" << m_name << "] 耗时 " << ms << " ms" << endl;
    }
private:
    const char *m_name;
    clock_t m_start;
};

void do_something(Mutex &m)
{
    LockGuard guard(m);   // 进入函数即加锁
    Timer t("do_something");
    // ... 假设这里做了一堆事，且可能提前 return ...
    // 函数结束时先析构 t（打印耗时），再析构 guard（自动解锁）
}

int main()
{
    Mutex m;
    cout << "调用 do_something:" << endl;
    do_something(m);
    cout << "调用结束，锁已自动释放" << endl;
    return 0;
}
```

关键行：第 19 行构造里 `lock()`、第 23 行析构里 `unlock()`——加锁解锁被对象生命周期自动配对；第 50 行 `Timer t(...)` 演示「进入函数记时、离开函数自动打印」。`do_something` 不需要任何手动的 `unlock`。

### 练习 10：多文件编译 `ex10/`

建子目录并创建三个文件：

```bash
mkdir -p ex10
```

`ex10/math_utils.h`：

```cpp
#ifndef MATH_UTILS_H   // 头文件保护：防止被重复包含
#define MATH_UTILS_H

int square(int x);
int cube(int x);

#endif
```

`ex10/math_utils.cpp`：

```cpp
#include "math_utils.h"

int square(int x)
{
    return x * x;
}

int cube(int x)
{
    return x * x * x;
}
```

`ex10/main.cpp`：

```cpp
#include <iostream>
#include "math_utils.h"
using namespace std;

int main()
{
    cout << "square(4) = " << square(4) << endl;
    cout << "cube(3)   = " << cube(3) << endl;
    return 0;
}
```

关键行：`math_utils.h` 第 1～2、7 行的 `#ifndef/#define/#endif` 是头文件保护；`main.cpp` 第 2 行用 `"math_utils.h"`（双引号，先找当前目录）而非 `<>`（找系统目录）。编译时把两个 `.cpp` 一起传给 g++。

### 练习 11：C 字符串解析 `ex11_cstring.cpp`

```cpp
#include <iostream>
#include <cstring>     // strlen/strpbrk/strspn/strchr/strcat 等标准 C 字符串函数
#include <strings.h>   // strncasecmp（POSIX 头文件，忽略大小写比较）
using namespace std;

// 模拟解析一行 HTTP 请求："GET /index.html HTTP/1.1"
// 本项目 http_conn.cpp 的 parse_request_line 正是这个套路
int main()
{
    char line[128] = "GET /index.html HTTP/1.1\r\n";

    // 1. strpbrk：找第一个空格/制表符，定位方法名末尾
    char *method_end = strpbrk(line, " \t");
    if (method_end == NULL)
    {
        cout << "解析失败" << endl;
        return 1;
    }
    *method_end = '\0';          // 截断：line 现在是 "GET"
    cout << "method = " << line << endl;

    // 2. strspn：跳过连续空白，定位 URL 开头
    char *url = method_end + 1;
    url += strspn(url, " \t");

    // 3. strchr：找 URL 末尾的空格并截断
    char *url_end = strchr(url, ' ');
    if (url_end != NULL)
        *url_end = '\0';
    cout << "url    = " << url << endl;   // /index.html

    // 4. strncasecmp：忽略大小写比较前 3 个字符
    if (strncasecmp(line, "GET", 3) == 0)
        cout << "这是一个 GET 请求" << endl;

    // 5. strcat：拼接路径（本项目拼 m_real_file 同理）
    char target[128] = "/root";
    strcat(target, url);   // url 是 "/index.html"，拼成 "/root/index.html"
    cout << "target = " << target << endl;

    // 6. strlen：字符串长度（不含结尾 '\0'）
    cout << "target 长度 = " << strlen(target) << endl;

    return 0;
}
```

关键行：第 13 行 `strpbrk` 定位方法名末尾；第 19 行 `*method_end = '\0'` 用空字符截断；第 24 行 `strspn` 跳过空白；第 27 行 `strchr` 找空格截断 URL；第 33 行 `strncasecmp` 忽略大小写判断方法。

### 练习 12：printf 格式串 `ex12_printf.cpp`

```cpp
#include <cstdio>
using namespace std;

int main()
{
    int fd = 3;
    const char *name = "conn";
    char ip[16] = "127.0.0.1";
    int port = 9006;
    double cost = 1.2345;

    // %d 整数，%s 字符串
    printf("fd = %d, name = %s, ip = %s:%d\n", fd, name, ip, port);

    // %p 打印指针地址（转成 void* 避免告警）
    printf("ip 字符串的首地址 = %p\n", (void *)ip);

    // 宽度与对齐：%5d 右对齐占 5 位，%-5d 左对齐
    printf("[%5d]\n", 42);
    printf("[%-5d]\n", 42);

    // %.2f 保留两位小数
    printf("cost = %.2f\n", cost);

    // snprintf：把格式化结果写进缓冲区（本项目 log.cpp 大量使用）
    char buf[128];
    snprintf(buf, sizeof(buf), "%s:%d", ip, port);
    printf("拼接结果 = %s\n", buf);

    return 0;
}
```

关键行：第 13 行 `%d/%s` 占位；第 16 行 `%p`；第 19～20 行 `%5d`/`%-5d` 对齐；第 27 行 `snprintf` 写入缓冲区。这些是看懂 `log.cpp` 日志格式串的基础。

## 编译与运行

回到 `stage01` 目录，逐个编译运行。用一条循环批处理单文件练习（`ex10` 是多文件，单独编）：

```bash
cd ~/projects/my_tiny_webserver/stage01

# 单个练习（以练习 1 为例）
g++ ex01_structure.cpp -o ex01_structure
./ex01_structure

# 批量编译并运行所有单文件练习
for f in ex01_structure ex02_ref ex03_func ex04_new ex05_stl \
         ex06_class ex07_template ex08_exception ex09_raii \
         ex11_cstring ex12_printf; do
    g++ "$f.cpp" -o "$f" && echo "===== $f =====" && ./"$f"
done

# 多文件练习
g++ ex10/main.cpp ex10/math_utils.cpp -o ex10/multi
./ex10/multi
```

> 小技巧：想打开编译器告警帮助自查，用 `g++ -Wall ex01_structure.cpp -o ex01_structure`。`-Wall` 会提示「变量未使用」「类型不匹配」等潜在问题，写代码时建议一直带上；出现 warning 不等于编译失败，但要养成清零告警的习惯。

## 验收清单

逐条执行，每一条的预期输出都出现才算过关（`ex04`/`ex12` 里的内存地址每次运行不同，属正常现象）：

- [ ] `g++ ex01_structure.cpp -o ex01_structure && ./ex01_structure` 输出 `9 的平方是 81`
- [ ] `g++ ex02_ref.cpp -o ex02_ref && ./ex02_ref` 依次输出 `a = 20`、`a = 30`、`a = 31`、`value = 31`、`MAX_FD = 10000`
- [ ] `g++ ex03_func.cpp -o ex03_func && ./ex03_func` 输出 `[level 0] server started`、`[level 3] bad request`、`6`、`7`、`3`
- [ ] `g++ ex04_new.cpp -o ex04_new && ./ex04_new` 输出三行地址（且 heap 地址与 stack/global 明显不同区段）以及 `heap_arr[0..4] = 0/10/20/30/40`、`*single = 42`
- [ ] `g++ ex05_stl.cpp -o ex05_stl && ./ex05_stl` 输出 `user 长度 = 4`、`port[0..2] = 9006/9007/9008`、`qinyi 已存在（不能重复注册）`、`登录成功`
- [ ] `g++ ex06_class.cpp -o ex06_class && ./ex06_class` 输出三段 `构造/拷贝构造`、中间 `当前对象个数 = 2` 与 `3`、结尾三个 `析构` 计数递减到 `0`
- [ ] `g++ ex07_template.cpp -o ex07_template && ./ex07_template` 输出 `8`、`2.5`、`pop 30/20/10`、`top char = b`
- [ ] `g++ ex08_exception.cpp -o ex08_exception && ./ex08_exception` 输出 `5`、`捕获到异常`、`程序继续执行`（且中间不出现 `这行不会执行`）
- [ ] `g++ ex09_raii.cpp -o ex09_raii && ./ex09_raii` 输出 `加锁` → `[do_something] 耗时 ...` → `解锁` → `调用结束，锁已自动释放`（加锁与解锁成对出现）
- [ ] `g++ ex10/main.cpp ex10/math_utils.cpp -o ex10/multi && ./ex10/multi` 输出 `square(4) = 16`、`cube(3) = 27`
- [ ] `g++ ex11_cstring.cpp -o ex11_cstring && ./ex11_cstring` 输出 `method = GET`、`url = /index.html`、`这是一个 GET 请求`、`target = /root/index.html`、`target 长度 = 16`
- [ ] `g++ ex12_printf.cpp -o ex12_printf && ./ex12_printf` 输出 `fd = 3, name = conn, ip = 127.0.0.1:9006`、`[   42]`、`[42   ]`、`cost = 1.23`、`拼接结果 = 127.0.0.1:9006`

## 参考答案对照

本阶段是语法课，练习代码是教学自带的，仓库里没有「逐文件对应」的答案。但每个知识点都指向了本项目真实源码，做练习时可随时翻开对照：

| 知识点 | 本项目对应位置 |
|---|---|
| 程序结构 | `main.cpp`（`main` 入口、`#include "config.h"`） |
| 类型/const/引用 | `http/http_conn.h`（`init(int, const sockaddr_in &, ...)`、`static const int READ_BUFFER_SIZE`） |
| 函数 | `http/http_conn.h`（`close_conn(bool real_close = true)`、内联 `get_line()`）；`lock/locker.h`（`sem()`/`sem(int)` 重载） |
| new/delete | `threadpool/threadpool.h`（`new pthread_t[]` / `delete[]`）；`log/block_queue.h`（`new T[max_size]`） |
| STL 容器 | `http/http_conn.h` 与 `http/http_conn.cpp`（`map<string, string>`，第 433、452 行的 `find`/`[]`）；`threadpool/threadpool.h`（`std::list<T *>`） |
| 类与封装 | `http/http_conn.h`（`static int m_epollfd;`、`static int m_user_count;` 第 111～112 行） |
| 模板 | `threadpool/threadpool.h`（`template <typename T> class threadpool`）；`log/block_queue.h`（`template <class T> class block_queue`） |
| 异常 | `lock/locker.h`（`throw std::exception();`）；`threadpool/threadpool.h` |
| RAII | `CGImysql/sql_connection_pool.h`（`class connectionRAII` 第 49～53 行） |
| 头文件保护 | `lock/locker.h`（`LOCKER_H`）、`threadpool/threadpool.h`（`THREADPOOL_H`）、`http/http_conn.h`（`HTTPCONNECTION_H`） |
| C 字符串函数族 | `http/http_conn.cpp`（`parse_request_line` 里的 `strpbrk`/`strspn`/`strncasecmp`/`strchr`/`strcat`/`strcpy`） |
| printf/格式串 | `log/log.cpp`（`snprintf`/`vsnprintf`/`va_list`）；`http/http_conn.cpp`（`add_response` 里的 `vsnprintf`） |

## 常见问题

**1. `g++ 文件.cpp -o 文件` 后 `./文件` 报 `command not found`**
少敲了 `./`。Linux 不会自动在当前目录找可执行程序，必须写 `./ex01_structure` 这样「当前目录下的」路径。

**2. 编译报 `expected ';' before '}'` / `undefined reference to 'square(int)'`**
前者是某行结尾漏了分号（`cout << ... << endl` 那句最常漏）；后者是「声明了函数但没定义」，多文件练习里通常是 `g++` 只写了一个 `.cpp`，要把 `main.cpp` 和 `math_utils.cpp` 一起编译。

**3. `delete` 数组时程序崩溃或报 `malloc(): invalid pointer`**
数组用了 `delete` 而非 `delete[]`（或反过来）。记住口诀：`new` 配 `delete`，`new[]` 配 `delete[]`。

**4. 改了 `ex02_ref.cpp` 里被注释的那行 `x = 0;` 后编译报 `assignment of read-only reference`**
这正是 `const int &x` 的防护：`const` 引用只读，不能赋值。看懂这条报错，就理解了「const 引用」存在的意义。

**5. 静态成员编译报 `undefined reference to 'Counter::s_count'`**
静态成员在类内只是「声明」，忘了在类外（`int Counter::s_count = 0;`）做「定义」。两者缺一不可，定义只能写一次。

**6. `map` 的 `users["不存在的键"]` 为什么「查」一下就会凭空多出一个键？**
`operator[]` 在键不存在时会**默认插入**一个空值再返回引用。所以「只判断存在性」要用 `find()`，不要用 `[]`——本项目登录校验里就是先 `find` 再 `[]`，避免意外插入。

**7. 模板类 `Stack` 如果把实现拆到 `.cpp` 文件会报链接错误**
类模板必须在「用到它的地方」看到完整定义才能实例化，所以实现要放在头文件里（本项目 `threadpool.h`/`block_queue.h` 都是这么干的）。普通类可以 .h 声明 + .cpp 实现，模板类不可以。

**8. `ex11_cstring.cpp` 里 `strcat(target, url)` 前为什么 target 要开 128 那么大的缓冲区？**
`strcat` 不会检查目标缓冲区是否够大，若 `target` 开太小会写越界，造成段错误或悄悄破坏相邻内存。操作 C 字符串时，缓冲区宁大勿小；本项目 HTTP 里拼路径用的就是 `FILENAME_LEN`（200）大小的数组并配合 `strncpy` 限长。

## 思考题

1. 「引用」和「指针」都能间接访问变量，为什么函数传参时更推荐 `const 引用` 而不是指针或值拷贝？说出一条理由。
2. `Counter c = a;` 触发的是拷贝构造；如果把 `Counter` 的拷贝构造删掉，这段代码还合法吗？它会调用什么？（提示：默认生成的拷贝构造是「浅拷贝」，对本例的 `int` 成员没问题，但如果成员是指针就有隐患。）
3. 静态成员 `s_count` 为什么必须「类内声明 + 类外定义」？试着用「所有对象共享一份，但编译后内存里只能有一份」来解释。
4. `ex07` 的 `Stack<int>` 和 `Stack<char>` 是同一个类吗？编译器在背后做了什么？
5. RAII 的核心是「析构函数一定会被调用」。什么情况下「一定」会被调用？如果对象是 `new` 出来的且忘了 `delete`，RAII 还成立吗？
6. `strpbrk`、`strspn`、`strchr` 三者都涉及「找字符」，它们的语义差别在哪里？回到 `http_conn.cpp` 的 `parse_request_line`，指出每一步用的是哪个、为什么。

## 语法速查表

| 知识点 | 本项目用到它的文件 |
|---|---|
| 引用 / const 引用 | `http/http_conn.h`、`webserver.h` |
| 默认参数 | `http/http_conn.h`（`close_conn(bool real_close = true)`） |
| 函数重载 | `lock/locker.h`（`sem()` / `sem(int)`） |
| 内联函数 | `http/http_conn.h`（`get_line()`） |
| new[] / delete[] | `threadpool/threadpool.h`、`log/block_queue.h` |
| string | `main.cpp`、`config.h`、`CGImysql/*` |
| vector | 教学补充，项目未直接使用（项目用 `list` 存队列与连接池） |
| map | `http/http_conn.cpp`（注册登录校验） |
| list | `threadpool/threadpool.h`（`std::list<T *> m_workqueue`）、`CGImysql/sql_connection_pool.h`（`list<MYSQL *> connList`） |
| 类 / 封装 / 静态成员 | `http/http_conn.h`（`m_epollfd`、`m_user_count`） |
| 函数模板 / 类模板 | `threadpool/threadpool.h`、`log/block_queue.h` |
| 异常 | `lock/locker.h`、`threadpool/threadpool.h` |
| RAII | `CGImysql/sql_connection_pool.h`（`connectionRAII`） |
| 头文件保护 | 所有 `.h` 文件 |
| C 字符串函数族 | `http/http_conn.cpp`（HTTP 解析） |
| printf/snprintf/vsnprintf | `log/log.cpp`、`http/http_conn.cpp`（`add_response`） |

## 下一步

语法基础就位后，进入 [Stage 2：阻塞式 echo 服务器](stage-02-socket-echo.md)——用 socket API 写出人生第一个能通过网络回显的服务器，并开始接触 make 构建。
