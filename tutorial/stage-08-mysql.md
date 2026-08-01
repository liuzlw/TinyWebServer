# Stage 8 MySQL 连接池与注册登录

> 项目"完整功能"的最后一块:数据库连接池 + 用户注册/登录。Stage 0 建的 `qgydb` 库和 `user` 表终于派上用场。

## 1. 本阶段目标

- [ ] 理解**数据库连接池**(复用连接、避免反复建连)与单例模式
- [ ] 理解 `connectionRAII`(C3 RAII 的实战:用完连接自动还回池子)
- [ ] 给 http_conn 加上真正的注册 / 登录校验
- [ ] 浏览器注册 → 数据库出现记录 → 登录成功跳转

**最终效果:** 在浏览器注册一个新账号,`mysql` 命令行能查到它;再用它登录,跳转到欢迎页。

> ⚠️ **前置条件**:本阶段需要 MySQL 已装好、`qgydb` 库和 `user` 表已建(Stage 0 做过)。没做的话先回去补。

## 2. 前置知识

- C3:RAII(本章 `connectionRAII` 就是它的实战)
- C5:锁、信号量;S5:http_conn 状态机
- 新增:MySQL C API、连接池思想

## 3. 问题:S5 的"注册/登录"是假的

S5 的 `/0`(注册页)、`/1`(登录页)只是**静态页面**——表单提交后没人真正校验。本阶段把它变成真功能:

```text
浏览器填用户名密码 → POST 提交 → 服务器查数据库:
  ├─ 注册:表里没有这名字 → INSERT 进 user 表 → 去登录页
  └─ 登录:表里名字密码都对 → 去欢迎页;不对 → 错误页
```

## 4. MySQL C API:最基础的四步

先认识本阶段用到的 C 接口(项目里通过连接池拿连接,然后):

```cpp
#include <mysql/mysql.h>

MYSQL *mysql = mysql_init(NULL);                    // 1. 初始化一个连接句柄
mysql_real_connect(mysql, "localhost", "root", "root", "qgydb", 3306, NULL, 0);  // 2. 连接
mysql_query(mysql, "SELECT username,passwd FROM user");  // 3. 执行 SQL
MYSQL_RES *res = mysql_store_result(mysql);          // 4a. 取结果集
MYSQL_ROW row = mysql_fetch_row(res);                // 4b. 取一行(row[0],row[1]...)
mysql_close(mysql);                                  // 5. 关闭
```

- `mysql_query` 执行 SQL;`mysql_error(mysql)` 拿错误信息
- 结果集用 `mysql_fetch_row` 一行行取,`row[i]` 是第 i 列(字符串)
- 编译链接必须加 `-lmysqlclient`(Stage 0 装了 `libmysqlclient-dev`)

## 5. 为什么需要连接池

每次请求都 `mysql_real_connect` 建连接 → 网络往返 + 认证,开销大。**连接池**的做法:启动时一次性建好 N 个连接,用的时候"借"、用完"还",全程复用。

```text
启动: 建 8 个 MySQL 连接,存进池子
请求: 从池子"借"一个 → 用 → "还"回池子
并发: 谁借到谁用,借不到就等(信号量控制)
```

**单例模式**:整个服务器只需要一个连接池,用 `GetInstance()` 保证唯一。

## 6. CGImysql/sql_connection_pool.h + .cpp

在 `my_tiny_webserver/` 下新建 `CGImysql/sql_connection_pool.h`(**与原项目一致**):

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

class connection_pool
{
public:
	MYSQL *GetConnection();				 //获取数据库连接
	bool ReleaseConnection(MYSQL *conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	//单例模式
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log); 

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	locker lock;
	list<MYSQL *> connList; //连接池
	sem reserve;            //信号量:控制"还能借出几个"
};

class connectionRAII{   // RAII 包装:构造借连接,析构还连接

public:
	connectionRAII(MYSQL **con, connection_pool *connPool);
	~connectionRAII();
	
private:
	MYSQL *conRAII;
	connection_pool *poolRAII;
};

#endif
```

在 `my_tiny_webserver/CGImysql/` 下新建 `sql_connection_pool.cpp`(**与原项目一致**):

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
	static connection_pool connPool;    // 单例:整个程序一个池子
	return &connPool;
}

//构造初始化:一次性建好 MaxConn 个连接
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
		con = mysql_init(con);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);
		}
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);
		if (con == NULL)
		{
			LOG_ERROR("MySQL Error");
			exit(1);                    // 连不上数据库直接退出(别硬跑)
		}
		connList.push_back(con);
		++m_FreeConn;
	}

	reserve = sem(m_FreeConn);          // 信号量初值 = 空闲连接数

	m_MaxConn = m_FreeConn;
}

//借连接:信号量 wait 保证"没空闲连接就等"
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

	reserve.wait();                     // 拿一个"名额",没有就阻塞

	lock.lock();
	con = connList.front();
	connList.pop_front();
	--m_FreeConn;
	++m_CurConn;
	lock.unlock();
	return con;
}

//还连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();
	connList.push_back(con);
	++m_FreeConn;
	--m_CurConn;
	lock.unlock();

	reserve.post();                     // 释放一个"名额",唤醒等待者
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{
	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		connList.clear();
	}
	lock.unlock();
}

int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

//RAII:构造时从池子借一个连接,析构时自动还
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}
```

**连接池的核心,就两把"锁":**

| 组件 | 作用 |
|---|---|
| `locker lock` | 互斥锁:保护 `connList` 列表(借/还时不能同时操作) |
| `sem reserve` | 信号量:记录"还能借出几个"。初值 = 空闲连接数;借一个 `wait()`(-1),还一个 `post()`(+1)。**没空闲连接时,`wait` 阻塞,调用方等着** |

**`connectionRAII` 就是 C3 学的 RAII 实战:**

```cpp
{
    connectionRAII mysqlcon(&users[sockfd].mysql, m_connPool);
    // 构造:从池子借一个连接,存进 users[sockfd].mysql
    users[sockfd].process();          // 处理期间用这个连接
}                                     // 析构:连接自动还回池子(绝不忘记还!)
```

## 7. http_conn 升级:加载用户表 + 注册/登录逻辑

S5 的 http_conn 把 MySQL 部分删掉了,现在补回来。**改动集中在 3 处:**

### ① http_conn.h:加回 MySQL 相关

在 include 区加:

```cpp
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
```

在 public 方法区加:

```cpp
void initmysql_result(connection_pool *connPool);   // 把 user 表加载进内存
```

`init` 签名改回原版(多了数据库参数):

```cpp
void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
          int close_log, string user, string passwd, string sqlname);
```

成员加(public 区):

```cpp
MYSQL *mysql;              // 当前连接用的数据库连接
```

(private 区):

```cpp
map<string, string> m_users;   // 用户名→密码 内存表
int m_TRIGMode;
int m_close_log;
char sql_user[100];
char sql_passwd[100];
char sql_name[100];
```

### ② http_conn.cpp:顶部加全局 + 实现 initmysql_result

```cpp
// 全局:注册/登录校验用的用户名密码表 + 保护它的锁
locker m_lock;
map<string, string> users;

//从数据库加载全部用户名密码到 users 表
void http_conn::initmysql_result(connection_pool *connPool)
{
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);      // 借一个连接

    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        printf("SELECT error:%s\n", mysql_error(mysql));
    }

    MYSQL_RES *result = mysql_store_result(mysql);  // 结果集
    int num_fields = mysql_num_fields(result);
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    while (MYSQL_ROW row = mysql_fetch_row(result)) // 一行行读
    {
        string temp1(row[0]);   // username
        string temp2(row[1]);   // passwd
        users[temp1] = temp2;   // 存进内存表
    }
}
```

> **为什么启动时把整个 user 表加载进内存?** 登录/注册查 `users` map 比每次查数据库快得多。注册时新增的行也会同步插进 map(见下方 do_request)。

### ③ http_conn.cpp:`do_request` 恢复 cgi 注册/登录分支

S5 里这段被删了,现在把原版加回到 `do_request` 的 `strrchr(m_url, '/')` 之后:

```cpp
    //处理cgi:登录(/2CGISQL.cgi)和注册(/3CGISQL.cgi)的 POST 请求
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //从 body 里提取用户名密码:格式 user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //注册:表里没这名字 → 插入数据库
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())   // 没有同名
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);          // 插入数据库
                users.insert(pair<string, string>(name, password)); // 更新内存表
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");        // 成功 → 去登录页
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");  // 重名 → 失败页
        }
        else if (*(p + 1) == '2')
        {
            //登录:名字密码都对 → 欢迎页
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
```

**这段代码的"骨架"一目了然:**

```text
POST /2CGISQL.cgi (登录) → 查 users[name] == password ? 欢迎页 : 错误页
POST /3CGISQL.cgi (注册) → users 里没这名字? INSERT + 更新内存表 : 重名错误页
```

`m_string` 是 S5 状态机解析出的 POST body(`user=xxx&passwd=xxx`),这里手动按 `&` 拆出用户名和密码。

## 8. main.cpp:初始化连接池 + 处理前借连接

**在 S7 的 main.cpp 基础上改 4 处:**

**① include 加:**

```cpp
#include "CGImysql/sql_connection_pool.h"
```

**② 全局加一个连接池指针:**

```cpp
connection_pool *m_connPool;
```

**③ 启动时初始化连接池(在 `init_log()` 之后):**

```cpp
void init_sql()
{
    m_connPool = connection_pool::GetInstance();
    // 参数: 主机, 用户, 密码, 库名, 端口, 连接数, 日志开关
    m_connPool->init("localhost", "root", "root", "qgydb", 3306, 8, 0);
    users[0].initmysql_result(m_connPool);   // 把 user 表加载进内存
    printf("数据库连接池初始化完成\n");
}
// main 里: init_log();  init_sql();  放在创建连接之前
```

**④ accept 里 `init` 传数据库参数:**

```cpp
users[connfd].init(connfd, client_address, root, TRIGMode, m_close_log, "root", "root", "qgydb");
```

**⑤ 处理请求前,给这个 http_conn 借一个数据库连接:**

```cpp
else if (events[i].events & EPOLLIN)
{
    if (users[sockfd].read_once())
    {
        adjust_timer(sockfd);
        // 处理前从连接池取一个连接(RAII:这个作用域结束时自动还回池子)
        connectionRAII mysqlcon(&users[sockfd].mysql, m_connPool);
        users[sockfd].process();
        LOG_INFO("处理连接 %d 的请求", sockfd);
    }
    ...
}
```

> `connectionRAII` 借出连接 → `process()` 里 `do_request` 用 `users[sockfd].mysql` 查库 → 处理完离开作用域,**连接自动还回池子**。即使代码中间 return,析构也会执行——这就是 RAII 的意义。

## 9. 编译与运行

更新 `CMakeLists.txt`(加入 `CGImysql/sql_connection_pool.cpp`,并链接 `mysqlclient`):

```cmake
cmake_minimum_required(VERSION 3.20)
project(webserver)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(server main.cpp http/http_conn.cpp timer/lst_timer.cpp log/log.cpp CGImysql/sql_connection_pool.cpp)
target_link_libraries(server pthread mysqlclient)
```

```bash
cd ~/TinyWebServer/my_tiny_webserver
cmake -S . -B build
cmake --build build
```

**先确认 MySQL 在跑:**

```bash
sudo service mysql status          # 应该显示 mysql is running
mysql -u root -proot -e "USE qgydb; SHOW TABLES;"   # 应有 user 表
```

**启动服务器:**

```bash
./build/server
```

**预期输出:**

```text
数据库连接池初始化完成
服务器已启动, 监听端口 9006
```

## 10. 验收清单

| # | 验证操作 | 预期结果 | 通过 |
|---|---|---|---|
| 1 | 启动服务器 | 输出 `数据库连接池初始化完成`,无报错 | ☐ |
| 2 | 浏览器打开 `http://127.0.0.1:9006/0`(注册页),填新用户名密码提交 | 提示注册成功,跳转登录页 | ☐ |
| 3 | `mysql -u root -proot qgydb -e "SELECT * FROM user;"` | **能看到刚注册的用户名和密码** | ☐ |
| 4 | 用刚注册的账号在 `/1`(登录页)登录 | 跳转 `welcome.html` 欢迎页 | ☐ |
| 5 | 再注册一个**同名**用户 | 提示注册失败(registerError) | ☐ |
| 6 | 登录时输错密码 | 跳转 `logError.html` | ☐ |
| 7 | 注册/登录后重启服务器 | 之前注册的账号仍能登录(数据已持久化) | ☐ |
| 8 | `curl -X POST -d "user=abc&passwd=123" http://127.0.0.1:9006/3CGISQL.cgi` | 返回 log.html(注册成功跳登录页) | ☐ |

> 第 7 条验证了**数据真的写进了数据库**(重启后 `users` map 从数据库重新加载,账号还在)。

## 11. 调试技巧

### 看注册后数据库有没有写进去

```bash
mysql -u root -proot qgydb -e "SELECT * FROM user;"
```

### gdb 断在注册分支

```bash
gdb ./build/server
```

```text
(gdb) break http_conn.cpp:420        ← 断在 do_request 的注册分支(以实际行号为准)
(gdb) run
(gdb) print name                      ← 浏览器提交后,看解析出的用户名
(gdb) print password
```

### 排查连不上数据库

先单独测 C API 能否连上:

```bash
mysql -u root -proot qgydb -e "SELECT 1;"
```

## 12. 常见坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 编译报 `mysql/mysql.h: No such file` | 没装 `libmysqlclient-dev` | `sudo apt install -y libmysqlclient-dev`(Stage 0 应已装) |
| 链接报 `undefined reference to mysql_query` | 忘了 `-lmysqlclient` | `target_link_libraries(server pthread mysqlclient)` |
| 启动报 `MySQL Error` 然后退出 | 连不上数据库 | `sudo service mysql start`;检查账号密码/库名 |
| `Access denied for user 'root'` | root 密码不是 root | 按 Stage 0 重新 `ALTER USER` 设置 |
| 注册成功但数据库查不到 | 表名/字段不对 | `DESCRIBE user;` 确认有 `username`/`passwd` 两列 |
| 注册/登录没反应 | `mysql` 连接没借到 | 确认 `connectionRAII` 加在了 `process()` 之前 |
| 登录总是跳 logError | 内存表 `users` 没加载 | 确认 `init_sql()` 里调了 `users[0].initmysql_result(m_connPool)` |

## 13. 与原项目对照

| 本阶段 | 原项目 |
|---|---|
| `CGImysql/sql_connection_pool.h` + `.cpp` | **逐字一致** |
| `http_conn` 的 initmysql_result + cgi 分支 | 与原项目 `http_conn.cpp` **对应位置逐字一致** |
| main 里的 `init_sql` + `connectionRAII` | 对应 `webserver.cpp` 的 `sql_pool()` 与 `threadpool` 的 Reactor 分支 |

> **diff 对照**:
> ```bash
> diff my_tiny_webserver/CGImysql/sql_connection_pool.cpp CGImysql/sql_connection_pool.cpp
> ```
> 本阶段完成后,`http_conn` 距离原版只剩 `LOG_*` 日志调用和 `timer_flag` 等字段的差异(S9 换原版)。

## 14. 下一步

进入 **[Stage 9 整合与压力测试](stage-09-integration.md)**——最后一步:用原版完整代码组装出真正的 TinyWebServer(main + config + webserver),支持命令行参数,并用 webbench 压测。
