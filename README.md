# TinyWebServer-start

这是一个跟随 TinyWebServer 思路逐步搭建的 C++ 学习版 WebServer。当前版本已经完成了 HTTP 基础服务器、线程池、日志、MySQL 连接池、注册登录、密码哈希和基础代码分层。

## 当前进度

```text
[###################-] 97%
```

已完成：

```text
HTTP 解析
GET 静态页面
POST 表单处理
epoll + 非阻塞 IO
线程池
日志系统
连接超时关闭
MySQL 连接池
RAII 自动归还数据库连接
注册 / 登录
密码 SHA256 哈希存储
SQL 字符串转义
基础目录结构整理
```

## 目录结构

```text
http-server-v1/
  app/
    main.cpp                  # 程序入口，启动日志、MySQL 连接池、建表、WebServer

  webserver/
    webserver.cpp
    webserver.hpp             # epoll 事件循环、accept、read/write 分发、连接管理

  http/
    http_conn.cpp
    http_conn.hpp             # HTTP 请求解析、响应生成、静态文件和 POST 路由
    auth.cpp
    auth.hpp                  # 表单取值、SQL 转义、密码哈希、注册/登录数据库逻辑

  thread_pool/
    thread_pool.cpp
    thread_pool.hpp           # 线程池：任务队列、worker、condition_variable、shutdown

  db/
    sql_connection_pool.cpp
    sql_connection_pool.hpp   # MySQL 连接池和 Sql_Connection_Guard

  logger/
    logger.cpp
    logger.hpp                # 同步日志，输出到终端和 logs/YYYY-MM-DD.log

  html/
    index.html
    login.html
    register.html
    ...                       # 静态页面和错误页

  tests/
    mysql_pool_test.cpp       # MySQL 连接池测试

  scripts/
    smoke.sh                  # HTTP 冒烟测试
    load.sh                   # 简单压测脚本

  makefile
```

## 构建

进入项目目录：

```bash
cd http-server-v1
```

编译服务器：

```bash
make
```

编译 MySQL 连接池测试：

```bash
make mysql_pool_test
```

清理编译产物：

```bash
make clean
```

## 运行

默认启动：

```bash
./server
```

指定端口、线程数、空闲超时时间：

```bash
./server 8888 4 30
```

参数含义：

```text
8888  -> 监听端口
4     -> 线程池 worker 数量
30    -> 空闲连接超时时间，单位秒
```

浏览器访问：

```text
http://127.0.0.1:8888/
http://127.0.0.1:8888/login
http://127.0.0.1:8888/register
```

## MySQL 准备

当前代码默认使用：

```text
host:     127.0.0.1
port:     3306
user:     tiny
password: tiny123
database: tinywebserver
pool:     4 connections
```

如果是新环境，可以创建数据库和用户：

```bash
sudo mysql
```

```sql
CREATE DATABASE IF NOT EXISTS tinywebserver;
CREATE USER IF NOT EXISTS 'tiny'@'localhost' IDENTIFIED BY 'tiny123';
CREATE USER IF NOT EXISTS 'tiny'@'127.0.0.1' IDENTIFIED BY 'tiny123';
GRANT ALL PRIVILEGES ON tinywebserver.* TO 'tiny'@'localhost';
GRANT ALL PRIVILEGES ON tinywebserver.* TO 'tiny'@'127.0.0.1';
FLUSH PRIVILEGES;
```

服务器启动时会自动执行建表兜底：

```sql
CREATE TABLE IF NOT EXISTS users(
    id INT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(50) NOT NULL UNIQUE,
    password VARCHAR(64) NOT NULL
);
```

`password` 存储的是 SHA256 后的 64 位哈希字符串，不再是明文密码。

## 主要路由

```text
GET  /              首页
GET  /login         登录页面
GET  /register      注册页面
GET  /hello.html    静态页面
GET  /time          静态 time 页面
GET  /threadpool    线程池演示页面

POST /echo          回显请求 body
POST /register      注册用户，写入 MySQL
POST /login         登录用户，校验密码哈希
```

## 注册登录流程

注册：

```text
POST /register
-> 解析 username/password
-> 校验空值
-> SHA256(password)
-> mysql_real_escape_string 转义
-> INSERT INTO users(username, password)
-> 处理用户名重复
```

登录：

```text
POST /login
-> 解析 username/password
-> 查 SELECT password FROM users WHERE username='...'
-> 对用户输入密码做 SHA256
-> 与数据库中的哈希比较
-> 返回 login success 或 username or password wrong
```

## 测试

HTTP 冒烟测试：

```bash
bash scripts/smoke.sh
```

MySQL 连接池测试：

```bash
make mysql_pool_test
./mysql_pool_test
```

简单压测：

```bash
bash scripts/load.sh 500 50 /hello.html
```

参数含义：

```text
500         总请求数
50          并发数
/hello.html 请求路径
```

## 已学习知识点

```text
HTTP 请求行、请求头、请求体
epoll 多路复用
非阻塞 socket
线程池 worker / task queue / mutex / condition_variable
join / notify_one / notify_all
RAII 资源管理
MySQL C API
MySQL 连接池
SQL 注入基础防护
密码哈希存储
Makefile 编译组织
项目按模块拆分
```

