# C2 类与对象

> Part 1 第二章。C++ 的类和面向对象是项目的地基——`config`、`webserver`、`http_conn`、`log`……几乎所有模块都是一个类。

## 1. 本课目标

- [ ] 理解类的封装:public / private
- [ ] 掌握构造函数、析构函数、初始化列表
- [ ] 掌握 `this`、`static` 成员、`const` 成员函数
- [ ] 会用 `std::string` 和 `std::vector`
- [ ] 运行示例程序 + 完成 2 道练习

**铺路说明:** 项目里 `http_conn` 是"大而全"的类(状态、解析、响应都封装在一个对象里),`locker.h` 用 RAII 封装锁,`sql_connection_pool` 用单例模式。本章把这些类的语法工具都备齐。

## 2. 类与封装

类 = 数据 + 操作这些数据的函数,把它们打包成一个类型:

```cpp
class Student {
private:                 // 私有:只有类内部能访问
    std::string name;
    int score;

public:                  // 公有:外面能调用
    void set_score(int s) {
        score = s;
    }
    int get_score() const {
        return score;
    }
};
```

- `private` 成员只能由类自己的成员函数访问——外面 `stu.score = 100` 会编译报错
- `public` 成员(如方法)对外可见
- **封装的意义**:数据被"锁"在类里,只能通过方法改,便于检查非法值。项目里普遍这么写

> `struct` 和 `class` 几乎一样,唯一区别:struct 默认 public,class 默认 private。项目里两个都用。

## 3. 构造函数与析构函数

对象创建时,自动调用**构造函数**;对象销毁时,自动调用**析构函数**。这是 C++ 生命周期管理的核心。

```cpp
class Student {
private:
    std::string name;
    int score;
public:
    // 构造函数:名字和类名相同,没有返回类型
    Student(const std::string &n, int s) : name(n), score(s) {
        std::cout << "[构造] " << name << std::endl;
    }

    // 析构函数:类名前加 ~,没有返回类型,没有参数,只能有一个
    ~Student() {
        std::cout << "[析构] " << name << std::endl;
    }
};
```

**初始化列表**(构造函数后冒号的部分):

```cpp
Student(const std::string &n, int s) : name(n), score(s) { }
```

这等价于 `name = n; score = s;`,但**更推荐**:
1. 效率更高(const / 引用成员**只能**用初始化列表赋值)
2. 语义清楚:一进来成员就是最终值

**没有写构造函数时**,编译器会生成一个什么都不干的默认构造函数。但一旦你写了带参构造,默认构造就没了——所以 `Student s;` 会报错,必须 `Student s("alice", 80);`。

## 4. this 指针、static 成员、const 成员函数

### this 指针

每个成员函数里都有一个隐藏的 `this` 指针,指向"当前这个对象":

```cpp
void set_score(int score) {      // 参数名和成员同名
    this->score = score;         // this->score 是成员,score 是参数
}
```

> `this->score` 明确说"这个对象的 score",解决命名冲突。项目里大量出现。

### static 成员(静态成员)

`static` 成员**不属于某个对象,而属于整个类**,所有对象共享一份:

```cpp
class Student {
private:
    static int count;        // 静态成员变量声明
public:
    Student(...) { count++; }
    static int total() { return count; }   // 静态成员函数
};

int Student::count = 0;      // ★ 必须在类外定义一次,分配真正的存储

// 用法:不依赖对象,直接用类名
std::cout << Student::total() << std::endl;
```

**记忆点:** 普通成员 = "每个对象各有一份";static 成员 = "整个类共有一份"。项目里 `http_conn::m_user_count` 就是 static——统计所有连接的个数,而不是某一个连接对象里的。

### const 成员函数

函数末尾加 `const`,声明"这个函数不会修改对象的成员":

```cpp
void print() const {          // 承诺只读
    // score = 100;          // ❌ 编译报错:const 函数里不能改成员
    std::cout << score << std::endl;
}
```

**记忆点:** 加 `const` 是给编译器和你自己的承诺。项目里读数据的函数基本都是 `const`。

## 5. std::string 和 std::vector

C++ 标准库的两员大将,项目里最常用的容器。

### std::string(字符串)

```cpp
#include <string>

std::string msg = "hello";
msg += " world";              // 拼接
std::cout << msg.size();      // 长度 = 11
std::cout << msg[0];          // 'h',按下标取字符
// 比较、查找
if (msg.find("world") != std::string::npos) {
    std::cout << "包含 world";
}
```

### std::vector(动态数组)

```cpp
#include <vector>

std::vector<int> v;           // 空数组,元素类型 int
v.push_back(10);              // 末尾追加 → [10]
v.push_back(20);              // [10, 20]
std::cout << v.size();        // 2
std::cout << v[1];            // 20,按下标访问

std::vector<std::string> names;   // 可以是任意类型
```

> 后面 C4 你会看到 `std::vector` 其实是个**模板类**——`vector<int>` 的 `int` 就是模板参数。这也是项目里 `threadpool<T>` 的原理。

## 6. 示例程序

在 `~/c2_demo` 下建 `demo.cpp`,把上面全部用起来:

```cpp
#include <iostream>
#include <string>
#include <vector>

class Student {
private:
    std::string name;
    int score;
    static int count;          // 静态成员:所有对象共享

public:
    // 构造函数:对象创建时自动调用(带初始化列表)
    Student(const std::string &n, int s) : name(n), score(s) {
        count++;
        std::cout << "[构造] " << name << " 创建, 当前共 " << count << " 个学生" << std::endl;
    }

    // 析构函数:对象销毁时自动调用
    ~Student() {
        count--;
        std::cout << "[析构] " << name << " 销毁, 剩余 " << count << " 个" << std::endl;
    }

    // const 成员函数:只读
    void print() const {
        std::cout << "name=" << name << " score=" << score << std::endl;
    }

    void set_score(int s) {    // this 用法:同名参数
        this->score = s;
    }

    static int total() {       // 静态成员函数
        return count;
    }
};

// 静态成员必须在类外定义(分配真正的存储)
int Student::count = 0;

int main() {
    std::cout << "当前学生数: " << Student::total() << std::endl;   // 用类名调用

    Student a("alice", 80);
    Student b("bob", 90);

    a.print();
    b.print();
    b.set_score(95);
    b.print();

    // std::vector 动态数组
    std::vector<int> scores;
    scores.push_back(80);
    scores.push_back(95);
    std::cout << "scores 大小: " << scores.size() << std::endl;

    // std::string 常用操作
    std::string msg = "hello";
    msg += " world";
    std::cout << "msg: " << msg << ", 长度 " << msg.size() << std::endl;

    std::cout << "程序结束前学生数: " << Student::total() << std::endl;
    return 0;
}  // 作用域结束,a、b 在这里被析构
```

编译运行:

```bash
cd ~/c2_demo
g++ -std=c++11 -Wall -o demo demo.cpp
./demo
```

**预期输出:**

```text
当前学生数: 0
[构造] alice 创建, 当前共 1 个学生
[构造] bob 创建, 当前共 2 个学生
name=alice score=80
name=bob score=90
name=bob score=95
scores 大小: 2
msg: hello world, 长度 11
程序结束前学生数: 2
[析构] bob 销毁, 剩余 1 个
[析构] alice 销毁, 剩余 0 个
```

**盯着这两行看**:`程序结束前学生数: 2` 之后,main 返回、`a`、`b` 两个对象离开作用域,析构函数自动被调用,`count` 从 2 减到 0。**析构函数不是你调用的,是 C++ 替你调用的**——这就是"对象生命周期管理",RAII 的核心(下一章)。

## 7. 练习

**练习 1:写一个 Circle 类。** 成员 `radius`,构造用初始化列表,写 `area()` const 成员函数返回面积,再写 getter / setter。main 里创建一个半径 2 的圆打印面积,改半径再打印。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>

class Circle {
private:
    double radius;
public:
    Circle(double r) : radius(r) {}          // 构造 + 初始化列表
    double area() const {                    // const 成员函数
        return 3.14159 * radius * radius;
    }
    void set_radius(double r) { radius = r; }
    double get_radius() const { return radius; }
};

int main() {
    Circle c(2.0);
    std::cout << "半径 " << c.get_radius() << " 面积 " << c.area() << std::endl;
    c.set_radius(1.0);
    std::cout << "改半径后面积 " << c.area() << std::endl;
    return 0;
}
```

预期输出:

```text
半径 2 面积 12.5664
改半径后面积 3.14159
```

</details>

**练习 2:vector + string 存名单。** 用 `std::vector<std::string>` 存 3 个名字,循环打印"序号. 名字"。

<details>
<summary>参考答案(先自己做,再点开)</summary>

```cpp
#include <iostream>
#include <string>
#include <vector>

int main() {
    std::vector<std::string> names;
    names.push_back("alice");
    names.push_back("bob");
    names.push_back("carol");
    std::cout << "共 " << names.size() << " 人:" << std::endl;
    for (int i = 0; i < (int)names.size(); i++) {
        std::cout << "  " << i + 1 << ". " << names[i] << std::endl;
    }
    return 0;
}
```

预期输出:

```text
共 3 人:
  1. alice
  2. bob
  3. carol
```

</details>

## 8. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 编译运行示例程序 | 输出与 6 节预期完全一致(含最后两条析构日志) | ☐ |
| 2 | 编译运行练习 1 | 面积 12.5664 / 3.14159 | ☐ |
| 3 | 编译运行练习 2 | 按序打印 3 个名字 | ☐ |
| 4 | 把示例里 `Student a("alice", 80);` 改成 `Student a;` | 编译报错,明白为什么(没有默认构造函数) | ☐ |

> 第 4 条再做一个实验:试着在 `main` 里写 `a.score = 100;`,会看到编译报错——private 起作用了。这是理解封装的直观方式。

## 9. 下一步

进入 **[C3 RAII](cpp-03-raii.md)**——把"对象生命周期"用到资源管理上,这是项目里锁封装和数据库连接池的核心思想。
