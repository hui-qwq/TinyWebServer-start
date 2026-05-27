# TinyWebServer

基于 C++17 实现的高性能 Web 服务器，内置博客系统。使用 epoll + 线程池 + MySQL 连接池架构。

## 架构

```
                    ┌─────────────┐
                    │   Client    │
                    └──────┬──────┘
                           │ HTTP
                    ┌──────▼──────┐
                    │   epoll     │  ← 事件驱动 I/O
                    │  (ET 模式)  │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼───┐  ┌─────▼────┐ ┌────▼───┐
         │ Accept │  │  Read    │ │ Write  │
         └────┬───┘  └─────┬────┘ └────┬───┘
              │            │            │
              └────────────┼────────────┘
                           │
                    ┌──────▼──────┐
                    │  ThreadPool │  ← 工作线程池
                    │  (4 线程)   │
                    └──────┬──────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼───┐  ┌─────▼────┐ ┌────▼───┐
         │  GET   │  │  POST    │ │ Static │
         │ Router │  │  Router  │ │ Files  │
         └────┬───┘  └─────┬────┘ └────┬───┘
              │            │            │
              └────────────┼────────────┘
                           │
              ┌────────────┼────────────┐
              │            │            │
         ┌────▼───┐  ┌─────▼────┐ ┌────▼───┐
         │  Auth  │  │  Blog    │ │  Cache  │
         │ PBKDF2 │  │  CRUD    │ │ (内存)  │
         └────┬───┘  └─────┬────┘ └─────────┘
              │            │
              └────────────┼────────────┐
                           │            │
                    ┌──────▼──┐  ┌──────▼──┐
                    │  MySQL  │  │ Logger  │
                    │  Pool   │  │ (异步)  │
                    └─────────┘  └─────────┘
```

## 特性

- **epoll 事件驱动** — ET 模式，批量 accept，支持高并发连接
- **线程池异步处理** — I/O 线程与工作线程分离，避免阻塞事件循环
- **HTTP/1.1 Keep-Alive** — 支持长连接和请求管线化（pipelining）
- **MySQL 连接池** — RAII 管理连接生命周期，prepared statement 防 SQL 注入
- **密码安全** — PBKDF2-HMAC-SHA256，10 万次迭代，随机盐
- **异步日志** — 独立线程写入，按天滚动，支持 stdout 同步输出
- **静态文件缓存** — 内存缓存热点文件，减少磁盘 I/O
- **Session 管理** — token cookie 实现登录态，HttpOnly 防 XSS
- **Markdown 博客** — 后端存取原文，前端 marked.js 渲染，编辑器实时预览

## 快速开始

### 依赖

- GCC 9+ (C++17)
- MySQL 8.0+ 或 MariaDB 10.3+
- OpenSSL 1.1+
- CMake 3.10+

```bash
# Ubuntu/Debian
sudo apt install g++ cmake libmysqlclient-dev libssl-dev
```

### 数据库

```sql
CREATE DATABASE tinywebserver;
CREATE USER 'tiny'@'localhost' IDENTIFIED BY 'tiny123';
GRANT ALL ON tinywebserver.* TO 'tiny'@'localhost';
```

### 构建

```bash
cmake -B build
cmake --build build
```

### 运行

```bash
# 使用默认配置
./build/server

# 自定义端口和线程数
./build/server 8080 8 60

# 使用环境变量（生产环境推荐）
DB_PASSWORD=secret ./build/server
```

### 环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `DB_HOST` | 127.0.0.1 | MySQL 地址 |
| `DB_PORT` | 3306 | MySQL 端口 |
| `DB_USER` | tiny | 用户名 |
| `DB_PASSWORD` | tiny123 | 密码 |
| `DB_NAME` | tinywebserver | 数据库名 |
| `DB_POOL_SIZE` | 4 | 连接池大小 |

## 路由

### 基础页面

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 首页 |
| GET | `/time` | 时间页 |
| GET | `/hello.html` | Hello 页 |
| GET/POST | `/login` | 登录 |
| GET/POST | `/register` | 注册 |
| GET/POST | `/echo` | POST 回显测试 |
| GET | `/threadpool.html` | 线程池压测工具 |

### 博客

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/blog` | 文章列表 |
| GET | `/blog/42` | 文章详情（Markdown 渲染） |
| GET | `/blog/new` | 新建文章（需登录） |
| POST | `/blog` | 创建文章 |
| GET | `/blog/42/edit` | 编辑文章（需登录） |
| POST | `/blog/42/edit` | 更新文章 |
| POST | `/blog/42/delete` | 删除文章 |

## 测试

```bash
# Makefile 方式
make test

# CMake 方式
cd build && ctest --output-on-failure
```

## 设计决策

**为什么 epoll 而不是 select/poll？**
select/poll 每次调用需要 O(n) 遍历全部 fd，连接数上万时 CPU 浪费严重。epoll 基于事件通知，只返回就绪的 fd，复杂度 O(1)。

**为什么线程池而不是每个请求一个线程？**
频繁创建/销毁线程代价高，线程数无上限可能导致系统资源耗尽。线程池复用固定数量的线程，任务队列削峰填谷。

**为什么 prepared statement？**
用户输入直接拼 SQL 字符串有 SQL 注入风险。prepared statement 将 SQL 结构与参数值分离传输，从根本上杜绝注入。

**为什么 PBKDF2 而不是 SHA-256？**
SHA-256 是快速哈希，GPU 每秒可计算数十亿次。PBKDF2 通过 10 万次迭代大幅降低暴力破解速度，是 NIST 推荐的密码存储方案。

## 项目结构

```
.
├── app/main.cpp              # 入口：初始化 DB、启动服务器
├── webserver/                # epoll 事件循环 + 连接管理
│   ├── webserver.hpp
│   └── webserver.cpp
├── http/                     # HTTP 协议处理
│   ├── http_conn.hpp
│   ├── http_conn.cpp         # 请求解析、路由、响应
│   ├── auth.hpp
│   ├── auth.cpp              # 注册、登录、PBKDF2
│   └── template.hpp          # 模板引擎、URL/HTML/JSON 工具
├── thread_pool/              # 工作线程池
│   ├── thread_pool.hpp
│   └── thread_pool.cpp
├── db/                       # MySQL 连接池
│   ├── sql_connection_pool.hpp
│   └── sql_connection_pool.cpp
├── logger/                   # 异步日志
│   ├── logger.hpp
│   └── logger.cpp
├── html/                     # 前端静态资源
│   ├── index.html
│   ├── blog/                 # 博客模板
│   │   ├── list.html
│   │   ├── detail.html
│   │   ├── editor.html
│   │   ├── blog.css
│   │   └── blog.js
│   └── marked.min.js         # Markdown 渲染库
├── tests/                    # 单元测试
│   └── test_utils.cpp
├── CMakeLists.txt
└── README.md
```

## 路线图

- [ ] HTTP 缓存头（ETag / Last-Modified）
- [ ] 优雅关闭（SIGTERM）
- [ ] 配置文件支持（TOML/YAML）
- [ ] 请求频率限制
- [ ] 文章标签和搜索
- [ ] Docker 部署支持
