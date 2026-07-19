# Stage 8：MySQL 连接池与注册登录

> 🎯 **本阶段目标**：实现 `CGImysql/` 模块 —— MySQL 连接池 + RAII 连接管理，
> 并让服务器支持 **POST 请求**，完成浏览器端的注册、登录功能。
> 至此，项目的所有业务功能齐备。

## 📚 理论铺垫

### 8.1 为什么需要数据库连接池？

登录/注册要查 MySQL。朴素做法：每次请求 `mysql_real_connect()` 建一条新连接，
用完 `mysql_close()`。问题：**建 TCP 连接 + MySQL 握手认证非常耗时**（毫秒级），
高并发下每个请求都付一次这个成本，数据库很快被打爆。

连接池的思路（和线程池一模一样）：
启动时预先建好 N 条（如 8 条）连接放进池子，请求来了**借**一条，用完**还**回去：

```
         ┌────────────── pool (list<MYSQL*>) ──────────────┐
请求 A ──→│ 取出连接 → 执行 SQL → 归还连接                   │←── 请求 B（复用同一条）
         └──────────────────────────────────────────────────┘
         信号量 reserve 记录"池里还剩几条"，取完了就排队等
```

### 8.2 RAII 管理连接：再也不怕忘记归还

Stage 3 讲过 RAII。数据库连接是绝佳的应用场景：

```cpp
{
    connectionRAII guard(&conn, pool);   // 构造：从池子取出连接
    mysql_query(conn, "SELECT ...");     // 随便用
    // 即使这里 return 了、抛异常了……
}   // 析构：自动归还连接。永不泄漏！
```

没有 RAII 的话，任何一条提前 return 的路径都会让连接有借无还，池子很快漏干。

### 8.3 注册/登录的 Web 交互流程

浏览器端的交互本质是一组 URL 约定（看 root/ 下的 html 源码）：

```
GET  /            → 欢迎页 welcome.html（登录/注册两个入口）
GET  /0           → 跳注册页 register.html
GET  /1           → 跳登录页 log.html
POST /2CGISQL.cgi → 登录校验：body 里带 user=xxx&password=yyy
POST /3CGISQL.cgi → 注册校验：同上，插入新用户
```

服务器启动时把 user 表**全量加载进内存 map**（`users`），
校验时查内存（快！），注册时同时写数据库 + 更新内存。

> ⚠️ 注意：本项目是**教学项目**，密码明文存储、SQL 直接字符串拼接。
> 生产环境必须密码哈希 + 参数化查询（防 SQL 注入），这一点面试要能说出来。

### 8.4 POST 请求解析

POST 和 GET 的区别在消息体：

```http
POST /2CGISQL.cgi HTTP/1.1\r\n
Host: 127.0.0.1:9006\r\n
Content-Length: 23\r\n       ← 靠这个长度读 body
\r\n
user=name&password=passwd    ← 消息体：表单数据
```

Stage 5 的状态机里 `CHECK_STATE_CONTENT` + `parse_content` 就是为它准备的。
消息体格式是 `key1=value1&key2=value2`（application/x-www-form-urlencoded）。

## 💻 本阶段 C++ 知识点

| 知识点 | 在哪用到 |
|--------|----------|
| `std::map<std::string, std::string>` | 内存中的用户表 |
| `std::list<MYSQL*>` | 连接池容器 |
| 单例模式（再次实战） | `connection_pool::GetInstance()` |
| 信号量实战 | 连接池的 `reserve` |
| C API 调用（mysqlclient） | `mysql_real_connect`/`mysql_query`/`mysql_store_result` |
| 字符串解析 `strchr`/`strtok` 思路 | 解析 user=xxx&password=yyy |

## 🔨 动手实现

### 8.1 准备数据库（若 Stage 0 已完成可跳过）

```bash
sudo service mysql start
mysql -uroot -proot -e "
CREATE DATABASE IF NOT EXISTS yourdb;
USE yourdb;
CREATE TABLE IF NOT EXISTS user(username char(50) NULL, passwd char(50) NULL)ENGINE=InnoDB;
INSERT INTO user(username, passwd) VALUES('name', 'passwd');"
```

### 8.2 `CGImysql/sql_connection_pool.h`

```cpp
#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include <stdio.h>
#include <list>
#include <mysql/mysql.h>
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"

class connection_pool {
public:
    MYSQL* GetConnection();               // 借连接
    bool ReleaseConnection(MYSQL* conn);  // 还连接
    int GetFreeConn();                    // 剩余连接数
    void DestroyPool();                   // 销毁池子

    static connection_pool* GetInstance() {
        static connection_pool connPool;
        return &connPool;
    }

    void init(std::string url, std::string User, std::string PassWord,
              std::string DataBaseName, int Port, int MaxConn, int close_log);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;          // 池大小
    int m_CurConn;          // 已借出数
    int m_FreeConn;         // 空闲数
    locker lock;
    std::list<MYSQL*> connList;
    sem reserve;            // ★ 信号量 = 空闲连接数

    std::string m_url;
    std::string m_Port;
    std::string m_User;
    std::string m_PassWord;
    std::string m_DatabaseName;
    int m_close_log;
};

// RAII 包装：构造借、析构还
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

### 8.3 `CGImysql/sql_connection_pool.cpp`

```cpp
#include "sql_connection_pool.h"

connection_pool::connection_pool() {
    m_CurConn = 0;
    m_FreeConn = 0;
}

connection_pool* connection_pool::GetInstance() {
    static connection_pool connPool;
    return &connPool;
}

void connection_pool::init(std::string url, std::string User, std::string PassWord,
                           std::string DataBaseName, int Port, int MaxConn, int close_log) {
    m_url = url;
    m_Port = std::to_string(Port);
    m_User = User;
    m_PassWord = PassWord;
    m_DatabaseName = DataBaseName;
    m_close_log = close_log;

    for (int i = 0; i < MaxConn; i++) {
        MYSQL* con = NULL;
        con = mysql_init(con);
        if (con == NULL) {
            LOG_ERROR("MySQL Error: init failed");
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(),
                                 DataBaseName.c_str(), Port, NULL, 0);
        if (con == NULL) {
            LOG_ERROR("MySQL Error: connect failed");
            exit(1);
        }
        connList.push_back(con);
        ++m_FreeConn;
    }

    reserve = sem(m_FreeConn);       // 信号量初始值 = 连接数
    m_MaxConn = m_FreeConn;
}

MYSQL* connection_pool::GetConnection() {
    MYSQL* con = NULL;
    if (0 == connList.size()) return NULL;

    reserve.wait();                  // ★ 没空闲连接就阻塞等
    lock.lock();
    con = connList.front();
    connList.pop_front();
    --m_FreeConn;
    ++m_CurConn;
    lock.unlock();
    return con;
}

bool connection_pool::ReleaseConnection(MYSQL* con) {
    if (NULL == con) return false;
    lock.lock();
    connList.push_back(con);
    ++m_FreeConn;
    --m_CurConn;
    lock.unlock();
    reserve.post();                  // ★ 唤醒等待中的借用人
    return true;
}

void connection_pool::DestroyPool() {
    lock.lock();
    if (connList.size() > 0) {
        for (auto con : connList) mysql_close(con);
        m_CurConn = 0;
        m_FreeConn = 0;
        connList.clear();
    }
    lock.unlock();
}

int connection_pool::GetFreeConn() { return this->m_FreeConn; }

connection_pool::~connection_pool() { DestroyPool(); }

// RAII：借还全自动
connectionRAII::connectionRAII(MYSQL** SQL, connection_pool* connPool) {
    *SQL = connPool->GetConnection();
    conRAII = *SQL;
    poolRAII = connPool;
}

connectionRAII::~connectionRAII() { poolRAII->ReleaseConnection(conRAII); }
```

> 🔑 注意 `GetConnection` 的顺序：**先 `reserve.wait()` 再 `lock.lock()`**。
> 信号量管「有没有」，互斥锁管「取得安全」—— 和线程池里的模式完全一致。

### 8.4 http_conn 集成数据库

在 `http_conn` 中增加（对照原始项目 `http_conn.cpp` 的 do_request 部分）：

```cpp
// http_conn.h 增加
public:
    void initmysql_result(connection_pool* connPool);   // 启动时加载用户表
    static std::map<std::string, std::string> m_users;  // 内存用户表
private:
    char* m_string;   // POST 消息体
```

```cpp
// http_conn.cpp
std::map<std::string, std::string> http_conn::m_users;

// 服务器启动时：把整个 user 表读进 map
void http_conn::initmysql_result(connection_pool* connPool) {
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }
    MYSQL_RES* result = mysql_store_result(mysql);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        m_users[row[0]] = row[1];
    }
    mysql_free_result(result);
}
```

`do_request()` 增加 CGI 分支（登录/注册）：

```cpp
const char* p = strrchr(m_url, '/');
// cgi=2 登录，cgi=3 注册（URL 形如 /2CGISQL.cgi）
if (*(p + 1) == '2' || *(p + 1) == '3') {
    char flag = m_url[1];                       // '2' or '3'
    // 从消息体解析 user=xxx&password=yyy
    char name[100], password[100];
    int i;
    for (i = 5; m_string[i] != '&'; ++i) name[i - 5] = m_string[i];
    name[i - 5] = '\0';
    int j = 0;
    for (i = i + 10; m_string[i] != '\0'; ++i, ++j) password[j] = m_string[i];
    password[j] = '\0';

    if (*(p + 1) == '3') {
        // 注册：用户不存在才插入
        if (m_users.find(name) == m_users.end()) {
            char* sql_insert = (char*)malloc(200);
            sprintf(sql_insert, "INSERT INTO user(username, passwd) VALUES('%s','%s')",
                    name, password);
            MYSQL* mysql = NULL;
            connectionRAII mysqlcon(&mysql, m_connPool);   // ★ RAII 借还
            int res = mysql_query(mysql, sql_insert);
            if (!res) { m_users[name] = password; strcpy(m_url, "/log.html"); }
            else strcpy(m_url, "/registerError.html");
            free(sql_insert);
        } else {
            strcpy(m_url, "/registerError.html");
        }
    } else {
        // 登录：查内存 map
        if (m_users.find(name) != m_users.end() && m_users[name] == password)
            strcpy(m_url, "/welcome.html");
        else
            strcpy(m_url, "/logError.html");
    }
}
// 之后是原有的 GET 分支：/0 → register.html，/1 → log.html ……
```

同时 `parse_content` 保存消息体：

```cpp
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        m_string = text;            // 保存 POST body
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
```

### 8.5 main.cpp 初始化连接池

```cpp
// main 开头
connection_pool* connPool = connection_pool::GetInstance();
connPool->init("localhost", "root", "root", "yourdb", 3306, 8, close_log);

// http_conn 需要一个静态的池指针 + 加载用户表
http_conn::m_connPool = connPool;     // （在 http_conn 中加这个 static 成员）
users[0].initmysql_result(connPool);  // 通过任意一个对象调用即可（操作的是 static map）
```

CMakeLists.txt 更新 —— 需要找到并链接 mysqlclient：

```cmake
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIB mysqlclient)

add_executable(server
    main.cpp
    http/http_conn.cpp
    timer/lst_timer.cpp
    log/log.cpp
    CGImysql/sql_connection_pool.cpp
)

target_include_directories(server PRIVATE ${MYSQL_INCLUDE_DIR})
target_link_libraries(server pthread ${MYSQL_LIB})
```

## ✅ 验证

**验证 1：注册新用户（浏览器）**

```
1. 浏览器打开 http://127.0.0.1:9006/
2. 点 "注册" → 输入新用户名密码 → 提交
3. 期望：跳到登录页（注册成功）
```

数据库侧验证：

```bash
mysql -uroot -proot -e "SELECT * FROM yourdb.user;"
# 期望：看到新插入的用户行
```

**验证 2：登录**

```
登录页输入刚才注册的账号 → 跳到 welcome 页面（能看到图片、视频入口）
故意输错密码 → 跳到 logError 页面
```

**验证 3：重复注册**

```
用已存在的用户名注册 → 跳到 registerError 页面
```

**验证 4：curl 直接验证 POST（不依赖浏览器）**

```bash
# 登录成功：应返回 welcome.html 的内容
curl -s -X POST http://127.0.0.1:9006/2CGISQL.cgi -d "user=name&password=passwd" | head -5

# 密码错误：应返回 logError.html
curl -s -X POST http://127.0.0.1:9006/2CGISQL.cgi -d "user=name&password=wrong" | head -5
```

**验证 5：连接池并发**

```bash
ab -n 1000 -c 50 -p post_data.txt -T "application/x-www-form-urlencoded" \
   http://127.0.0.1:9006/2CGISQL.cgi
# post_data.txt 内容：user=name&password=passwd
# 期望：1000 次登录校验全部成功，服务器不崩
```

## 🐛 常见问题

**Q1: 编译报 `mysql/mysql.h: No such file or directory`？**
开发包没装：`sudo apt install libmysqlclient-dev`。

**Q2: 运行时报 `connect failed`？**
① MySQL 没启动：`sudo service mysql start`
② 用户名/密码/库名不对：对照 8.1 检查
③ WSL 里 MySQL 监听地址问题：一般 localhost 即可

**Q3: 高并发登录时请求卡住或超时？**
连接池耗尽且没有归还 —— 检查每条路径是否都用了 `connectionRAII`（而不是裸
GetConnection 后忘记 Release）。这正是 RAII 存在的意义。

**Q4: 注册成功但登录失败？**
内存 map 和数据库不同步：注册分支忘了 `m_users[name] = password`，
或服务器重启后 `initmysql_result` 没被调用。

## 🤔 思考与练习

1. 压测时观察日志，统计 MySQL 连接被复用了多少次（在 GetConnection 里加计数）。
2. 把连接池大小改成 1，压测 50 并发，观察现象 —— 理解池大小的意义。
3. **SQL 注入实验**：登录时用户名输入 `' or '1'='1`，密码任意，会发生什么？
   为什么？（看 sprintf 拼出来的 SQL 就明白了）生产中怎么防？
4. 面试题：连接池为什么用信号量而不用条件变量？RAII 解决什么问题？
   如果析构时池子已经销毁了怎么办？（单例的 static 析构顺序问题，深入思考）
5. 拓展（选做）：给密码加 MD5 哈希存储（`<openssl/md5.h>`），体会明文存储的风险。

---

➡️ 下一阶段：[Stage 9：整合、命令行参数与压力测试](stage-09-integration.md)
