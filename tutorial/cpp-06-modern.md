# C6 现代特性

> Part 1 第六章,也是最后一章基础。把项目代码里高频出现的 C++11 写法补齐,读代码时就不会被"没见过"吓到。

## 1. 本课目标

- [ ] 会用 `nullptr`、`auto`、范围 for
- [ ] 会写 lambda 和 `std::function`
- [ ] 知道 `std::move` 是干嘛的(简述)
- [ ] 运行示例程序 + 完成 1 道练习

**铺路说明:** 项目里 `block_queue.h` 用范围 for 释放线程;`http_conn` 的 `users` 全局 map 赋值用了 `pair`;lambda 在 C++11 版实现里常用于回调。这些是最后一层语法垫脚石。

## 2. nullptr / auto / 范围 for

### nullptr

C++11 之前用 `NULL`(本质是 `0`),容易和整数 0 混淆。现在统一用 `nullptr` 表示空指针:

```cpp
int *p = nullptr;          // 安全的空指针
if (p == nullptr) { ... }  // 判断是否为空
```

### auto

让编译器推断类型:

```cpp
auto n = 42;                  // int
auto s = std::string("abc");  // std::string
```

`auto` 不是动态类型,类型在编译期就定死了,只是省得手写。

### 范围 for(range-based for)

遍历容器最爽的写法:

```cpp
std::vector<int> v = {1, 2, 3, 4, 5};

// 传统写法
for (int i = 0; i < (int)v.size(); i++) {
    std::cout << v[i];
}

// 范围 for:直接给"每个元素"
for (int x : v) {           // x 依次等于 1, 2, 3, 4, 5
    std::cout << x;
}
```

不想复制元素、只想读,可以用 `const auto &x : v`(对 `std::string` 这种大对象很有用)。

## 3. lambda(匿名函数)

就地定义一个"一次性函数",常用于回调、排序、算法参数:

```cpp
// [捕获列表] (参数) { 函数体 }
auto square = [](int x) { return x * x; };
int r = square(5);            // 25
```

**捕获列表** `[...]`:让 lambda 能读到外面的变量。

```cpp
int factor = 3;
auto times = [factor](int x) { return x * factor; };  // 捕获 factor 的值
std::cout << times(7);   // 21
```

三种常见捕获:
- `[factor]`:按值捕获(拷贝一份)
- `[&]`:按引用捕获全部外部变量
- `[=]`:按值捕获全部外部变量

## 4. std::function

一个"能装各种可调用对象"的通用类型:普通函数、lambda、函数对象都能存进去:

```cpp
#include <functional>

int add_one(int x) { return x + 1; }

std::function<int(int)> f = add_one;   // 存一个普通函数
f(9);                                  // 10

f = [](int x) { return x * x; };       // 换成一个 lambda
f(4);                                  // 16
```

> 项目里 `std::function` 用得不算多,但了解它对理解"把函数当参数传"很有帮助。

## 5. std::move(简述)

```cpp
#include <utility>   // 或 <string>

std::string s1 = "hello";
std::string s2 = std::move(s1);   // 把 s1 的"资源"转移给 s2
// 之后 s1 通常是空串,数据在 s2 手里
```

- `std::move` **不拷贝数据**,而是把资源的所有权转移走,省一次大拷贝
- 被 move 的变量之后"通常为空",不能再依赖它的内容

> **这一章知道"move 是转移资源、省拷贝"就够了。** 深层的右值引用与完美转发,等你进阶再看。项目里用得很少。

## 6. 示例程序

在 `~/c6_demo` 下建 `demo.cpp`:

```cpp
#include <iostream>
#include <vector>
#include <string>
#include <functional>

// 一个普通函数,用于赋值给 std::function
int add_one(int x) {
    return x + 1;
}

int main() {
    // nullptr:安全的空指针
    int *p = nullptr;
    if (p == nullptr) {
        std::cout << "p 是空指针(nullptr)" << std::endl;
    }

    // auto:类型推断
    auto n = 42;
    auto name = std::string("TinyWebServer");

    // 范围 for:遍历容器
    std::vector<int> v = {1, 2, 3, 4, 5};
    int sum = 0;
    for (int x : v) {          // x 依次等于每个元素
        sum += x;
    }
    std::cout << "总和 = " << sum << std::endl;

    // lambda:匿名函数
    auto square = [](int x) { return x * x; };
    std::cout << "5 的平方 = " << square(5) << std::endl;

    // lambda 捕获变量
    int factor = 3;
    auto times = [factor](int x) { return x * factor; };
    std::cout << "7 × 3 = " << times(7) << std::endl;

    // std::function:存储任意可调用对象
    std::function<int(int)> f = add_one;        // 存普通函数
    std::cout << "add_one(9) = " << f(9) << std::endl;
    f = square;                                  // 换个 lambda
    std::cout << "f(4) = " << f(4) << std::endl;

    return 0;
}
```

编译运行:

```bash
cd ~/c6_demo
g++ -std=c++11 -Wall -o demo demo.cpp
./demo
```

**预期输出:**

```text
p 是空指针(nullptr)
总和 = 15
5 的平方 = 25
7 × 3 = 21
add_one(9) = 10
f(4) = 16
```

再建 `move.cpp` 看 move 的效果:

```cpp
#include <iostream>
#include <string>

int main() {
    std::string s1 = "hello";
    std::string s2 = std::move(s1);   // 把 s1 的资源"转移"给 s2
    std::cout << "s2 = " << s2 << std::endl;
    std::cout << "s1 转移后 = '" << s1 << "' (通常已空)" << std::endl;
    return 0;
}
```

```bash
g++ -std=c++11 -Wall -o move move.cpp
./move
```

**预期输出:**

```text
s2 = hello
s1 转移后 = '' (通常已空)
```

## 7. 练习

**练习:用 lambda + 范围 for 求偶数和。** 对 `{1,2,3,4,5,6,7,8}` 用范围 for 遍历,用一个 `is_even` lambda 判断偶数,求和打印。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <vector>

int main() {
    std::vector<int> v = {1, 2, 3, 4, 5, 6, 7, 8};
    auto is_even = [](int x) { return x % 2 == 0; };   // lambda:判断偶数
    int sum = 0;
    for (int x : v) {          // 范围 for
        if (is_even(x)) sum += x;
    }
    std::cout << "偶数和 = " << sum << std::endl;
    return 0;
}
```

预期输出:

```text
偶数和 = 20
```

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行 demo.cpp | 输出与 6 节预期完全一致(6 行) | ☐ |
| 2 | 编译运行 move.cpp | `s2 = hello`、`s1 转移后 = ''` | ☐ |
| 3 | 编译运行练习 | `偶数和 = 20` | ☐ |
| 4 | 把练习的 `auto is_even` 改成普通函数再编译 | 能运行,理解 lambda 只是"就地定义的函数" | ☐ |

## 9. 下一步

**C++ 基础部分结束!** 从 [Stage 1](stage-01-tcp-echo.md) 开始进入真正的网络编程——第一个 socket 程序。
