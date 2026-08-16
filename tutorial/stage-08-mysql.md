# Stage 8：数据库与注册登录

本阶段给服务器接入 MySQL：写一个**数据库连接池**（单例 + 信号量 + 互斥锁 + RAII），让 HTTP 服务器支持**注册 / 登录**。学完本阶段，你就能在浏览器里完成"注册新用户 → 跳转登录页 → 登录成功进入欢迎页"的完整闭环，同时学会 RAII、单例、STL 容器、C 字符串操作和 MySQL C API。

本阶段结束时的 `my_tiny_webserver/` 目录长这样（相对上一阶段新增/改动的文件用 `+` 标出）：

```text
my_tiny_webserver/
├── lock/locker.h                       # 与仓库一致（不变）
├── threadpool/threadpool.h             # + 升级为仓库完整版
├── http/http_conn.h                    # + 增加 cgi/mysql/m_users 等成员
├── http/http_conn.cpp                  # + 增加 initmysql_result / do_request 的 cgi 分支
├── timer/lst_timer.h、lst_timer.cpp    # 与仓库一致（不变）
├── log/block_queue.h、log.h、log.cpp   # 与仓库一致（不变）
├── CGImysql/sql_connection_pool.h      # + 新增：连接池头文件
├── CGImysql/sql_connection_pool.cpp    # + 新增：连接池实现
├── server.cpp                          # + 接入连接池与用户表加载
├── root/                               # 学习者自备页面（本阶段补齐表单页）
└── makefile                            # + 加 -lmysqlclient 与 sql_connection_pool.cpp
```

## 前置要求

- 已完成 [Stage 7](stage-07-log.md)：`my_tiny_webserver/` 里有能跑起来的 epoll + 线程池 + 定时器 + 日志的 HTTP 服务器。
- 已按 [Stage 0](stage-00-environment.md) 安装好 MySQL 8 与客户端；确认 `mysql -uroot -p` 能登录。
- 已安装 MySQL 开发库（供 C/C++ 链接）：

```bash
sudo apt update
sudo apt install -y libmysqlclient-dev
```

安装后确认头文件和库存在：

```bash
ls /usr/include/mysql/mysql.h
# 预期输出：/usr/include/mysql/mysql.h
```

- 掌握 [Stage 3](stage-03-threadpool.md) 的互斥锁 / 信号量 / 条件变量封装类（`locker`、`sem`、`cond`）。

## 理论学习

### 为什么需要数据库与连接池

注册登录需要"把用户名密码存起来、下次能查回来"。文件当然也能存，但数据库更适合：结构化查询、并发读写、事务，都是现成的。本项目用 MySQL 存一张 `user` 表。

问题在于：**每次和 MySQL 打交道都要先建立一条 TCP 连接**，`mysql_real_connect` 内部要经历三次握手、认证、交换字符集等步骤，开销很大。如果每个 HTTP 请求都现场 `connect` + `close`，在压测下很快就会被连接开销拖垮。

```text
没有连接池（每次请求都建连）：
请求1 ──connect──> MySQL ──close──
请求2 ──connect──> MySQL ──close──
请求3 ──connect──> MySQL ──close──   ← 大量时间花在建连/拆连上

有连接池（预先建好 N 条，随取随还）：
启动时一次性建好 8 条连接放在池子里
请求1 ──取连接1──> MySQL ──还回池子──
请求2 ──取连接2──> MySQL ──还回池子──
请求3 ──取连接3──> MySQL ──还回池子──   ← 几乎零建连开销
```

连接池 = "提前建好一批连接，用的时候借，用完还回去，而不是销毁"。本项目把连接池数量做成可配置（默认 8 条）。

### 连接池的工作方式：信号量计数 + list 队列 + 互斥锁

仓库的 `connection_pool` 用三样东西配合：

| 组件 | 作用 |
|---|---|
| `sem reserve`（信号量） | 记录"池子里还剩几条可用连接"，同时充当**阻塞队列**：没连接时 `wait()` 会阻塞线程，有连接归还时 `post()` 唤醒一个 |
| `list<MYSQL*> connList` | 真正存放连接指针的容器，取连接 = 从头弹出一个，还连接 = 从尾塞回 |
| `locker lock`（互斥锁） | 保护 `connList` 这个共享容器，同一时刻只允许一个线程改它 |

```text
                ┌───────────────────────────────────┐
 线程A 取连接    │   reserve.wait()  (信号量计数-1)   │
 ─────────────> │        ↓                          │
 线程B 取连接    │   lock.lock()    (互斥锁保护队列)  │
 ─────────────> │        ↓                          │
                │   connList.pop_front()            │
                │        ↓                          │
                │   lock.unlock()                   │
                └───────────────────────────────────┘
                        │ 拿到连接，去干活
                        ▼
                 ┌───────────────────┐
 线程A 还连接     │  lock.lock()      │
 ─────────────>  │  connList.push_back│
                 │  lock.unlock()     │
                 │  reserve.post()    │  ← 信号量+1，唤醒一个等待者
                 └───────────────────┘
```

关键点：**先 `wait()` 再 `lock()`**。信号量负责"有没有货"，互斥锁负责"取货这个动作互斥"。如果反过来先 `lock()` 再 `wait()`，一个线程在没连接时占着锁睡觉，别的线程想还连接都进不来，直接死锁。这个问题会放在思考题里再问一遍。

### RAII 管理连接：任何路径都不会漏还

普通写法容易出问题：

```cpp
MYSQL *con = pool->GetConnection();
// 中间一旦 return / 抛异常，下面这行就永远执行不到 → 连接泄漏
pool->ReleaseConnection(con);
```

仓库用一个 `connectionRAII` 类把"取"和"还"绑到对象生命周期上：

```cpp
class connectionRAII {
public:
    connectionRAII(MYSQL **con, connection_pool *connPool); // 构造：取连接
    ~connectionRAII();                                      // 析构：还连接
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};
```

构造时把池子里取出的连接同时存进 `conRAII`；析构时无条件 `ReleaseConnection(conRAII)`。因为**局部对象离开作用域必然析构**，所以无论函数是正常 return 还是提前 return，连接都会还回去。这就是 RAII（Resource Acquisition Is Initialization，资源获取即初始化）：用对象的生命周期自动管理资源。

### SQL 基础：本项目的 user 表

本项目默认库名 `qgydb`，里面一张表 `user`，两列都是 `char(50)`：

```sql
-- 建库
CREATE DATABASE qgydb;

-- 建表
USE qgydb;
CREATE TABLE user(
    username char(50) NULL,
    passwd   char(50) NULL
) ENGINE=InnoDB;

-- 插入一条（注册时由服务器执行）
INSERT INTO user(username, passwd) VALUES('name', 'passwd');

-- 查询（服务器启动时把整表读进内存）
SELECT username, passwd FROM user;
```

三个语句分别对应：建表（DDL）、插入（DML，注册）、查询（DML，登录校验 + 启动时预热缓存）。

### 注册 / 登录的 CGI 流程

"CGI" 原意是服务器把请求交给外部程序处理，本项目里泛指"POST 请求触发的一段程序化逻辑"——具体就是 `do_request()` 里那段操作数据库的分支。

仓库用 `root/` 下的 HTML 表单，把"页面跳转"和"数据库读写"串成一个闭环：

```text
judge.html（欢迎页，两个按钮）
   ├─ 按钮"新用户"：<form action="0">   POST 到 /0
   └─ 按钮"已有账号"：<form action="1"> POST 到 /1
             │
             ▼  do_request 看 URL 最后字符
   *(p+1)=='0' → 返回 register.html（注册表单，action="3CGISQL.cgi"）
   *(p+1)=='1' → 返回 log.html（登录表单，action="2CGISQL.cgi"）
             │
             ▼ 表单提交，POST 到 /3CGISQL.cgi 或 /2CGISQL.cgi
   do_request 看 URL 最后字符 '3' / '2'
   ├─ '3' 注册：先查内存 map 是否重名
   │         ├─ 无重名 → INSERT 进数据库 → 跳 /log.html（去登录）
   │         └─ 重名   → 跳 /registerError.html
   └─ '2' 登录：内存 map 里比对用户名密码
             ├─ 成功 → 跳 /welcome.html（含 5/6/7 三个按钮）
             └─ 失败 → 跳 /logError.html
```

**为什么是 URL 最后字符 '2' / '3'？** 表单 `action="2CGISQL.cgi"` 提交后，浏览器请求的路径就是 `/2CGISQL.cgi`。服务器在 `do_request()` 里：

```cpp
const char *p = strrchr(m_url, '/');   // p 指向最后一个 '/'
if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3')) { ... }
```

`strrchr(m_url, '/')` 返回字符串里**最后一个** `/` 的位置；对 `/2CGISQL.cgi` 来说，`p` 指向开头那个 `/`，于是 `*(p+1)` 就是 `'2'`。同时 `m_url[1]` 也是 `'2'`（`m_url[0]=='/'`，`m_url[1]=='2'`），代码里那句 `char flag = m_url[1];` 取的就是这个值——不过 `flag` 声明后其实没被用到，真正的判断用的是 `*(p+1)`，这是个可以留意的细节。

同理 `root/welcome.html` 里的 `action="5"/"6"/"7"` 提交后 URL 最后字符是 `'5'/'6'/'7'`，`do_request()` 分别跳转 `picture.html`（图）、`video.html`（视频）、`fans.html`（关注页）。

### SQL 注入概念（学习版的风险）

本项目注册语句是**字符串拼接**：

```cpp
strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
strcat(sql_insert, "'"); strcat(sql_insert, name); strcat(sql_insert, "', '");
strcat(sql_insert, password); strcat(sql_insert, "')");
mysql_query(mysql, sql_insert);
```

如果用户名里带 `'`、`;`、`--` 等字符，拼出来的 SQL 就可能被"注入"成另一条语句（例如 `name` 填 `'); DROP TABLE user; --`）。这是本项目作为**学习版**的取舍：重点是讲清楚"为什么能注入"和"防御方向"（参数化查询 / 预处理语句 `mysql_stmt_*` / 转义 `mysql_real_escape_string` / 白名单校验）。真实生产环境绝不能直接拼接。

## 本阶段 C++ 知识点

### 1. RAII（connectionRAII 类）

见 [理论学习](#raii-管理连接任何路径都不会漏还)。本阶段最核心的 C++ 思想就是：**把资源生命周期交给对象生命周期**。日志的 `block_queue`、这里的 `connectionRAII` 都是这个套路。

### 2. 单例（局部静态变量）

```cpp
connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;   // 第一次调用时构造，之后永远返回同一个对象
    return &connPool;
}
```

函数内的 `static` 对象在**第一次执行到这行时**构造，之后每次调用返回同一个地址；程序结束自动析构。C++11 之后这种"局部静态变量懒汉单例"线程安全（编译器保证只初始化一次），本项目就是 C++11 下标准写法。构造函数 `connection_pool()` 被放到 `private`，外面只能通过 `GetInstance()` 拿到唯一实例。

### 3. list<MYSQL*> 队列

`connList` 是 `std::list<MYSQL*>`。用 `list` 而不是 `vector` 是因为连接池**频繁在头部弹出、尾部插入**——`list` 的 `push_back/pop_front` 都是 O(1) 且不移动其他元素；`vector` 在头部删除需要整体搬移，且指针失效问题多。

```cpp
con = connList.front();   // 取队首（最"老"的连接，可复用其已建立的会话）
connList.pop_front();     // 弹出
// ...
connList.push_back(con);  // 归还：塞回队尾
```

### 4. map<string, string> 用户名→密码缓存

服务器启动时把整张 `user` 表读进内存，之后登录校验直接查内存，不再打数据库：

```cpp
// http_conn.cpp 文件作用域的全局 map（注意：不是类成员 m_users）
map<string, string> users;

while (MYSQL_ROW row = mysql_fetch_row(result))
{
    string temp1(row[0]);   // username
    string temp2(row[1]);   // passwd
    users[temp1] = temp2;   // 用户名 → 密码
}
```

`users[name]` 用 `operator[]` 访问，`users.find(name)` 判断是否存在（`find` 返回 `end()` 表示没有）。这里的键值都是 `std::string`，`[]` 和 `find` 都用红黑树查找，复杂度 O(log n)。

> 一个容易踩的坑：头文件里声明的私有成员 `map<string,string> m_users` 在实现里其实**没有被用到**，真正生效的是 `.cpp` 文件作用域里的全局 `users`。判断登录注册用的是全局 `users`，`m_users` 是个"僵尸成员"。对照参考答案时你会发现这一点。

### 5. C 字符串与 malloc（仓库 do_request 的写法）

`do_request()` 里有大量 C 风格字符串操作，是学习 `string.h` 函数族的好素材：

```cpp
char *m_url_real = (char *)malloc(sizeof(char) * 200); // 手动分配 200 字节
strcpy(m_url_real, "/");                 // 拷贝
strcat(m_url_real, m_url + 2);           // 拼接：跳过 "/2" 或 "/3"
strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1); // 限定长度拷贝
free(m_url_real);                        // 用完释放
```

以及手工解析 POST 正文 `user=xxx&password=xxx`：

```cpp
char name[100], password[100];
int i;
for (i = 5; m_string[i] != '&'; ++i)   // 跳过 "user=" 这 5 个字符
    name[i - 5] = m_string[i];
name[i - 5] = '\0';                     // 手动补 '\0' 结尾

int j = 0;
for (i = i + 10; m_string[i] != '\0'; ++i, ++j)  // 跳过 "&password=" 这 10 个字符
    password[j] = m_string[i];
password[j] = '\0';
```

注意这里硬编码的偏移量：`5` 是 `"user="` 的长度，`10` 是 `"&password="` 的长度。HTML 表单里的字段名必须是 `user` 和 `password`（见 `root/register.html`），代码才解析得对。

> **内存泄漏点**：注册分支里 `char *sql_insert = (char *)malloc(200);` 拼好 SQL 执行完之后**没有 `free(sql_insert)`**，每注册一次就漏 200 字节。`m_url_real` 在当前参考答案里每个分支都 `free` 了（旧版曾经漏过，后来修复），但 `sql_insert` 至今仍是泄漏点。这是本阶段的思考题之一。

### 6. MySQL C API

本项目用到的函数一览（全部来自 `<mysql/mysql.h>`，链接 `-lmysqlclient`）：

| 函数 | 作用 |
|---|---|
| `mysql_init(MYSQL*)` | 初始化一个 MYSQL 句柄 |
| `mysql_real_connect(con, host, user, passwd, db, port, NULL, 0)` | 建立到 MySQL 的连接，失败返回 NULL |
| `mysql_query(mysql, sql)` | 执行一条 SQL，成功返回 0 |
| `mysql_store_result(mysql)` | 取回结果集到客户端内存 |
| `mysql_num_fields(result)` | 结果集的列数 |
| `mysql_fetch_fields(result)` | 所有列的元信息数组（本项目只是取一下，未实际使用） |
| `mysql_fetch_row(result)` | 取下一行，返回 `MYSQL_ROW`（字符串数组），结束返回 NULL |
| `mysql_error(mysql)` | 最近一次错误描述（用于日志） |
| `mysql_close(con)` | 关闭连接 |

## 动手实现

### 第 1 步：连接池 `CGImysql/sql_connection_pool.h`

先建目录 `my_tiny_webserver/CGImysql/`，写头文件。**与仓库一致**，逐行加了教学注释：

```cpp
#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;

// 数据库连接池：单例，内部用 list 存放连接，信号量计数，互斥锁保护队列
class connection_pool
{
public:
    MYSQL *GetConnection();                // 获取一条数据库连接
    bool ReleaseConnection(MYSQL *conn);   // 归还一条连接
    int GetFreeConn();                     // 查询当前空闲连接数
    void DestroyPool();                    // 销毁池中所有连接

    // 单例模式：唯一入口
    static connection_pool *GetInstance();

    // 初始化：建 MaxConn 条连接放入池中
    void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);

private:
    connection_pool();   // 构造私有，禁止外部 new
    ~connection_pool();

    int m_MaxConn;  // 最大连接数
    int m_CurConn;  // 当前已被借出的连接数
    int m_FreeConn; // 当前空闲连接数
    locker lock;    // 保护 connList 的互斥锁
    list<MYSQL *> connList; // 连接池本体（队列）
    sem reserve;    // 信号量：计数空闲连接 + 阻塞取连接

public:
    string m_url;            // 主机地址
    string m_Port;           // 端口号
    string m_User;           // 数据库用户名
    string m_PassWord;       // 数据库密码
    string m_DatabaseName;   // 数据库名
    int m_close_log;         // 日志开关
};

// RAII 包装：构造时取连接，析构时归还，保证任何路径都不漏还
class connectionRAII
{
public:
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif
```

关键点：

- 构造、析构是 `private`，只有 `GetInstance()` 能拿到唯一实例——这就是单例。
- `connectionRAII` 单独拿出来，因为它不是连接池本身，而是"借还"的守卫对象。

### 第 2 步：连接池实现 `CGImysql/sql_connection_pool.cpp`

**与仓库一致**：

```cpp
#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
    static connection_pool connPool;   // 局部静态单例：第一次调用时构造
    return &connPool;
}

// 构造初始化：一次性建立 MaxConn 条连接
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
    m_url = url;
    m_Port = Port;
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    for (int i = 0; i < MaxConn; i++)
    {
        MYSQL *con = NULL;
        con = mysql_init(con);                       // 初始化句柄

        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        // 真正建立连接；失败返回 NULL
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

        if (con == NULL)
        {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        connList.push_back(con);   // 连接入池
        ++m_FreeConn;              // 空闲计数 +1
    }

    reserve = sem(m_FreeConn);     // 信号量初值 = 空闲连接数
    m_MaxConn = m_FreeConn;
}

// 当有请求时，从连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
    MYSQL *con = NULL;

    if (0 == connList.size())
        return NULL;

    reserve.wait();      // 先等信号量：没空闲连接就阻塞在这里

    lock.lock();         // 再抢互斥锁，保证取连接互斥

    con = connList.front();
    connList.pop_front();

    --m_FreeConn;        // 空闲 -1
    ++m_CurConn;         // 使用中 +1

    lock.unlock();
    return con;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if (NULL == con)
        return false;

    lock.lock();

    connList.push_back(con);   // 连接塞回队尾
    ++m_FreeConn;              // 空闲 +1
    --m_CurConn;               // 使用中 -1

    lock.unlock();

    reserve.post();            // 信号量 +1，唤醒一个等待的线程
    return true;
}

// 销毁数据库连接池
void connection_pool::DestroyPool()
{
    lock.lock();
    if (connList.size() > 0)
    {
        list<MYSQL *>::iterator it;
        for (it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL *con = *it;
            mysql_close(con);          // 真正关闭 MySQL 连接
        }
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

// 当前空闲的连接数
int connection_pool::GetFreeConn()
{
    return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool)
{
    *SQL = connPool->GetConnection();   // 取连接，同时写回调用方的指针

    conRAII = *SQL;                     // 自己记住，析构时归还
    poolRAII = connPool;
}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);   // 析构自动归还
}
```

逐函数讲解：

- `GetInstance()`：局部 `static` 单例，见上文"单例"知识点。
- `init()`：循环 `MaxConn` 次，`mysql_init` + `mysql_real_connect` 建好连接塞进 `list`；最后 `reserve = sem(m_FreeConn)` 把信号量初值设为空闲连接数——之后这个信号量既是"计数"也是"阻塞器"。
- `GetConnection()`：先 `reserve.wait()`（没货就睡），再 `lock.lock()` 弹出队首，改计数，解锁返回。**先 wait 后 lock 的顺序是死锁与否的关键**。
- `ReleaseConnection()`：锁内把连接塞回队尾、改计数，解锁后 `reserve.post()` 唤醒等待者。
- `DestroyPool()`：遍历 `list` 逐个 `mysql_close`，清空。析构函数调用它。
- `connectionRAII`：构造时 `GetConnection()` 并把结果写回 `*SQL`（这样调用方的 `MYSQL *mysql` 就拿到了连接），同时自己保存一份；析构时无条件归还。

### 第 3 步：升级 `threadpool/threadpool.h` 为仓库完整版

上一阶段线程池是简化版（`threadpool(int thread_number, int max_requests)`、`append(T*)`、`run()` 直接 `process()`）。本阶段改成**仓库完整版**：新增 `actor_model` 与 `connection_pool*`，`append(T*, int state)` 写入请求的 `m_state`，`run()` 里按并发模型分叉。

```cpp
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
public:
    /*thread_number 是线程池中线程的数量，max_requests 是请求队列中最多允许等待处理的请求数量*/
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    bool append(T *request, int state);   // Reactor 用：把任务 + 读写标志入队
    bool append_p(T *request);            // Proactor 用：把任务入队

private:
    static void *worker(void *arg);       // 线程入口
    void run();                           // 线程真正执行的逻辑

private:
    int m_thread_number;        // 线程数
    int m_max_requests;         // 队列最大长度
    pthread_t *m_threads;       // 线程 id 数组
    std::list<T *> m_workqueue; // 任务队列
    locker m_queuelocker;       // 保护队列的互斥锁
    sem m_queuestat;            // 信号量：任务计数 + 阻塞
    connection_pool *m_connPool;// 数据库连接池（任务里取连接用）
    int m_actor_model;          // 并发模型：0 = Proactor，1 = Reactor
};

template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests)
    : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; ++i)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))   // 分离线程，结束自动回收
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;   // 写 0 = 读事件，写 1 = 写事件（Reactor 用）
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();         // 唤醒一个工作线程
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        m_queuestat.wait();          // 没任务就阻塞
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        if (1 == m_actor_model)
        {
            // Reactor：读/写都在工作线程里做
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;                    // 通知主线程"我干完了"
                    connectionRAII mysqlcon(&request->mysql, m_connPool); // RAII 取连接
                    request->process();
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;                // 读失败 → 标记踢下线
                }
            }
            else
            {
                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        else
        {
            // Proactor：主线程已读好数据，这里只处理 + 取连接
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}
#endif
```

讲解：

- 构造函数多了 `actor_model` 和 `connPool` 两个参数，用初始化列表赋给成员。
- `append(T*, int state)` 比 `append_p` 多了 `request->m_state = state`——Reactor 模式下主线程只投递"读事件/写事件"，到底该读还是该写由工作线程按 `m_state` 决定。
- `run()` 的 `if (1 == m_actor_model)` 分支是 Reactor：工作线程自己 `read_once()`/`write()`，干完把 `request->improv` 置 1 通知主线程；读写失败则 `timer_flag = 1` 让主线程把它踢下线。`else` 分支是 Proactor：主线程已经读好数据，工作线程只需 `process()`（解析 + 响应）。
- 两种模型在 `process()` 之前都用 `connectionRAII mysqlcon(&request->mysql, m_connPool)` 取出连接，`process()` 里若要访问数据库就直接用 `request->mysql`；`mysqlcon` 析构时自动归还。

> 本阶段 server.cpp 固定用 Proactor（`actor_model = 0`），所以本阶段只走 `else` 分支；Reactor 分支的完整对接（主线程自旋等 `improv`）留到 [Stage 9](stage-09-integration.md)。

### 第 4 步：升级 `http/http_conn.h`（增加 cgi / mysql 成员）

把上一阶段的简化版头文件升级为仓库完整版。主要新增：`#include "../CGImysql/sql_connection_pool.h"`、`#include <map>`、`init` 签名恢复仓库版、公开成员 `mysql / m_state`、私有成员 `cgi / m_string / doc_root / m_users / m_TRIGMode / m_close_log / sql_user / sql_passwd / sql_name`，以及方法 `initmysql_result / parse_content` 等。

```cpp
#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT, PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST, GET_REQUEST, BAD_REQUEST,
        NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST,
        INTERNAL_ERROR, CLOSED_CONNECTION
    };
    enum LINE_STATUS
    {
        LINE_OK = 0, LINE_BAD, LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    // 仓库完整版签名：多了 user/passwd/sqlname 三个数据库参数
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);   // 启动时把 user 表读进内存 map
    int timer_flag;
    int improv;

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);      // 新增：判断 POST 正文是否读完整
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;       // 处理本请求时借到的连接（由 connectionRAII 填入）
    int m_state;        // 读为 0，写为 1（Reactor 用）

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;            // 是否启用 POST（1 = 是）
    char *m_string;     // 存储 POST 正文（user=xxx&password=xxx）
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;

    map<string, string> m_users;   // 声明了但实际未使用（实现里用全局 users）
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];    // 数据库用户名（暂存，本版本未实际使用）
    char sql_passwd[100];
    char sql_name[100];
};

#endif
```

### 第 5 步：升级 `http/http_conn.cpp`（增加 cgi / mysql 逻辑）

这是本阶段改动最大的文件。**与仓库一致**，在上一阶段基础上新增：`initmysql_result`（20~48 行）、`init` 恢复仓库签名（112~132 行）、`parse_content`（330~340 行）、`do_request` 的 cgi 分支与 0/1/5/6/7 跳转（388~515 行）。下面是完整文件：

```cpp
#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义 http 响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;                 // 全局锁：保护注册时的"查重 + INSERT"临界区
map<string, string> users;     // 全局 map：用户名 → 密码（登录注册都查它）

// 启动时调用：把数据库 user 表读进内存 map
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接（RAII，函数结束自动归还）
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在 user 表中检索 username、passwd 数据
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组（本版本未实际使用，仅演示 API）
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 逐行读取，把 username/passwd 存入 map
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 将内核事件表注册读事件，ET 模式，选择开启 EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核事件表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为 EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

// 关闭连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 初始化连接：仓库完整版签名
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或 http 响应格式出错或访问的文件内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());      // 暂存数据库信息（本版本未实际使用）
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

// 初始化新接受的连接，check_state 默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;                 // 默认不是 POST
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有 LINE_OK、LINE_BAD、LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞 ET 工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    // LT 读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }
        return true;
    }
    // ET 读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析 http 请求行，获得请求方法、目标 url 及 http 版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;                 // 标记为 POST，后续可能走数据库分支
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    // 当 url 为 / 时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析 http 请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断 http 请求是否被完整读入（POST 正文完整性判断）
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';   // 把正文截断成 C 字符串
        // POST 请求中最后为输入的用户名和密码
        m_string = text;                 // m_string 指向 "user=xxx&password=xxx"
        return GET_REQUEST;
    }
    return NO_REQUEST;                   // 正文还没读全，继续等
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    const char *p = strrchr(m_url, '/');    // 取 URL 最后一个 '/' 的位置

    // 处理 cgi：POST 且 URL 最后字符是 '2'（登录）或 '3'（注册）
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 根据标志判断是登录检测还是注册检测（flag 未实际使用，真正判断看 *(p+1)）
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);   // 去掉 "/2" 或 "/3"，拼成 "/GISQL.cgi"
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);                // 当前参考答案已释放 m_url_real

        // 将用户名和密码提取出来，正文形如：user=xxx&password=xxx
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)   // 跳过 "user="（5 字符）
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)  // 跳过 "&password="（10 字符）
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            // 注册：先查内存 map 是否重名，无重名再 INSERT
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();                                // 锁住"查重 + 插入"，保证并发注册安全
                int res = mysql_query(mysql, sql_insert);     // 真正写库
                users.insert(pair<string, string>(name, password)); // 同步内存 map
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");               // 注册成功 → 去登录
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");         // 重名
            // 注意：sql_insert 这里没有 free，是内存泄漏点
        }
        else if (*(p + 1) == '2')
        {
            // 登录：直接在内存 map 里比对用户名密码
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    // 以下分支处理 welcome/judge 页里的 0/1/5/6/7 跳转
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}

bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
```

关键段逐一讲解：

- `initmysql_result`：从池子借一条连接执行 `SELECT`，把结果逐行灌进全局 `users` map。`connectionRAII mysqlcon(&mysql, connPool)` 让连接在函数结束时自动归还。
- `init`（外部）：签名多了 `user/passwd/sqlname`，把它们 `strcpy` 进 `sql_user/sql_passwd/sql_name`（本版本只暂存，未实际用于建连，建连统一走连接池）。
- `parse_content`：POST 时正文可能分多次到达，这里判断 `m_read_idx >= m_content_length + m_checked_idx` 才认为读完整，把正文截断成 C 字符串并让 `m_string` 指向它；否则返回 `NO_REQUEST` 继续等。
- `do_request` 的 cgi 分支：`strrchr` 取最后一个 `/`，`*(p+1)` 判 '2'/'3'；`m_string` 里按固定偏移解析出 `name` 和 `password`；'3' 走注册（查重→锁→INSERT→同步 map→跳转），'2' 走登录（map 比对→跳转）。
- `do_request` 的 0/1/5/6/7 分支：把 `m_real_file` 指到对应的静态 HTML，交给后面 `mmap` + `writev` 返回。

### 第 6 步：`server.cpp` 接入连接池

上一阶段你的 `server.cpp` 是"逻辑平铺在 main 里"的 Proactor 单模型版本。本阶段只做三处接入：

1. 顶部 `#include "./CGImysql/sql_connection_pool.h"`；
2. 启动时初始化连接池 + 加载用户表 + 用新签名构造线程池；
3. accept 后 `init` 换用仓库签名（多传 user/passwd/sqlname）。

下面是**完整可编译**的 `server.cpp`（Proactor 单模型过渡版，逻辑与上一阶段一致，只加了连接池接入；Reactor 与完整 `WebServer` 类留给 Stage 9）：

```cpp
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>
#include <string>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"
#include "./timer/lst_timer.h"
#include "./CGImysql/sql_connection_pool.h"

using namespace std;

const int MAX_FD = 65536;            // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000;  // 最大事件数
const int TIMESLOT = 5;              // 定时器最小超时单位（秒）

// 全局资源：信号管道、epoll 句柄、连接数组、定时器数组、工具类
static int pipefd[2];
static int epollfd = -1;
static http_conn *users;             // 下标 = 连接 fd
static client_data *users_timer;
static Utils utils;

// 全局 m_close_log：LOG_* 宏展开后引用"调用处作用域里的 m_close_log"（Stage 7 讲过的坑）。
// 本阶段 server.cpp 还是平铺的自由函数版，所以用一个文件级全局顶上；main 里另有局部
// close_log 传给 init 等函数，两者默认都是 0（日志开）。Stage 9 收拢成 WebServer 类后，
// 这个全局会被成员 m_close_log 取代（删掉）。
int m_close_log = 0;

// 定时器相关（Proactor 单模型版）
void add_timer(int connfd, struct sockaddr_in client_address, char *root, int CONNTrigmode,
               int close_log, string user, string passwd, string sqlname)
{
    users[connfd].init(connfd, client_address, root, CONNTrigmode, close_log, user, passwd, sqlname);

    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

void adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);
    LOG_INFO("%s", "adjust timer once");
}

void deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool dealclientdata(int listenfd, char *root, int LISTENTrigmode, int CONNTrigmode,
                    int close_log, string user, string passwd, string sqlname)
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == LISTENTrigmode)
    {
        int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        add_timer(connfd, client_address, root, CONNTrigmode, close_log, user, passwd, sqlname);
    }
    else
    {
        while (1)
        {
            int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            add_timer(connfd, client_address, root, CONNTrigmode, close_log, user, passwd, sqlname);
        }
        return false;
    }
    return true;
}

bool dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1 || ret == 0)
    {
        return false;
    }
    for (int i = 0; i < ret; ++i)
    {
        switch (signals[i])
        {
        case SIGALRM:
            timeout = true;
            break;
        case SIGTERM:
            stop_server = true;
            break;
        }
    }
    return true;
}

// Proactor：主线程读好数据，只把"处理"丢给线程池
void dealwithread(int sockfd, threadpool<http_conn> *pool)
{
    util_timer *timer = users_timer[sockfd].timer;
    if (users[sockfd].read_once())
    {
        LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        pool->append_p(users + sockfd);
        if (timer)
        {
            adjust_timer(timer);
        }
    }
    else
    {
        deal_timer(timer, sockfd);
    }
}

void dealwithwrite(int sockfd, threadpool<http_conn> *pool)
{
    util_timer *timer = users_timer[sockfd].timer;
    if (users[sockfd].write())
    {
        LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if (timer)
        {
            adjust_timer(timer);
        }
    }
    else
    {
        deal_timer(timer, sockfd);
    }
}

int main(int argc, char *argv[])
{
    // ---- 数据库信息（按需改成你自己的用户名密码库名）----
    string user = "root";
    string passwd = "root";
    string databasename = "qgydb";
    int sql_num = 8;          // 连接池大小
    int thread_num = 8;       // 线程数
    int port = 9006;
    int close_log = 0;        // 0 开日志，1 关日志
    int log_write = 0;        // 0 同步，1 异步
    int TRIGMode = 0;         // 本阶段固定 LT+LT；ET/Reactor 到 Stage 9
    int LISTENTrigmode = 0, CONNTrigmode = 0;
    int OPT_LINGER = 0;

    // ---- 日志 ----
    if (0 == close_log)
    {
        if (1 == log_write)
            Log::get_instance()->init("./ServerLog", close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", close_log, 2000, 800000, 0);
    }

    // ---- 分配连接数组与定时器数组（Stage 9 会把这步移进 WebServer 构造函数）----
    users = new http_conn[MAX_FD];
    users_timer = new client_data[MAX_FD];

    // ---- 数据库连接池（接入点 1）----
    connection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost", user, passwd, databasename, 3306, sql_num, close_log);

    // ---- 加载用户表进内存 map（接入点 2）----
    users[0].initmysql_result(connPool);

    // ---- 线程池（接入点 3：完整版签名，本阶段固定 Proactor=0）----
    threadpool<http_conn> *pool = new threadpool<http_conn>(0, connPool, thread_num);

    // ---- 网络编程基础步骤 ----
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    if (0 == OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    utils.addfd(epollfd, listenfd, false, LISTENTrigmode);
    http_conn::m_epollfd = epollfd;

    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    utils.setnonblocking(pipefd[1]);
    utils.addfd(epollfd, pipefd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    Utils::u_pipefd = pipefd;
    Utils::u_epollfd = epollfd;

    // 站点根目录 = 当前工作目录 + "/root"（与仓库 WebServer 构造函数一致；
    // 注意不能直接写 "/root"——那是绝对路径，指向文件系统根下的 /root，静态文件会全部 404）
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    char *doc_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(doc_root, server_path);
    strcat(doc_root, root);

    // ---- 事件循环 ----
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {
                bool flag2 = dealclientdata(listenfd, doc_root, LISTENTrigmode, CONNTrigmode,
                                            close_log, user, passwd, databasename);
                if (false == flag2)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag2 = dealwithsignal(timeout, stop_server);
                if (false == flag2)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd, pool);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd, pool);
            }
        }

        if (timeout)
        {
            utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
```

几个说明：

- `main` 开头的 `users = new http_conn[MAX_FD];` 与 `users_timer = new client_data[MAX_FD];` 是**必须先分配再按下标访问**的（这两行在 Stage 9 会移进 `WebServer` 的构造函数里）。如果漏了这两行，`users[connfd]` / `users_timer[connfd]` 会访问未初始化的指针，直接段错误。
- `add_timer` 里 `users[connfd].init(...)` 已经换成仓库签名（多了 user/passwd/sqlname）。
- 站点根目录用 `getcwd` 拼出"当前工作目录 + `/root`"（与仓库 `WebServer` 构造函数同款写法）；所以运行时要在 `my_tiny_webserver/` 目录下执行 `./server`。**不要**图省事直接写 `char root[6] = "/root"`——那是绝对路径，文件都在 `/root` 下而你的页面在 `my_tiny_webserver/root/`，会导致全部 404（进而因仓库版 `process_write` 没有 `NO_RESOURCE` 分支而"Empty reply"）。
- **别漏文件级全局 `int m_close_log = 0;`**：本文件里的自由函数（`adjust_timer`/`deal_timer`/`dealwithread` 等）都用了 `LOG_*` 宏，宏展开后引用"调用处作用域的 `m_close_log`"——没有这个全局会编译报 `'m_close_log' was not declared in this scope`（Stage 7 讲过的坑）。`main` 里传给 `init` 的局部 `close_log` 只管 http_conn 成员的日志开关，两者职责不同，默认都是 0。

### 第 7 步：更新 `makefile`（加 `-lmysqlclient`）

上一阶段 makefile 链接的是 `server.cpp + lst_timer.cpp + http_conn.cpp + log.cpp`。本阶段新增 `sql_connection_pool.cpp` 和 MySQL 库：

```makefile
CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

server: server.cpp ./timer/lst_timer.cpp ./http/http_conn.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp
	$(CXX) -o server $^ $(CXXFLAGS) -lpthread -lmysqlclient

clean:
	rm -r server
```

注意两点：

- 编译目标里**只列 `.cpp` 文件**，头文件（`locker.h`、`http_conn.h`、`sql_connection_pool.h`…）不要列，它们靠 `#include` 被拉进来（Stage 9 会专门讲依赖树）。
- `-lmysqlclient` 让链接器找到 MySQL C API 的实现；`-lpthread` 是线程库。
- 命令行里 makefile 必须以 **Tab** 缩进，复制时如果编辑器把 Tab 换成了空格会报 `missing separator`。

## 编译与运行

1. 补齐 `root/` 下的表单页。上一阶段你可能只有自己写的静态页，现在需要完整的注册登录闭环。把仓库的页面复制过来（或自己照着写）：`judge.html`、`register.html`、`log.html`、`welcome.html`、`logError.html`、`registerError.html`、`picture.html`、`video.html`、`fans.html`。

2. 建库建表（一次性）：

```bash
mysql -uroot -p
```

```sql
CREATE DATABASE qgydb;
USE qgydb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
) ENGINE=InnoDB;
```

3. 编译：

```bash
cd ~/projects/my_tiny_webserver
make
```

预期输出：无报错，生成 `server` 可执行文件（`-g` 调试模式下比较大）。

4. 运行：

```bash
./server
```

预期输出（有日志时）：服务器不退出，开始监听；`ls` 能看到 `ServerLog` 目录下按日期命名的日志文件。

5. 浏览器访问 `http://127.0.0.1:9006/`，应显示"欢迎访问"页（judge.html）。

## 验收清单

每一条都要实际跑通，看到预期输出才算过关：

- [ ] `sudo apt install -y libmysqlclient-dev` 成功，`ls /usr/include/mysql/mysql.h` 输出该路径。
- [ ] `make` 编译成功，生成 `server`，无 error（若提示找不到 `mysql/mysql.h` 或 `undefined reference to mysql_*`，见"常见问题"）。
- [ ] 启动前 `mysql -uroot -p -e "SELECT * FROM qgydb.user;"` 能列出 user 表（初始可无数据）。
- [ ] `./server` 启动后，浏览器访问 `http://127.0.0.1:9006/` 显示"欢迎访问"（judge.html）。
- [ ] 浏览器点"新用户"→ 跳转注册页；填用户名 `alice`、密码 `123456` 提交 → 跳转到 `log.html`（登录页）。
- [ ] 在 MySQL 里验证注册数据落库：`mysql -uroot -p -e "SELECT * FROM qgydb.user;"` 预期输出一行 `| alice | 123456 |`。
- [ ] 用 `alice` 重复注册 → 跳转 `registerError.html`（提示"该用户名被注册"）。
- [ ] 用 `alice` / `123456` 登录 → 跳转 `welcome.html`（"是时候做出选择了"）。
- [ ] 用错误密码登录 → 跳转 `logError.html`（提示"用户名或密码错误"）。
- [ ] `curl` 构造 POST 验证响应为 200 且内容含跳转目标（见下方命令），预期输出带 `HTTP/1.1 200`。
- [ ] 并发注册：`for i in $(seq 1 10); do curl -s -o /dev/null -X POST -d "user=u$i&password=p$i" http://127.0.0.1:9006/3CGISQL.cgi & done; wait`，随后 `mysql -uroot -p -e "SELECT COUNT(*) FROM qgydb.user;"` 预期输出**比执行前多 10 行**（例如此前只有 alice，执行后为 11）。
- [ ] 连接池耗尽场景：把 `server.cpp` 里 `sql_num` 临时改成 `1`，重新 `make` 并启动，再跑上面的 10 并发注册，观察：请求全部成功但完成时间明显变长（排队等连接）；同时 `mysql -uroot -p -e "SHOW STATUS LIKE 'Threads_connected';"` 预期值接近 1（而不是 10）。验证完改回 8。

`curl` 验证命令（注册）：

```bash
curl -v -X POST -d "user=bob&password=123456" http://127.0.0.1:9006/3CGISQL.cgi 2>&1 | head -20
```

预期输出末尾能看到 `HTTP/1.1 200 OK` 以及 `Content-Length` 等头；响应体是 `log.html` 的内容（注册成功跳登录页）。

## 参考答案对照

| 你的文件 | 仓库对应文件 | 差异说明 |
|---|---|---|
| `CGImysql/sql_connection_pool.h` | `CGImysql/sql_connection_pool.h` | **一致**（本阶段新增） |
| `CGImysql/sql_connection_pool.cpp` | `CGImysql/sql_connection_pool.cpp` | **一致**（本阶段新增） |
| `threadpool/threadpool.h` | `threadpool/threadpool.h` | **一致**（由简化版升级为完整版） |
| `http/http_conn.h` | `http/http_conn.h` | **一致**（恢复仓库完整版成员与签名） |
| `http/http_conn.cpp` | `http/http_conn.cpp` | **一致**（新增 `initmysql_result`、`parse_content`、`do_request` cgi 分支） |
| `server.cpp` | `webserver.cpp` | **结构不同**：仓库已把逻辑封装进 `WebServer` 类，你的 `server.cpp` 仍是平铺版（封装在 Stage 9 完成）。函数内部逻辑与仓库 Proactor 分支逐行对应 |
| `makefile` | `makefile` | 目标名/源文件列表不同（你的主程序叫 `server.cpp`，仓库是 `main.cpp + webserver.cpp + config.cpp`），`-lpthread -lmysqlclient` 一致 |

重点对照三处容易抄错的地方：

1. `do_request` 里解析 POST 正文的偏移量：`5` 是 `"user="` 长度，`10` 是 `"&password="` 长度；HTML 表单字段名必须是 `user` 和 `password`，否则解析出的用户名密码是空串。
2. `initmysql_result` 填的是**文件作用域全局 `users`**，不是头文件里的成员 `m_users`（`m_users` 声明了但没用）。
3. `GetConnection` 是"先 `reserve.wait()` 再 `lock.lock()`"；`ReleaseConnection` 是"锁内归还，解锁后 `post()`"。

## 常见问题

1. **编译报 `fatal error: mysql/mysql.h: No such file or directory`**：没装开发库。执行 `sudo apt install -y libmysqlclient-dev`。若还不行，确认头文件在 `/usr/include/mysql/mysql.h`，并确认 makefile 里用的是 `-lmysqlclient`（不是 `-lmysql`）。

2. **链接报 `undefined reference to mysql_init/mysql_query/...`**：链接顺序或库名问题。把 `-lmysqlclient` 放在**所有 `.o`/源文件之后**（本 makefile 的 `$^` 在前、`-lpthread -lmysqlclient` 在后，顺序正确）。如果是用 g++ 手动一条命令编，同理把 `-lmysqlclient` 放最后。

3. **启动即退出并打印 `MySQL Error`**：连接池 `init` 里 `mysql_real_connect` 失败。九成是账号密码库名不对，或 MySQL 没启动（`sudo service mysql start`）。确认 `main`/`server.cpp` 里 `user="root"`、`passwd="root"`、库名 `qgydb` 与你的实际环境一致，且已 `CREATE DATABASE qgydb;`。

4. **注册总跳 `registerError.html`，但数据库里根本没数据**：多半是 `m_string` 解析错位。检查表单字段名是不是 `name="user"` 和 `name="password"`（不要写成 `passwd`），以及 `do_request` 里偏移量 `5` 和 `10` 是否被改错。可在 `do_request` 加 `printf("name=%s password=%s\n", name, password);` 观察。

5. **登录永远失败（跳到 logError.html）**：登录是查内存 map `users`，而 map 是**启动时**从数据库一次性读入的。如果你在服务器运行期间用 SQL 手动插了数据、或换了库，内存 map 不会自动更新。重启服务器再试。另外确认 `initmysql_result` 确实被调用了（漏调用则 map 为空，谁都登录不上）。

6. **`users` / `users_timer` 未初始化就按下标访问，段错误**：`main` 开头的 `users = new http_conn[MAX_FD];` 与 `users_timer = new client_data[MAX_FD];` 两行必须保留；若被误删，`users[connfd]` 会访问野指针直接崩溃（Stage 9 的 `WebServer` 构造函数里做同样的分配）。

7. **浏览器注册成功却显示一堆乱码/空白**：`root/` 目录相对路径问题。站点根目录是"当前工作目录下的 `root`"，必须在 `my_tiny_webserver/` 下执行 `./server`；用绝对路径启动而 `root` 又不在对应目录时就会 404。

## 思考题

1. `GetConnection` 为什么必须"先 `reserve.wait()` 再 `lock.lock()`"？如果对调会有什么后果？试着画一个"线程 A 拿不到连接占着锁睡、线程 B 想还连接却进不来"的死锁时序图。

2. `connectionRAII` 析构时，如果那条连接已经损坏（比如 MySQL 断连），直接 `ReleaseConnection` 还回池子会发生什么？更健壮的连接池应该怎么做（健康检查 / 重连 / 归还前 `mysql_ping`）？

3. 服务器启动后，`users` map 与数据库表如果"不一致"会怎样？举一个具体场景（提示：运行期间用 SQL 改了数据，或两个服务器实例共享一个库），并说明这种设计有什么优缺点、怎么改进。

4. 仓库 `do_request` 里还有哪些 `malloc` 泄漏点？（提示：`sql_insert`）用三种方式修复：a) 补 `free`；b) 换成栈上数组 `char sql_insert[200]`；c) 换成 `std::string`。三种方式的取舍是什么？

5. 为什么 `initmysql_result` 要放在"建完连接池之后、线程池启动之前"？如果加载用户表也要走线程池，会有什么问题？

6. 本项目把用户密码**明文**存进数据库，也没有做 SQL 注入防御。如果要上生产，至少需要补哪几项（参数化查询、密码加盐哈希、连接加密）？各解决什么问题？

## 下一步

恭喜，你的服务器已经会"注册 + 登录"了。下一阶段 [Stage 9：整合收官](stage-09-integration.md) 会把散在 `server.cpp` 里的逻辑重新组织进 `WebServer` 类，补齐命令行参数解析、Reactor 并发模型、LT/ET 四组合，并用 WebBench 压测——至此 `my_tiny_webserver/` 就与仓库源码逐文件对应了。
