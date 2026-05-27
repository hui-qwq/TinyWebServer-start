#include "http_conn.hpp"
#include "auth.hpp"
#include "template.hpp"
#include "../db/sql_connection_pool.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mysql/mysql.h>
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


}  // namespace

// 从请求头中提取 Content-Length 值，只扫描不构建 Request 结构体
static size_t extract_content_length(const std::string& buf, size_t header_end) {
    size_t pos = 0;
    while (pos < header_end) {
        size_t line_end = buf.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) break;

        std::string line = buf.substr(pos, line_end - pos);
        if (line.empty()) { pos = line_end + 2; continue; }

        size_t colon = line.find(':');
        if (colon == std::string::npos) { pos = line_end + 2; continue; }

        std::string key = line.substr(0, colon);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (key == "content-length") {
            std::string val = line.substr(colon + 1);
            while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                val.erase(val.begin());
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
                val.pop_back();

            if (!val.empty() && std::all_of(val.begin(), val.end(), ::isdigit)) {
                try { return std::stoull(val); }
                catch (...) { return 0; }
            }
        }
        pos = line_end + 2;
    }
    return 0;
}

// 静态文件缓存的存储定义
std::unordered_map<std::string, std::string> HttpConn::file_cache_;
std::mutex HttpConn::file_cache_mutex_;

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

void HttpConn::set_html_response(const std::string& status,
                                 const std::string& html) {
    last_status_ = status;
    last_body_bytes_ = html.size();
    write_buf_ = make_response(status, "text/html; charset=UTF-8", html);
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
    gen_++;
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

bool HttpConn::handle_register(Request& req) {
    std::string username = get_form_value(req.body, "username");
    std::string password = get_form_value(req.body, "password");
    AuthResult res = register_user(username, password);
    if (res == AuthResult::InvalidInput) {
        set_html_response("400 Bad Request",
                          "<h1>Register</h1><p>username or password empty</p>");
        return true;
    }

    if (res == AuthResult::DatabaseError) {
        set_html_response("500 Internal Server Error",
                          "<h1>Register</h1><p>database connection unavailable</p>");
        return true;
    }

    if (res == AuthResult::UserExists) {
        set_html_response("200 OK",
                            "<h1>Register</h1><p>username already exists</p>");
            return true;
    }

    set_html_response("200 OK",
                      "<h1>Register</h1><p>register success</p>"
                      "<p><a href=\"/login\">Go Login</a></p>");
    return true;
}

bool HttpConn::handle_login(Request& req) {
    std::string username = get_form_value(req.body, "username");
    std::string password = get_form_value(req.body, "password");

    int user_id = 0;
    AuthResult res = login_user(username, password, &user_id);
    if (res == AuthResult::InvalidInput) {
        set_html_response("400 Bad Request",
                          "<h1>Login</h1><p>username or password empty</p>");
        return true;
    }

    if (res == AuthResult::DatabaseError) {
        set_html_response("500 Internal Server Error",
                          "<h1>Login</h1><p>database connection unavailable</p>");
        return true;
    }

    if (res == AuthResult::Success) {
        std::string token = generate_token();
        {
            Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
            MYSQL* conn = guard.get();
            if (conn) {
                std::string sql =
                    "INSERT INTO sessions (user_id, token) VALUES ("
                    + std::to_string(user_id) + ", '" + token + "')";
                mysql_query(conn, sql.c_str());
            }
        }

        std::string cookie = "session_token=" + token + "; HttpOnly; Path=/";
        std::string body = "<h1>Login</h1><p>login success</p>"
                           "<p><a href=\"/blog\">Go to Blog</a></p>";
        last_status_ = "200 OK";
        last_body_bytes_ = body.size();
        write_buf_ = make_response("200 OK", "text/html; charset=UTF-8", body, cookie);
        bytes_sent_ = 0;
    } else {
        set_html_response("200 OK",
                          "<h1>Login</h1><p>username or password wrong</p>");
    }
    return true;
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

    if (req.url.find("/blog") == 0) {
        return handle_blog_post(req);
    }

    if (req.url == "/register") {
        return handle_register(req);
    }

    if (req.url == "/login") {
        return handle_login(req);
    }

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

int HttpConn::get_session_user_id() const {
    size_t header_end = read_buf_.find("\r\n\r\n");
    if (header_end == std::string::npos) return 0;

    size_t cookie_pos = read_buf_.find("Cookie: ");
    if (cookie_pos == std::string::npos || cookie_pos >= header_end) return 0;

    cookie_pos += 8;
    size_t cookie_end = read_buf_.find("\r\n", cookie_pos);
    if (cookie_end == std::string::npos || cookie_end > header_end) return 0;

    std::string cookie_line = read_buf_.substr(cookie_pos, cookie_end - cookie_pos);
    const std::string key = "session_token=";
    size_t key_pos = cookie_line.find(key);
    if (key_pos == std::string::npos) return 0;

    size_t val_start = key_pos + key.size();
    size_t val_end = cookie_line.find(';', val_start);
    if (val_end == std::string::npos) val_end = cookie_line.size();
    std::string token = cookie_line.substr(val_start, val_end - val_start);
    if (token.empty()) return 0;

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if (!conn) return 0;

    const char* sql = "SELECT user_id FROM sessions WHERE token = ?";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) return 0;
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    MYSQL_BIND bind[1];
    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)token.c_str();
    bind[0].buffer_length = token.size();
    mysql_stmt_bind_param(stmt, bind);

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        return 0;
    }

    int user_id = 0;
    MYSQL_BIND result[1];
    std::memset(result, 0, sizeof(result));
    result[0].buffer_type = MYSQL_TYPE_LONG;
    result[0].buffer = &user_id;
    mysql_stmt_bind_result(stmt, result);

    int fetch_ret = mysql_stmt_fetch(stmt);
    mysql_stmt_close(stmt);
    return (fetch_ret == 0) ? user_id : 0;
}

// ---------- Blog handlers ----------

bool HttpConn::handle_blog_list() {
    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();

    std::string items;
    if (conn) {
        const char* sql =
            "SELECT id, title, category, DATE_FORMAT(created_at, '%Y-%m-%d') "
            "FROM articles WHERE is_published=1 ORDER BY id DESC LIMIT 50";
        if (mysql_query(conn, sql) == 0) {
            MYSQL_RES* res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res))) {
                    std::string id    = row[0] ? row[0] : "";
                    std::string title = row[1] ? row[1] : "";
                    std::string cat   = row[2] ? row[2] : "";
                    std::string date  = row[3] ? row[3] : "";

                    items += "<article class=\"post-item\">"
                             "<h2><a href=\"/blog/" + id + "\">" + html_escape(title) + "</a></h2>"
                             "<div class=\"post-meta\">"
                             "<span>" + date + "</span>";
                    if (!cat.empty()) {
                        items += " <span class=\"post-category\">" + html_escape(cat) + "</span>";
                    }
                    items += "</div></article>\n";
                }
                mysql_free_result(res);
            }
        }
    }

    if (items.empty()) {
        items = "<p class=\"empty\">还没有文章，<a href=\"/blog/new\">写一篇</a>吧。</p>";
    }

    std::string html = read_file(root_ + "blog/list.html");
    if (html.empty()) html = "<h1>Blog</h1>" + items;
    replace_placeholder(html, "ARTICLES", items);

    set_html_response("200 OK", html);
    return true;
}

bool HttpConn::handle_blog_detail(int id) {
    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();

    std::string title, content, date_str, category;
    if (conn) {
        std::string sql_str =
            "SELECT title, content, category, DATE_FORMAT(created_at, '%Y-%m-%d %H:%i') "
            "FROM articles WHERE id = " + std::to_string(id);
        if (mysql_query(conn, sql_str.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row) {
                    title    = row[0] ? row[0] : "";
                    content  = row[1] ? row[1] : "";
                    category = row[2] ? row[2] : "";
                    date_str = row[3] ? row[3] : "";
                }
                mysql_free_result(res);
            }
        }
    }

    if (title.empty() && content.empty()) {
        set_404_response();
        return true;
    }

    std::string html = read_file(root_ + "blog/detail.html");
    if (html.empty()) {
        set_html_response("200 OK", "<h1>" + html_escape(title) + "</h1><pre>" + html_escape(content) + "</pre>");
        return true;
    }

    replace_placeholder(html, "TITLE", html_escape(title));
    replace_placeholder(html, "CONTENT_JSON", escape_json(content));
    replace_placeholder(html, "DATE", date_str);
    replace_placeholder(html, "CATEGORY", html_escape(category));
    replace_placeholder(html, "ID", std::to_string(id));

    set_html_response("200 OK", html);
    return true;
}

bool HttpConn::handle_blog_new_page() {
    int user_id = get_session_user_id();
    if (user_id == 0) {
        set_html_response("200 OK",
            "<h1>Blog</h1><p>请先<a href=\"/login\">登录</a>后再写文章。</p>"
            "<p><a href=\"/blog\">返回列表</a></p>");
        return true;
    }

    std::string html = read_file(root_ + "blog/editor.html");
    if (html.empty()) {
        html = "<h1>New Article</h1>"
               "<form method=\"POST\" action=\"/blog\">"
               "<input name=\"title\" placeholder=\"Title\" required>"
               "<textarea name=\"content\" placeholder=\"Markdown...\" required></textarea>"
               "<button type=\"submit\">Publish</button></form>";
    } else {
        replace_placeholder(html, "HEADING", "New Article");
        replace_placeholder(html, "FORM_ACTION", "/blog");
        replace_placeholder(html, "TITLE_VALUE", "");
        replace_placeholder(html, "CONTENT_VALUE", "");
        replace_placeholder(html, "CATEGORY_VALUE", "");
        replace_placeholder(html, "DELETE_HTML", "");
        replace_placeholder(html, "ID", "");
    }

    set_html_response("200 OK", html);
    return true;
}

bool HttpConn::handle_blog_create(Request& req) {
    int user_id = get_session_user_id();
    if (user_id == 0) {
        std::string conn_str = keep_alive_ ? "keep-alive" : "close";
        write_buf_ =
            "HTTP/1.1 302 Found\r\n"
            "Location: /login\r\n"
            "Content-Length: 0\r\n"
            "Connection: " + conn_str + "\r\n\r\n";
        bytes_sent_ = 0;
        last_status_ = "302 Found";
        last_body_bytes_ = 0;
        return true;
    }

    std::string title    = url_decode(get_form_value(req.body, "title"));
    std::string content  = url_decode(get_form_value(req.body, "content"));
    std::string category = url_decode(get_form_value(req.body, "category"));

    if (title.empty() || content.empty()) {
        set_html_response("400 Bad Request", "<h1>Error</h1><p>Title and content are required.</p>");
        return true;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if (!conn) {
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database unavailable.</p>");
        return true;
    }

    const char* sql = "INSERT INTO articles (title, content, category) VALUES (?, ?, ?)";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) {
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database error.</p>");
        return true;
    }
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database error.</p>");
        return true;
    }

    MYSQL_BIND bind[3];
    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)title.c_str();
    bind[0].buffer_length = title.size();
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void*)content.c_str();
    bind[1].buffer_length = content.size();
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void*)category.c_str();
    bind[2].buffer_length = category.size();
    mysql_stmt_bind_param(stmt, bind);

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Failed to create article.</p>");
        return true;
    }
    mysql_stmt_close(stmt);

    std::string conn_str = keep_alive_ ? "keep-alive" : "close";
    write_buf_ =
        "HTTP/1.1 302 Found\r\n"
        "Location: /blog\r\n"
        "Content-Length: 0\r\n"
        "Connection: " + conn_str + "\r\n\r\n";
    bytes_sent_ = 0;
    last_status_ = "302 Found";
    last_body_bytes_ = 0;
    return true;
}

bool HttpConn::handle_blog_edit_page(int id) {
    int user_id = get_session_user_id();
    if (user_id == 0) {
        set_html_response("200 OK",
            "<h1>Blog</h1><p>请先<a href=\"/login\">登录</a>后再编辑。</p>");
        return true;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();

    std::string title, content, category;
    if (conn) {
        std::string sql_str =
            "SELECT title, content, category FROM articles WHERE id = " + std::to_string(id);
        if (mysql_query(conn, sql_str.c_str()) == 0) {
            MYSQL_RES* res = mysql_store_result(conn);
            if (res) {
                MYSQL_ROW row = mysql_fetch_row(res);
                if (row) {
                    title    = row[0] ? row[0] : "";
                    content  = row[1] ? row[1] : "";
                    category = row[2] ? row[2] : "";
                }
                mysql_free_result(res);
            }
        }
    }

    if (title.empty() && content.empty()) {
        set_404_response();
        return true;
    }

    std::string html = read_file(root_ + "blog/editor.html");
    if (html.empty()) {
        set_html_response("200 OK", "<h1>Edit</h1><p>Editor template not found.</p>");
        return true;
    }

    replace_placeholder(html, "HEADING", "Edit Article");
    replace_placeholder(html, "FORM_ACTION", "/blog/" + std::to_string(id) + "/edit");
    replace_placeholder(html, "TITLE_VALUE", html_escape(title));
    replace_placeholder(html, "CONTENT_VALUE", html_escape(content));
    replace_placeholder(html, "CATEGORY_VALUE", html_escape(category));
    replace_placeholder(html, "ID", std::to_string(id));

    std::string delete_html =
        "<form method=\"POST\" action=\"/blog/" + std::to_string(id) + "/delete\" "
        "onsubmit=\"return confirm('Delete this article?')\" style=\"display:inline;\">"
        "<button type=\"submit\" class=\"btn-delete\">Delete</button></form>";
    replace_placeholder(html, "DELETE_HTML", delete_html);

    set_html_response("200 OK", html);
    return true;
}

bool HttpConn::handle_blog_update(int id, Request& req) {
    int user_id = get_session_user_id();
    if (user_id == 0) {
        std::string conn_str = keep_alive_ ? "keep-alive" : "close";
        write_buf_ =
            "HTTP/1.1 302 Found\r\n"
            "Location: /login\r\n"
            "Content-Length: 0\r\n"
            "Connection: " + conn_str + "\r\n\r\n";
        bytes_sent_ = 0;
        last_status_ = "302 Found";
        last_body_bytes_ = 0;
        return true;
    }

    std::string title    = url_decode(get_form_value(req.body, "title"));
    std::string content  = url_decode(get_form_value(req.body, "content"));
    std::string category = url_decode(get_form_value(req.body, "category"));

    if (title.empty() || content.empty()) {
        set_html_response("400 Bad Request", "<h1>Error</h1><p>Title and content are required.</p>");
        return true;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if (!conn) {
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database unavailable.</p>");
        return true;
    }

    const char* sql = "UPDATE articles SET title=?, content=?, category=? WHERE id=?";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) {
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database error.</p>");
        return true;
    }
    if (mysql_stmt_prepare(stmt, sql, std::strlen(sql)) != 0) {
        mysql_stmt_close(stmt);
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Database error.</p>");
        return true;
    }

    MYSQL_BIND bind[4];
    std::memset(bind, 0, sizeof(bind));
    bind[0].buffer_type = MYSQL_TYPE_STRING;
    bind[0].buffer = (void*)title.c_str();
    bind[0].buffer_length = title.size();
    bind[1].buffer_type = MYSQL_TYPE_STRING;
    bind[1].buffer = (void*)content.c_str();
    bind[1].buffer_length = content.size();
    bind[2].buffer_type = MYSQL_TYPE_STRING;
    bind[2].buffer = (void*)category.c_str();
    bind[2].buffer_length = category.size();
    bind[3].buffer_type = MYSQL_TYPE_LONG;
    bind[3].buffer = &id;
    mysql_stmt_bind_param(stmt, bind);

    if (mysql_stmt_execute(stmt) != 0) {
        mysql_stmt_close(stmt);
        set_html_response("500 Internal Server Error", "<h1>Error</h1><p>Failed to update article.</p>");
        return true;
    }
    mysql_stmt_close(stmt);

    std::string conn_str = keep_alive_ ? "keep-alive" : "close";
    write_buf_ =
        "HTTP/1.1 302 Found\r\n"
        "Location: /blog/" + std::to_string(id) + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: " + conn_str + "\r\n\r\n";
    bytes_sent_ = 0;
    last_status_ = "302 Found";
    last_body_bytes_ = 0;
    return true;
}

bool HttpConn::handle_blog_delete(int id) {
    int user_id = get_session_user_id();
    if (user_id == 0) {
        std::string conn_str = keep_alive_ ? "keep-alive" : "close";
        write_buf_ =
            "HTTP/1.1 302 Found\r\n"
            "Location: /login\r\n"
            "Content-Length: 0\r\n"
            "Connection: " + conn_str + "\r\n\r\n";
        bytes_sent_ = 0;
        last_status_ = "302 Found";
        last_body_bytes_ = 0;
        return true;
    }

    Sql_Connection_Guard guard(Sql_Connection_Pool::instance());
    MYSQL* conn = guard.get();
    if (conn) {
        std::string sql_str = "DELETE FROM articles WHERE id = " + std::to_string(id);
        mysql_query(conn, sql_str.c_str());
    }

    std::string conn_str = keep_alive_ ? "keep-alive" : "close";
    write_buf_ =
        "HTTP/1.1 302 Found\r\n"
        "Location: /blog\r\n"
        "Content-Length: 0\r\n"
        "Connection: " + conn_str + "\r\n\r\n";
    bytes_sent_ = 0;
    last_status_ = "302 Found";
    last_body_bytes_ = 0;
    return true;
}

bool HttpConn::handle_blog_get(const Request& req) {
    if (req.url == "/blog") {
        return handle_blog_list();
    }
    if (req.url == "/blog/new") {
        return handle_blog_new_page();
    }

    std::string path = req.url.substr(5);  // remove "/blog"
    if (path.empty() || path[0] != '/') {
        set_404_response();
        return true;
    }
    path = path.substr(1);  // remove leading '/'

    if (path.size() > 5 && path.substr(path.size() - 5) == "/edit") {
        std::string id_str = path.substr(0, path.size() - 5);
        try { return handle_blog_edit_page(std::stoi(id_str)); }
        catch (...) { set_404_response(); return true; }
    }

    // /blog/42  →  detail
    try { return handle_blog_detail(std::stoi(path)); }
    catch (...) { set_404_response(); return true; }
}

bool HttpConn::handle_blog_post(Request& req) {
    if (req.url == "/blog") {
        return handle_blog_create(req);
    }

    std::string path = req.url.substr(5);  // remove "/blog"
    if (path.empty() || path[0] != '/') {
        set_404_response();
        return true;
    }
    path = path.substr(1);

    if (path.size() > 5 && path.substr(path.size() - 5) == "/edit") {
        std::string id_str = path.substr(0, path.size() - 5);
        try { return handle_blog_update(std::stoi(id_str), req); }
        catch (...) { set_404_response(); return true; }
    }

    if (path.size() > 7 && path.substr(path.size() - 7) == "/delete") {
        std::string id_str = path.substr(0, path.size() - 7);
        try { return handle_blog_delete(std::stoi(id_str)); }
        catch (...) { set_404_response(); return true; }
    }

    set_404_response();
    return true;
}

bool HttpConn::handle_get(Request& req) {
    std::cout << "[REQ] fd=" << fd_ << " url=" << req.url << " connection=" << req.connection
            << std::endl;

    bool is_blog = false;
    if (req.url == "/blog" || req.url == "/blog/new") {
        is_blog = true;
    } else if (req.url.find("/blog/") == 0) {
        std::string rest = req.url.substr(6);
        is_blog = (!rest.empty() && rest.find('.') == std::string::npos);
    }
    if (is_blog) {
        return handle_blog_get(req);
    }

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

    size_t content_length = extract_content_length(read_buf_, header_end);
    size_t body_start = header_end + 4;
    size_t need = body_start + content_length;
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
        size_t content_length = extract_content_length(read_buf_, header_end);
        size_t body_start = header_end + 4;
        size_t len = std::min(body_start + content_length, read_buf_.size());

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
                                    const std::string& body,
                                    const std::string& set_cookie) const {
    const std::string conn = keep_alive_ ? "keep-alive" : "close";
    std::string res =
        "HTTP/1.1 " + status + "\r\n"
        "Content-Type: " + type + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: " + conn + "\r\n";
    if (!set_cookie.empty()) {
        res += "Set-Cookie: " + set_cookie + "\r\n";
    }
    res += "\r\n" + body;
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

// 以文本方式读取文件，失败返回空串。命中缓存时避免磁盘 I/O。
std::string HttpConn::read_file(const std::string& filename) const {
    // 先查缓存
    {
        std::lock_guard<std::mutex> lock(file_cache_mutex_);
        auto it = file_cache_.find(filename);
        if (it != file_cache_.end()) {
            return it->second;
        }
    }

    // 缓存未命中，从磁盘读取
    std::ifstream fin(filename);
    if (!fin.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << fin.rdbuf();
    std::string content = buffer.str();

    // 写入缓存供后续请求复用
    {
        std::lock_guard<std::mutex> lock(file_cache_mutex_);
        file_cache_[filename] = content;
    }

    return content;
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
