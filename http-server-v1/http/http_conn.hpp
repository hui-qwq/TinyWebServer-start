#pragma once

#include <cstddef>
#include <string>
#include <utility>

// 请求校验结果
enum class VerifyResult { OK, BadRequest, NotAllowed };
// 非阻塞 IO 的读写状态
enum class IOState { READY, AGAIN, CLOSED, ERROR, HEAD_TOO_LARGE, BODY_TOO_LARGE};

struct Request {
    // 请求行：GET /index.html HTTP/1.1
    std::string method;
    std::string url;
    std::string version;
    // 常用请求头
    std::string host;
    std::string connection;

    size_t content_length = 0;
    std::string body;
};

class HttpConn {
public:
    HttpConn();

    // 绑定连接 fd，并重置连接内状态
    void init(int fd);
    // 主动关闭连接并清空缓冲
    void close_conn();

    // 从 socket 读取请求数据（非阻塞）
    IOState read_once();
    // 把响应写回客户端（非阻塞）
    IOState write();
    // 解析请求并生成响应
    bool process();
    bool has_complete_request() const;
    void set_413_response();
    void set_431_response();

    int fd() const;
    bool keep_alive() const;
    const std::string& last_method() const;
    const std::string& last_url() const;
    const std::string& last_status() const;
    size_t last_body_bytes() const;
    void reset_for_next_request();

private:
    bool handle_post(Request& req);
    bool handle_get(Request& req);
    bool handle_register(Request& req);
    bool handle_login(Request& req);
    // 解析请求行 + 部分请求头
    Request parse_request(const std::string& msg) const;
    // 校验请求合法性，并产出错误类型
    VerifyResult validate_request(Request& req) const;
    // 组装 HTTP 响应报文
    std::string make_response(const std::string& status,
                              const std::string& type,
                              const std::string& body) const;

    // 按 URL 扩展名推断 Content-Type
    std::string get_content_type(const std::string& url) const;
    // 读取静态文件内容，失败返回空串
    std::string read_file(const std::string& filename) const;
    // URL 映射到本地文件路径
    std::string get_file_path(const std::string& url) const;
    // 路由：动态接口或静态文件
    std::pair<std::string, std::string> route(const std::string& url) const;
    // 统一设置错误响应
    void set_error_response(const std::string& status,
                            const std::string& html_file,
                            const std::string& fallback_html,
                            bool force_close);
    // 统一设置一段 HTML 响应
    void set_html_response(const std::string& status,
                           const std::string& html);
    // 统一设置 400 响应
    void set_400_response();
    // 统一设置 405 响应
    void set_405_response();
    // 统一设置 404 响应
    void set_404_response();
    

private:
    int fd_;
    std::string read_buf_;
    std::string write_buf_;
    // 已发送字节数，用于处理分段发送
    size_t bytes_sent_;
    // 当前请求是否启用 keep-alive
    bool keep_alive_;
    // 最近一次请求/响应摘要，供统一日志输出
    std::string last_method_;
    std::string last_url_;
    std::string last_status_;
    size_t last_body_bytes_;
    // 站点根目录
    std::string root_;
};
