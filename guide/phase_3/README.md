# Phase 3 —— MySQL 连接池

## 本阶段目标

实现一个**线程安全的 MySQL 数据库连接池**，使用单例模式 + 信号量控制连接数量，并通过 RAII 机制自动归还连接。

**可见结果：** 多个线程从连接池获取连接 → 执行 SQL 查询 → 归还连接。所有线程安全地共用有限数量的数据库连接。

**验收标准：**

- [ ] 连接池正确初始化指定数量的 MySQL 连接
- [ ] 多线程并发获取/归还连接，不会超用
- [ ] RAII 包装类离开作用域时自动归还连接
- [ ] 连接池耗尽时，请求线程阻塞等待（信号量机制）
- [ ] 用 gdb 验证连接数不会超过 MaxConn

---

## 理论与机制

### 1. 为什么要用连接池？

**MySQL 连接很昂贵。** 每次 `mysql_real_connect` 需要：
1. TCP 三次握手（~1.5 RTT）
2. MySQL 协议握手（认证、字符集协商）
3. 在服务器端分配线程/内存

一个 Web 请求可能只需要几十毫秒的数据库查询，但建立连接可能花几百毫秒。**连接池预先建立 N 个连接，请求来了直接"借用"一个，用完归还。**

### 2. 连接池的核心设计

```
┌─────────────────────────────────────────────┐
│             connection_pool (单例)            │
│                                              │
│  connList: [conn1] → [conn2] → ... → [connN] │
│                                              │
│  reserve (信号量, 初值 N):                     │
│    - wait(): 没有空闲连接 → 阻塞               │
│    - post(): 归还连接 → 唤醒等待者              │
│                                              │
│  lock (互斥锁)：保护 list 的并发修改             │
└─────────────────────────────────────────────┘

线程获取连接流程:
  GetConnection()
    → reserve.wait()  // 信号量 P：有空闲吗？没有就等
    → lock.lock()     // 拿锁：保护 list 操作
    → connList.pop_front()  // 从队头取连接
    → lock.unlock()
    → 返回连接指针

线程归还连接流程:
  ReleaseConnection()
    → lock.lock()
    → connList.push_back()  // 放回队尾
    → lock.unlock()
    → reserve.post()  // 信号量 V：唤醒等待的线程
```

### 3. 信号量 + 互斥锁的双重保护

**为什么需要两把"锁"？**

- **信号量（reserve）**：控制"能不能拿"——语义层面的保护（有可用连接吗？）
- **互斥锁（lock）**：控制"拿的过程"——操作层面的保护（list 操作原子性）

如果只用互斥锁：需要 `while(队列空) sleep` → 实现复杂且浪费 CPU
如果只用信号量：不能保护 `list` 的并发修改（两个线程可能同时 pop 同一个元素）

**两者配合 = 正确的连接池。**

### 4. RAII 自动归还

```cpp
class connectionRAII {
public:
    connectionRAII(MYSQL **con, connection_pool *pool) {
        *con = pool->GetConnection();  // 构造时获取
        conRAII = *con;
        poolRAII = pool;
    }
    ~connectionRAII() {
        poolRAII->ReleaseConnection(conRAII);  // 析构时自动归还！
    }
};

// 使用：无论函数怎么退出（return、异常），连接都会被归还
void handle_request() {
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, pool);  // 获取
    // ... 使用 mysql 执行查询 ...
    // 自动归还 ← C++ RAII 的威力！
}
```

---

## 实现指南

### Step 1：连接池类的声明

```cpp
class connection_pool {
public:
    static connection_pool* GetInstance();  // 单例入口

    void init(string url, string User, string PassWord,
              string DBName, int Port, int MaxConn, int close_log);
    MYSQL* GetConnection();
    bool ReleaseConnection(MYSQL* conn);
    void DestroyPool();

private:
    connection_pool();   // [语法] 私有构造 → 外部不能 new
    ~connection_pool();

    int m_MaxConn, m_CurConn, m_FreeConn;
    locker lock;              // 保护 connList
    list<MYSQL*> connList;    // [语法] STL 双向链表
    sem reserve;              // 信号量：控制可用连接数
};
```

### Step 2：C++11 Magic Statics（单例）

```cpp
connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;  // [语法] 局部静态变量
    return &connPool;
}
```

**C++11 保证：** `static` 局部变量的初始化是线程安全的。编译器自动插入锁。C++98 则不是。

### Step 3：初始化连接

```cpp
void connection_pool::init(...) {
    for (int i = 0; i < MaxConn; i++) {
        MYSQL* con = mysql_init(NULL);
        con = mysql_real_connect(con, url.c_str(), User.c_str(),
                                 PassWord.c_str(), DBName.c_str(),
                                 Port, NULL, 0);
        connList.push_back(con);
        ++m_FreeConn;
    }
    reserve = sem(m_FreeConn);  // [语法] 带参构造：信号量初值 = 连接数
    m_MaxConn = m_FreeConn;
}
```

### Step 4：获取/归还连接

```cpp
MYSQL* connection_pool::GetConnection() {
    reserve.wait();          // P 操作：等待可用连接
    lock.lock();
    MYSQL* con = connList.front();
    connList.pop_front();
    --m_FreeConn; ++m_CurConn;
    lock.unlock();
    return con;
}

bool connection_pool::ReleaseConnection(MYSQL* con) {
    lock.lock();
    connList.push_back(con);
    ++m_FreeConn; --m_CurConn;
    lock.unlock();
    reserve.post();          // V 操作：唤醒等待者
    return true;
}
```

---

## 验证用例与预期结果

### 测试 1：编译

```bash
cd guide/phase_3/src
mkdir -p build && cd build
cmake ..
make
```

### 测试 2：验证连接数上限

```bash
gdb ./build/test_connpool
(gdb) watch m_CurConn
(gdb) run
# 每次 m_CurConn 变化时暂停，确认不超过 MaxConn
(gdb) print m_CurConn
```

### 失败排查

| 症状 | 可能原因 |
|------|---------|
| 编译时 `mysql/mysql.h: No such file` | 没装 MySQL 开发库：`sudo apt install libmysqlclient-dev` |
| 运行时 `MySQL Error` | MySQL 服务没启动或数据库/用户不存在 |
| 连接池耗尽，程序卡住 | 有线程获取连接后没归还 |
| `undefined reference to mysql_init` | 链接时没加 `-lmysqlclient` |

---

## C++ 语法速查

| 语法 | 示例 | 说明 |
|------|------|------|
| `static` 局部变量 | `static connection_pool connPool` | 单例，C++11 线程安全初始化 |
| 私有构造函数 | `private: connection_pool()` | 禁止外部实例化 |
| `std::list<T>` | `list<MYSQL*> connList` | 双向链表，O(1) 头尾操作 |
| `string::c_str()` | `url.c_str()` | string → const char* |

---

## 阶段小结

你实现了 `connection_pool`（单例 + 信号量 + 互斥锁）和 `connectionRAII`（自动归还）。

下一阶段：**Phase 4 — 线程池**。
