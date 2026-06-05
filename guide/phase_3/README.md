# Phase 3 —— MySQL 连接池

## 目标

实现一个**单例 MySQL 连接池**，支持多线程安全地获取和归还连接，并通过 **RAII 类**自动管理连接的生命周期。

**可见结果：** 程序启动时创建 N 个 MySQL 连接放入池中，多线程并发取连接 → 执行 SQL → 归还连接，全程无泄漏、无竞争。

---

## 前置知识

- Phase 1 的 `locker.h`（信号量在这里是核心）
- Phase 2 了解日志宏 `LOG_ERROR` 的用法即可
- 知道 MySQL 的基本概念：连接、查询、结果集

---

## 工具聚焦

| 工具 | 本次学什么 |
|------|-----------|
| **cmake** | `find_package` / `find_library` 链接外部 C 库（libmysqlclient） |

---

## 分步实现

### Step 1：MySQL C API 快速入门

MySQL 提供了 C 语言的客户端库 `libmysqlclient`。核心 API：

```cpp
#include <mysql/mysql.h>

// 1. 初始化连接句柄
MYSQL* conn = mysql_init(NULL);

// 2. 连接数据库
mysql_real_connect(conn, "localhost", "root", "password",
                   "dbname", 3306, NULL, 0);

// 3. 执行 SQL
mysql_query(conn, "SELECT username, passwd FROM user");

// 4. 获取结果集
MYSQL_RES* result = mysql_store_result(conn);
int num_fields = mysql_num_fields(result);
MYSQL_ROW row;  // 一行数据，char** 类型
while ((row = mysql_fetch_row(result))) {
    printf("user: %s, pass: %s\n", row[0], row[1]);
}

// 5. 清理
mysql_free_result(result);
mysql_close(conn);
```

### Step 2：连接池设计

**核心问题：** 如果每个 HTTP 请求都新建一个 MySQL 连接 → 太慢（TCP 握手 + 认证开销）。所以预先创建一批连接，需要时取一个，用完归还。

```cpp
// sql_connection_pool.h
#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <string>
#include "../lock/locker.h"
#include "../log/log.h"

class connection_pool {
public:
    MYSQL* GetConnection();          // 获取一个连接（阻塞直到有可用的）
    bool   ReleaseConnection(MYSQL* conn);  // 归还连接
    int    GetFreeConn();            // 当前空闲连接数
    void   DestroyPool();            // 销毁所有连接

    static connection_pool* GetInstance();   // 单例

    void init(std::string url, std::string User,
              std::string PassWord, std::string DBName,
              int Port, int MaxConn, int close_log);

private:
    connection_pool();
    ~connection_pool();

    int   m_MaxConn;
    int   m_CurConn;        // 当前正在使用的连接数
    int   m_FreeConn;       // 当前空闲连接数
    locker  lock;
    std::list<MYSQL*> connList;
    sem     reserve;        // 信号量：空闲连接数

public:
    std::string m_url;
    std::string m_User;
    std::string m_PassWord;
    std::string m_DatabaseName;
    int m_close_log;
};

// RAII 包装：构造时获取连接，析构时自动归还
class connectionRAII {
public:
    connectionRAII(MYSQL** con, connection_pool* connPool);
    ~connectionRAII();
private:
    MYSQL* conRAII;
    connection_pool* poolRAII;
};

#endif
```

**为什么用信号量 `reserve`？**

`GetConnection` 需要：如果池中没有空闲连接，阻塞调用线程直到有人归还。这正是信号量的语义——`reserve` 的值 = 当前空闲连接数。

```cpp
// 线程 A 想要连接
reserve.wait();    // 空闲数 -1；如果为 0 则阻塞
// ... 此时空闲连接必定 >= 1，因为 reserve 保证了 ...
lock.lock();
conn = connList.front();  // 取一个
connList.pop_front();
lock.unlock();

// 线程 B 归还连接
lock.lock();
connList.push_back(conn);
lock.unlock();
reserve.post();    // 空闲数 +1；唤醒等待的线程
```

如果不加信号量而只用 `cond` + `while`，逻辑也可以实现，但信号量更直观——它天然代表"可用资源数"。

### Step 3：连接池实现

```cpp
// sql_connection_pool.cpp
#include "sql_connection_pool.h"

connection_pool::connection_pool()
    : m_CurConn(0), m_FreeConn(0) {}

connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(std::string url, std::string User,
    std::string PassWord, std::string DBName,
    int Port, int MaxConn, int close_log)
{
    m_url     = url;
    m_User    = User;
    m_PassWord = PassWord;
    m_DatabaseName = DBName;
    m_close_log = close_log;

    for (int i = 0; i < MaxConn; ++i) {
        MYSQL* con = mysql_init(NULL);
        if (con == NULL) {
            LOG_ERROR("MySQL init error");
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(),
                                 User.c_str(), PassWord.c_str(),
                                 DBName.c_str(), Port, NULL, 0);
        if (con == NULL) {
            LOG_ERROR("MySQL connect error: %s", mysql_error(con));
            exit(1);
        }
        connList.push_back(con);
        ++m_FreeConn;
    }

    reserve = sem(m_FreeConn);   // 用空闲数初始化信号量
    m_MaxConn = m_FreeConn;
}

MYSQL* connection_pool::GetConnection() {
    MYSQL* con = NULL;
    if (connList.size() == 0) return NULL;

    reserve.wait();       // 等待有可用的连接

    lock.lock();
    con = connList.front();
    connList.pop_front();
    --m_FreeConn;
    ++m_CurConn;
    lock.unlock();

    return con;
}

bool connection_pool::ReleaseConnection(MYSQL* con) {
    if (con == NULL) return false;

    lock.lock();
    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;
    lock.unlock();

    reserve.post();       // 通知等待的线程
    return true;
}

void connection_pool::DestroyPool() {
    lock.lock();
    for (auto it = connList.begin(); it != connList.end(); ++it) {
        mysql_close(*it);
    }
    m_CurConn = 0;
    m_FreeConn = 0;
    connList.clear();
    lock.unlock();
}

connection_pool::~connection_pool() { DestroyPool(); }
```

### Step 4：RAII 连接包装

```cpp
// 在 sql_connection_pool.h 中定义
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() {
    poolRAII->ReleaseConnection(conRAII);
}
```

**使用方式：**

```cpp
// 每次需要连接时：
MYSQL* mysql = NULL;
connectionRAII mysqlcon(&mysql, pool);  // 构造：获取连接

// ... 用 mysql 执行各种 SQL ...

// 离开作用域自动调用析构 → ReleaseConnection
```

对比不用 RAII 的写法：

```cpp
MYSQL* mysql = pool->GetConnection();
// ... 执行 SQL ...
// 如果这里抛异常或提前 return —— 连接泄漏！
pool->ReleaseConnection(mysql);
```

RAII 保证：无论函数如何退出（正常 return、异常、提前返回），连接一定被归还。

### Step 5：cmake 链接 MySQL 库

```cmake
cmake_minimum_required(VERSION 3.10)
project(MySQLPoolDemo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR})

# 方式一：手动指定路径（如果 find_package 找不到）
# find_library(MYSQL_LIB mysqlclient /usr/lib/x86_64-linux-gnu)

# 方式二：使用 find_package（推荐）
# 注意：libmysqlclient 并非 cmake 官方模块，需要自己写 FindMySQL.cmake
# 更简单的做法：
set(MYSQL_LIB /usr/lib/x86_64-linux-gnu/libmysqlclient.so)

add_library(sql_pool_lib STATIC
    sql_connection_pool.cpp
)

add_executable(test_pool test_pool.cpp)
target_link_libraries(test_pool sql_pool_lib pthread ${MYSQL_LIB})

# 如果 libmysqlclient 不在标准路径，用下面的方式
# find_library(MYSQLCLIENT_LIB mysqlclient PATHS /usr/lib/x86_64-linux-gnu)
# target_link_libraries(test_pool sql_pool_lib pthread ${MYSQLCLIENT_LIB})
```

> **安装依赖：** `sudo apt install libmysqlclient-dev`

### Step 6：测试程序

```cpp
// test_pool.cpp
#include <iostream>
#include <pthread.h>
#include "CGImysql/sql_connection_pool.h"
#include "log/log.h"

int close_log = 0;

void* worker(void* arg) {
    connection_pool* pool = (connection_pool*)arg;

    for (int i = 0; i < 3; ++i) {
        MYSQL* mysql = NULL;
        connectionRAII mysqlcon(&mysql, pool);  // 自动获取

        // 执行一个简单的查询
        if (mysql_query(mysql, "SELECT 1") == 0) {
            MYSQL_RES* res = mysql_store_result(mysql);
            mysql_free_result(res);
            std::cout << "Thread " << pthread_self()
                      << " query OK (" << i << ")" << std::endl;
        }
    }  // 连接自动归还
    return NULL;
}

int main() {
    Log::get_instance()->init("./TestLog", close_log);

    connection_pool* pool = connection_pool::GetInstance();
    pool->init("localhost", "root", "your_password",
               "your_db", 3306, 5, close_log);

    const int THREADS = 10;
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i)
        pthread_create(&threads[i], NULL, worker, pool);
    for (int i = 0; i < THREADS; ++i)
        pthread_join(threads[i], NULL);

    std::cout << "All done! Free connections: "
              << pool->GetFreeConn() << std::endl;
    return 0;
}
```

预期输出：10 个线程各执行 3 次查询，最终空闲连接 = 5（全部归还）。

---

## 验证方法

- [ ] 10 个线程并发获取/归还连接，无崩溃无死锁
- [ ] 最终空闲连接数 = 池大小（全部归还）
- [ ] 注释掉 `connectionRAII` 的析构函数调用 → 运行后空闲连接数变少，验证 RAII 的价值
- [ ] 把池大小设为 2，线程数 10 → 观察 `GetConnection` 的阻塞行为

---

## 踩坑记录

1. **MySQL 8.0 认证问题。** `mysql_real_connect` 返回 `Authentication plugin 'caching_sha2_password' cannot be loaded`？MySQL 8.0 默认用 `caching_sha2_password`。解决方案：
   ```sql
   ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY 'your_password';
   FLUSH PRIVILEGES;
   ```

2. **`mysql_init` 返回 NULL。** 通常是 `libmysqlclient` 没安装或没链接上。`ldd ./build/test_pool | grep mysql` 可以检查动态库是否正确链接。

3. **信号量初始化为 0 的陷阱。** `connection_pool::init` 中信号量必须在连接创建**之后**初始化，因为信号量的值 = 空闲连接数。如果初始化早了就是 0，`GetConnection` 永远阻塞。

4. **不要在多线程中共享同一个 MYSQL* 连接。** MySQL 的 C API 本身不是线程安全的。连接池的正确用法是：每个线程取独立的连接，用完归还。

---

## 阶段小结

你实现了：
- MySQL C API 的基本操作
- 信号量驱动的连接池（获取阻塞、归还唤醒）
- RAII 包装类 `connectionRAII`：自动归还的核心机制

下一阶段：**线程池**——用模板实现通用线程池，让所有 HTTP 请求的处理任务并行执行。
