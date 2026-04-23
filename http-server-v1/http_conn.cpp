#include "http_conn.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
size_t HEADLIMIT = 8*1024;
size_t BODYLIMIT = 1024*1024;

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}
}  // namespace

HttpConn::HttpConn()
    : fd_(-1),
      bytes_sent_(0),
      keep_alive_(false),
      last_body_bytes_(0),
      root_("html/") {}

void HttpConn::set_error_response(const std::string& status,
                                  const std::string& html_file,
                                  const std::string& fallback_html,
                                  bool force_close) {
    if (force_close) {
        keep_alive_ = false;
    }

    std::string body;
    if (!html_file.empty()) {
        body = read_file(root_ + html_file);
    }
    if (body.empty()) {
        body = fallback_html;
    }
    
    last_status_ = status;
    last_body_bytes_ = body.size();
    write_buf_ = make_response(status, "text/html; charset=UTF-8", body);
    bytes_sent_ = 0;
}

void HttpConn::set_400_response() {
    set_error_response("400 Bad Request", "400.html", "<h1>400 Bad Request</h1>", false);
}

void HttpConn::set_405_response() {
    set_error_response("405 Method Not Allowed",
                       "405.html",
                       "<h1>405 Method Not Allowed</h1>",
                       false);
}

void HttpConn::set_404_response() {
    set_error_response("404 Not Found", "404.html", "<h1>404 Not Found</h1>", false);
}

void HttpConn::set_413_response() {
    set_error_response("413 Payload Too Large",
                       "413.html",
                       "<h1>413 Payload Too Large</h1>",
                       true);
}

void HttpConn::set_431_response() {
    set_error_response("431 Request Header Fields Too Large",
                       "431.html",
                       "<h1>431 Request Header Fields Too Large</h1>",
                       true);
}


// 连接初始化：挂载 fd 并清空读写状态
void HttpConn::init(int fd) {
    fd_ = fd;
    read_buf_.clear();
    write_buf_.clear();
    bytes_sent_ = 0;
    keep_alive_ = false;
    last_method_.clear();
    last_url_.clear();
    last_status_.clear();
    last_body_bytes_ = 0;
}

// 连接关闭：关闭 fd 并复位状态
void HttpConn::close_conn() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    read_buf_.clear();
    write_buf_.clear();
    bytes_sent_ = 0;
    keep_alive_ = false;
    last_method_.clear();
    last_url_.clear();
    last_status_.clear();
    last_body_bytes_ = 0;
}

// 读取客户端请求：GET 读完整请求头，POST 读完整请求头 + body
IOState HttpConn::read_once() {
    char buf[4096];
    while (true) {
        ssize_t rn = recv(fd_, buf, sizeof(buf), 0);
        if (rn > 0) {
            read_buf_.append(buf, static_cast<size_t>(rn));
            
            size_t header_end = read_buf_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                if(read_buf_.size() > HEADLIMIT) return IOState::HEAD_TOO_LARGE;
                continue;
            }else if(header_end > HEADLIMIT) {
                return IOState::HEAD_TOO_LARGE;
            }
            Request req = parse_request(read_buf_);
            if(req.content_length > BODYLIMIT) return IOState::BODY_TOO_LARGE; 

            if (req.method == "POST") {
                size_t body_start = header_end + 4;
                if (read_buf_.size() < body_start + req.content_length) {
                    continue;
                }
            }
            return IOState::READY;
        } else if (rn == 0) {
            return IOState::CLOSED;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return IOState::ERROR;
        }
    }
    return IOState::AGAIN;
}

bool HttpConn::handle_post(Request& req) {
    size_t header_end = read_buf_.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        set_400_response();
        return true;
    }

    size_t body_start = header_end + 4;
    if (read_buf_.size() < body_start + req.content_length) {
        set_400_response();
        return true;
    }

    req.body = read_buf_.substr(body_start, req.content_length);

    if (req.url != "/echo") {
        set_404_response();
        return true;
    }

    std::string html = read_file(root_ + "echo.html");
    if (html.empty()) {
        html = "<h1>POST Echo</h1><p>echo.html not found.</p>";
    }
    const std::string marker = "{{BODY}}";
    size_t pos = html.find(marker);
    if (pos != std::string::npos) {
        html.replace(pos, marker.size(), html_escape(req.body));
    }
    last_status_ = "200 OK";
    last_body_bytes_ = html.size();
    write_buf_ = make_response("200 OK", "text/html; charset=UTF-8", html);
    bytes_sent_ = 0;
    return true;
}

bool HttpConn::handle_get(Request& req) {
    std::cout << "[REQ] fd=" << fd_ << " url=" << req.url << " connection=" << req.connection
            << std::endl;

    auto [status, body] = route(req.url);
    if (body.empty()) {
        body = "<h1>500 Internal Server Error</h1>";
        status = "500 Internal Server Error";
    }

    std::string type = get_content_type(req.url);
    if (status == "404 Not Found" || status == "500 Internal Server Error") {
        type = "text/html; charset=UTF-8";
    }

    last_status_ = status;
    last_body_bytes_ = body.size();
    write_buf_ = make_response(status, type, body);
    bytes_sent_ = 0;
    return true;
}

// 业务处理入口：校验请求 -> 路由 -> 组装响应
bool HttpConn::process() {
    Request req;
    VerifyResult res = validate_request(req);
    last_method_ = req.method;
    last_url_ = req.url;

    // HTTP/1.1 默认 keep-alive，除非显式 close；HTTP/1.0 反之
    if (req.version == "HTTP/1.1") {
        keep_alive_ = (req.connection != "close");
    } else {
        keep_alive_ = (req.connection == "keep-alive");
    }

    if (res == VerifyResult::BadRequest) {
        set_400_response();
        return true;
    }

    if (res == VerifyResult::NotAllowed) {
        set_405_response();
        return true;
    }

    if (req.method == "GET") {
        return handle_get(req);
    }
    return handle_post(req);
}

// 非阻塞写回，支持一次请求分多次 send 完成
IOState HttpConn::write() {
    // 非阻塞发送，直到发完或 socket 暂时不可写
    while (bytes_sent_ < write_buf_.size()) {
        ssize_t wn = send(fd_, write_buf_.c_str() + bytes_sent_, write_buf_.size() - bytes_sent_, 0);
        if (wn > 0) {
            bytes_sent_ += static_cast<size_t>(wn);
        } else if (wn < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return IOState::AGAIN;
        } else {
            return IOState::ERROR;
        }
    }
    return IOState::READY;
}


bool HttpConn::has_complete_request() const {
    size_t header_end = read_buf_.find("\r\n\r\n");
    if(header_end == std::string::npos) return false;

    Request req = parse_request(read_buf_);
    size_t body_start = header_end + 4;
    size_t need = body_start + req.content_length; // GET时content_length=0
    return read_buf_.size() >= need;
}


int HttpConn::fd() const { return fd_; }
bool HttpConn::keep_alive() const { return keep_alive_; }
const std::string& HttpConn::last_method() const { return last_method_; }
const std::string& HttpConn::last_url() const { return last_url_; }
const std::string& HttpConn::last_status() const { return last_status_; }
size_t HttpConn::last_body_bytes() const { return last_body_bytes_; }


void HttpConn::reset_for_next_request() {
    size_t header_end = read_buf_.find("\r\n\r\n");
    if(header_end == std::string::npos) read_buf_.clear();
    else {
        Request req = parse_request(read_buf_);
        size_t body_start = header_end + 4;
        size_t len = std::min(body_start + req.content_length, read_buf_.size());

        std::cout << "[RESET] consumed=" << len
          << " remain=" << read_buf_.size() - len << std::endl;

        read_buf_.erase(0, len);

    }

    write_buf_.clear();
    bytes_sent_ = 0;
    keep_alive_ = false;
    last_method_.clear();
    last_url_.clear();
    last_status_.clear();
    last_body_bytes_ = 0;
}

VerifyResult HttpConn::validate_request(Request& req) const {
    req = parse_request(read_buf_);
    
    if (req.method.empty() || req.url.empty() || req.version.empty()) {
        return VerifyResult::BadRequest;
    }
    if (req.method != "GET" && req.method != "POST") {
        return VerifyResult::NotAllowed;
    }
    if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") {
        return VerifyResult::BadRequest;
    }
    // HTTP/1.1 要求 Host 必须存在；HTTP/1.0 可不带
    if (req.version == "HTTP/1.1" && req.host.empty()) {
        return VerifyResult::BadRequest;
    }

    if (!req.connection.empty() && req.connection != "close" && req.connection != "keep-alive") {
        return VerifyResult::BadRequest;
    }

    if (req.url[0] != '/') {
        return VerifyResult::BadRequest;
    }
    return VerifyResult::OK;
}


// 解析请求：请求行 + 常用请求头（Host/Connection）
Request HttpConn::parse_request(const std::string& msg) const {
    Request req{};

    size_t header_end = msg.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return req;
    }

    size_t line_end = msg.find("\r\n");
    if (line_end == std::string::npos) {
        return req;
    }

    std::string request_line = msg.substr(0, line_end);
    std::stringstream rl(request_line);
    std::string extra;
    
    if (!(rl >> req.method >> req.url >> req.version) || (rl >> extra)) {
        return Request{};
    }

    if (req.method.empty() || req.url.empty() || req.version.empty()) {
        return Request{};
    }

    size_t cur = line_end + 2;
    bool has_content_length = false;
    size_t content_length_value = 0;
    while (cur < header_end) {
        size_t next = msg.find("\r\n", cur);
        if (next == std::string::npos || next > header_end) {
            return Request{};
        }

        std::string line = msg.substr(cur, next - cur);
        if (line.empty()) {
            break;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            return Request{}; // 非法 header 行
        }

        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        if (key.empty()) {
            return Request{};
        }

        // 去掉 value 左侧空格
        while (!value.empty() && (value[0] == ' ' || value[0] == '\t')) {
            value.erase(value.begin());
        }
        // 去掉 value 右侧空白
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        std::string key_lower = to_lower_copy(key);
        if (key_lower == "host") {
            req.host = value;
        } else if (key_lower == "connection") {
            req.connection = to_lower_copy(value);
        } else if(key_lower == "content-length") {
            if (value.empty()) {
                return Request{};
            }
            if (!std::all_of(value.begin(), value.end(),
                             [](unsigned char c) { return std::isdigit(c) != 0; })) {
                return Request{};
            }
            try {
                unsigned long long parsed = std::stoull(value);
                size_t parsed_len = static_cast<size_t>(parsed);
                if (has_content_length && parsed_len != content_length_value) {
                    return Request{};
                }
                has_content_length = true;
                content_length_value = parsed_len;
                req.content_length = parsed_len;
            } catch(...) {
                return Request{};
            }
        }

        cur = next + 2;
    }

    size_t body_start = header_end + 4;
    if(msg.size() >= body_start) {
        size_t avail = msg.size() - body_start;
        size_t take = std::min(avail, req.content_length);
        req.body = msg.substr(body_start, take);
    }
    return req;
}


std::string HttpConn::make_response(const std::string& status,
                                    const std::string& type,
                                    const std::string& body) const {
    const std::string conn = keep_alive_ ? "keep-alive" : "close";
    std::string res =
        "HTTP/1.1 " + status + "\r\n"
        "Content-Type: " + type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: " + conn + "\r\n"
        "\r\n" +
        body;
    return res;
}
// 简单的扩展名到 MIME 类型映射
std::string HttpConn::get_content_type(const std::string& url) const {
    if (url == "/time" || url == "/" || url.empty()) {
        return "text/html; charset=UTF-8";
    }
    std::string target = url;
    if (!target.empty() && target[0] == '/') {
        target = target.substr(1);
    }
    if (!target.empty() && target.find('.') == std::string::npos) {
        target += ".html";
    }
    if (target.size() >= 5 && target.substr(target.size() - 5) == ".html") {
        return "text/html; charset=UTF-8";
    }
    if (target.size() >= 4 && target.substr(target.size() - 4) == ".css") {
        return "text/css; charset=UTF-8";
    }
    if (target.size() >= 3 && target.substr(target.size() - 3) == ".js") {
        return "application/javascript; charset=UTF-8";
    }
    return "text/plain; charset=UTF-8";
}

// 以文本方式读取文件，失败返回空串
std::string HttpConn::read_file(const std::string& filename) const {
    std::ifstream fin(filename);
    if (!fin.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << fin.rdbuf();
    return buffer.str();
}

std::string HttpConn::get_file_path(const std::string& url) const {
    // / 默认映射到首页，其它路径按静态文件相对路径拼接

    if (url.empty() || url.find("..") != std::string::npos) {
        return "";
    }
    
    std::string rel = url.substr(1);
    if (rel.empty()) {
        return root_ + "index.html";
    }
    // 无扩展名的路径默认按 html 文件处理，例如 /hello -> hello.html
    if (rel.find('.') == std::string::npos) {
        rel += ".html";
    }
    return root_ + rel;
}

std::pair<std::string, std::string> HttpConn::route(const std::string& url) const {
    // time 页面改为静态 html 文件
    if (url == "/time") {
        return {"200 OK", read_file(root_ + "time.html")};
    }

    std::string path = get_file_path(url);
    std::string body = read_file(path);
    if (!body.empty()) {
        return {"200 OK", body};
    }

    // 静态文件路由：未命中则回退到 404 页面
    return {"404 Not Found", read_file(root_ + "404.html")};
}
