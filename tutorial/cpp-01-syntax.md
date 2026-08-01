# C1 语法速览

> 本教程 Part 1 的第一章。目标:把 C++ 的日常语法过一遍,你不需要记住全部,重点是**后面读服务器代码时不发怵**。

## 1. 本课目标

- [ ] 掌握基本类型、`const`、`auto`
- [ ] 掌握 if / for / while
- [ ] 分清函数的三种传参方式:**传值 / 传引用 / 传指针**
- [ ] 搞懂指针和引用的区别
- [ ] 运行示例程序 + 完成 2 道练习

**铺路说明:** 这一章是全部服务器阶段的地基。`config.cpp` 的 `parse_arg` 用到指针和 `switch`;`http_conn.cpp` 里到处都是 `const`、引用、指针。别急着背,读到后面回头查就行。

## 2. 变量与类型

C++ 基本类型:

```cpp
int    count = 10;        // 整数
double pi    = 3.14159;   // 浮点数
char   ch    = 'a';       // 单个字符
bool   ok    = true;      // 布尔
```

**`const` —— 只读修饰符:**

```cpp
const int MAX_SIZE = 100;   // MAX_SIZE 不能被修改
MAX_SIZE = 200;             // ❌ 编译报错:assignment of read-only variable
```

写 `const` 的代码,编译器会在你试图修改时帮你拦住——这是保护代码的好习惯,项目里大量使用。

**`auto` —— 自动类型推断(C++11):**

```cpp
auto x = 42;       // x 自动推断为 int
auto name = "abc"; // name 推断为 const char*
```

`auto` 不改变语义,只是省去手写类型。适合类型名很长的时候(后面模板章节你会爱上它)。

## 3. 控制流

```cpp
// if / else
if (score >= 60) {
    std::cout << "及格" << std::endl;
} else {
    std::cout << "不及格" << std::endl;
}

// for 循环:初始化; 条件; 每次迭代后执行
for (int i = 0; i < 5; i++) {
    std::cout << i << " ";
}   // 输出 0 1 2 3 4

// while 循环
int n = 3;
while (n > 0) {
    n--;
}   // 执行 3 次

// switch:按值分支(项目里 config 解析参数会用到)
switch (code) {
    case 0:
        std::cout << "zero" << std::endl;
        break;              // 别忘了 break,否则会"掉落"到下个 case
    case 1:
        std::cout << "one" << std::endl;
        break;
    default:
        std::cout << "other" << std::endl;
}
```

## 4. 函数:三种传参方式

这是本章重点。看这段三行各异的函数:

```cpp
// ① 传值:参数是"拷贝",改了不影响外面
void add_one(int a) {
    a = a + 1;
}

// ② 传引用:参数是"别名",改了影响外面(参数带 &)
void add_one(int &a) {
    a = a + 1;
}

// ③ 传指针:传的是地址,通过地址修改(参数是 int*)
void add_one(int *a) {
    *a = *a + 1;   // *a 表示"指针 a 指向的那个变量"
}
```

调用区别:

```cpp
int x = 10;
add_one(x);       // 传值:x 仍是 10
add_one(x);       // 传引用:x 变成 11
add_one(&x);      // 传指针:x 变成 12(&x 取 x 的地址)
```

**怎么记?**
- 不想让函数改我的变量 → **传值**
- 想让函数改我的变量,或避免大对象拷贝 → **传引用**(`&`)
- 有 C 背景、习惯用指针 → **传指针**(`*`)

> 💡 项目里最常见的组合是 **`const` + 引用**:`const std::string &s` 表示"把字符串传进来,但不允许函数修改它"——既有引用的效率,又有传值的安心。Stage 8 你会频繁见到。

**默认参数:**

```cpp
void greet(std::string name, std::string prefix = "Hi") {
    std::cout << prefix << ", " << name << std::endl;
}
greet("Bob");            // 输出 Hi, Bob
greet("Bob", "Hello");   // 输出 Hello, Bob
```

## 5. 指针与引用

| | 指针 `int *p` | 引用 `int &r` |
|---|---|---|
| 是什么 | 存地址的变量 | 变量的别名 |
| 可以为空 | 可以(`nullptr`) | 不行,必须绑定 |
| 能改指向 | 可以 | 不行,绑定终生 |
| 取值写法 | `*p` | 直接用 |
| 取地址写法 | `&x` 给 p | `&x` 给 r |
| 声明时 | `int *p = &x;` | `int &r = x;` |

```cpp
int x = 42;
int *p = &x;   // p 存 x 的地址
int &r = x;    // r 是 x 的别名

*p = 43;       // 通过指针改 → x = 43
r = 44;        // 通过引用改 → x = 44
```

**空指针用 `nullptr`**(C++11 推荐,不要用 C 的 `NULL` 或 `0`):

```cpp
int *p = nullptr;   // 表示"不指向任何东西"
if (p == nullptr) {
    std::cout << "空指针" << std::endl;
}
```

## 6. 示例程序

把上面全用起来。在 `~/c1_demo` 下建 `demo.cpp`:

```cpp
#include <iostream>
#include <string>

// 结构体:描述一个学生
struct Student {
    std::string name;   // 字符串类型,后面 C2 细讲
    int score;
};

// 传引用:能修改调用者的变量(给分数加分)
void boost(int &s, int delta) {
    s += delta;
}

// 传指针:通过指针修改 name;const std::string& 表示只读传入
void rename_student(Student *p, const std::string &new_name) {
    p->name = new_name;
}

// 传值:只读打印,不影响调用者
void print_student(Student s) {
    std::cout << "name=" << s.name << " score=" << s.score << std::endl;
}

int main() {
    Student alice{"alice", 80};     // 聚合初始化:按成员顺序给值
    print_student(alice);           // 传值,拷贝一份

    boost(alice.score, 5);          // 传引用,修改 score
    print_student(alice);

    rename_student(&alice, "ALICE");  // 传指针,修改 name
    print_student(alice);

    // const 与 auto
    const int MAX = 100;
    auto score = alice.score;       // auto 自动推断类型
    if (score >= MAX) {
        std::cout << "满分!" << std::endl;
    } else {
        std::cout << "继续加油, 还差 " << MAX - score << " 分" << std::endl;
    }

    // 循环
    int sum = 0;
    for (int i = 1; i <= 5; i++) {
        sum += i;
    }
    std::cout << "1..5 之和 = " << sum << std::endl;

    return 0;
}
```

编译运行:

```bash
cd ~/c1_demo
g++ -std=c++11 -Wall -o demo demo.cpp
./demo
```

**预期输出:**

```text
name=alice score=80
name=alice score=85
name=ALICE score=85
继续加油, 还差 15 分
1..5 之和 = 15
```

对照代码逐行看输出:
- `boost(alice.score, 5)` 把 score 从 80 改成 85(引用修改)
- `rename_student(&alice, ...)` 把 name 改成 ALICE(指针修改)
- `auto score` 推断成 int,和 `MAX` 比较
- 循环累加 1..5

## 7. 练习

**练习 1:写一个交换函数。** 用引用实现 `swap(int &a, int &b)`,在 `main` 里交换两个变量并打印。要求:必须真的交换成功。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>

void swap(int &a, int &b) {
    int t = a;
    a = b;
    b = t;
}

int main() {
    int x = 1, y = 2;
    swap(x, y);
    std::cout << "x=" << x << " y=" << y << std::endl;   // 输出 x=2 y=1
    return 0;
}
```

> 这里**必须用引用或指针**。如果写成传值 `swap(int a, int b)`,交换的是拷贝,`x`、`y` 不会变——试一下看看会发生什么,这就是理解传值/传引用的最好实验。

</details>

**练习 2:统计字符次数。** 写函数 `count_char(const std::string &s, char c)` 返回 `c` 在 `s` 里出现的次数,在 `main` 里对 `"hello world"` 统计 `'l'`。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <string>

int count_char(const std::string &s, char c) {
    int n = 0;
    for (int i = 0; i < (int)s.size(); i++) {
        if (s[i] == c) n++;
    }
    return n;
}

int main() {
    std::string msg = "hello world";
    std::cout << "l 出现 " << count_char(msg, 'l') << " 次" << std::endl;
    return 0;
}
```

预期输出:`l 出现 3 次`

> `(int)s.size()` 把 `size()` 返回的无符号数转成 int,避免"无符号数比大小"的坑。`s[i]` 取第 i 个字符,`s.size()` 返回长度。

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行示例程序 | 输出与 6 节预期完全一致(5 行) | ☐ |
| 2 | 编译运行练习 1 | 输出 `x=2 y=1` | ☐ |
| 3 | 编译运行练习 2 | 输出 `l 出现 3 次` | ☐ |
| 4 | 把练习 1 的 `swap` 改成传值,观察 | `x`、`y` 不变(输出 `x=1 y=2`) | ☐ |

> 第 4 条是本章最重要的一次"破坏性实验":它让你亲眼看到传值和传引用的差别,比背一百遍定义都有用。

## 9. 下一步

进入 **[C2 类与对象](cpp-02-class.md)**——项目里几乎所有模块都是类,这一章把类的语法学扎实。
