#include "webserver.hpp"
#include "../http/http_conn.hpp"
#include "../logger/logger.hpp"
#include "../thread_pool/thread_pool.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
namespace {
// 把 fd 设置为非阻塞，配合 epoll 边缘/水平触发都更稳妥
void set_nonblock(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag >= 0) {
        fcntl(fd, F_SETFL, flag | O_NONBLOCK);
    }
}
}  // namespace

WebServer::WebServer(size_t thread_count, int idle_timeout_sec)
    : port_(0),
      listenfd_(-1),
      epfd_(-1),
      pool_(thread_count, 10000),
      idle_timeout_sec_(idle_timeout_sec) 
{}


WebServer::~WebServer() {
    if (listenfd_ >= 0) {
        close(listenfd_);
    }
    if (epfd_ >= 0) {
        close(epfd_);
    }
}

bool WebServer::init(int port) {
    port_ = port; 
    return event_listen();
}

bool WebServer::event_listen() {
    // 1) 创建监听 socket
    listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd_ < 0) {
        Logger::instance().error("socket failed: " + std::string(std::strerror(errno)));
        return false;
    }

    int on = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(static_cast<uint16_t>(port_));

    // 2) 绑定端口并开始监听
    if(bind(listenfd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) != 0) {
        Logger::instance().error("bind failed: " + std::string(std::strerror(errno)));
        return false;
    }
    if(listen(listenfd_, 5) != 0){
        Logger::instance().error("listen failed: " + std::string(std::strerror(errno)));
        return false;
    }

    set_nonblock(listenfd_);

    // 3) 创建 epoll 并把监听 fd 加进去
    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        close(listenfd_);
        Logger::instance().error("epoll_create1 failed: " + std::string(std::strerror(errno)));
        return false;
    }

    epoll_event ev{};
    ev.data.fd = listenfd_;
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listenfd_, &ev) < 0) {
        close(listenfd_);
        close(epfd_);
        Logger::instance().error("epoll_ctl add listenfd failed: " + std::string(std::strerror(errno)));
        return false;
    }

    Logger::instance().info("listening on " + std::to_string(port_) + "...");
    return true;
}

// epoll 主循环：分发连接、读写、异常断开事件
void WebServer::run() {
    if (listenfd_ < 0 || epfd_ < 0) {
        return;
    }

    while (true) {
        int n = epoll_wait(epfd_, events_, 1024, 1000);
        if (n < 0) {
            if(errno == EINTR) continue;
            Logger::instance().error("epoll_wait failed: " + std::string(std::strerror(errno)));
            break;
        }

        handle_timeout();

        for (int i = 0; i < n; i++) {
            int fd = events_[i].data.fd;
            uint32_t e = events_[i].events;

            if (fd == listenfd_) {
                handle_accept();
            } else if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                Logger::instance().info("client[" + std::to_string(fd) + "] disconnected");
                close_conn(fd);
            } else if (e & EPOLLIN) {
                handle_read(fd);
            } else if (e & EPOLLOUT) {
                handle_write(fd);
            }
        }

    }
}


void WebServer::handle_timeout() {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    std::vector<int> to_close;
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        to_close.reserve(last_active_.size());

        for(const auto& kv : last_active_) {
            int fd = kv.first;
            const auto& tp = kv.second;
            auto idle = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();
            if(idle > idle_timeout_sec_) to_close.push_back(fd);
        }
    }
    for (int fd : to_close) {
        Logger::instance().warn("client[" + std::to_string(fd) + "] timeout, close");
        close_conn(fd);
    }
}

// 批量 accept 已到达的连接，直到返回 EAGAIN
void WebServer::handle_accept() {
    while (true) {
        sockaddr_in cliaddr{};
        socklen_t len = sizeof(cliaddr);
        int clfd = accept(listenfd_, reinterpret_cast<sockaddr*>(&cliaddr), &len);
        if (clfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            Logger::instance().warn("accept failed: " + std::string(std::strerror(errno)));
            break;
        }

        set_nonblock(clfd);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = clfd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, clfd, &ev) < 0) {
            Logger::instance().warn("epoll_ctl add client fd failed: " + std::string(std::strerror(errno)));
            close(clfd);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            users_[clfd].init(clfd);
            last_active_[clfd] = std::chrono::steady_clock::now();
        }
        Logger::instance().info("client[" + std::to_string(clfd) + "] connected");
    }
}

// 读请求并构造响应，然后切到可写事件发送数据
void WebServer::handle_read(int fd) {
    bool ok = pool_.enqueue([this, fd]() {
        bool need_close = false;
        bool need_mod_write = false;

        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            auto it = users_.find(fd);
            if (it == users_.end()) {
                return;
            }
            last_active_[fd] = std::chrono::steady_clock::now();

            IOState st = it->second.read_once();
            if (st == IOState::CLOSED || st == IOState::ERROR) {
                need_close = true;
            } else if (st == IOState::AGAIN) {
                return;
            } else if (st == IOState::HEAD_TOO_LARGE) {
                it->second.set_431_response();
                need_mod_write = true;
            } else if (st == IOState::BODY_TOO_LARGE) {
                it->second.set_413_response();
                need_mod_write = true;
            } else {
                if (!it->second.process()) {
                    need_close = true;
                } else {
                    need_mod_write = true;
                }
            }
        }

        if (need_close) {
            close_conn(fd);
            return;
        }
        if (need_mod_write) {
            modfd(fd, EPOLLOUT | EPOLLRDHUP);
        }
    });

    if (!ok) {
        Logger::instance().warn("thread pool full, close client[" + std::to_string(fd) + "]");
        close_conn(fd);
    }
}


void WebServer::handle_write(int fd) {
    bool need_close = false;
    bool need_mod_read = false;
    bool need_mod_write = false;

    std::string method;
    std::string url;
    std::string status;
    size_t body_bytes = 0;
    bool keep_alive = false;

    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        auto it = users_.find(fd);
        if (it == users_.end()) {
            return;
        }
        last_active_[fd] = std::chrono::steady_clock::now();

        auto st = it->second.write();
        if (st == IOState::ERROR || st == IOState::CLOSED) {
            need_close = true;
        } else if (st == IOState::AGAIN) {
            return;
        } else {
            keep_alive = it->second.keep_alive();
            method = it->second.last_method();
            url = it->second.last_url();
            status = it->second.last_status();
            body_bytes = it->second.last_body_bytes();

            if (keep_alive) {
                it->second.reset_for_next_request();

                if (it->second.has_complete_request()) {
                    if (!it->second.process()) {
                        need_close = true;
                    } else {
                        need_mod_write = true;
                    }
                } else {
                    need_mod_read = true;
                }
            } else {
                need_close = true;
            }
        }
    }

    if (!status.empty()) {
        std::ostringstream oss;
        oss << "[RES] fd=" << fd
            << " method=" << method
            << " url=" << url
            << " status=" << status
            << " bytes=" << body_bytes
            << " conn=" << (keep_alive ? "keep-alive" : "close");
        Logger::instance().info(oss.str());
    }

    if (need_close) {
        close_conn(fd);
        return;
    }
    if (need_mod_write) {
        modfd(fd, EPOLLOUT | EPOLLRDHUP);
    } else if (need_mod_read) {
        modfd(fd, EPOLLIN | EPOLLRDHUP);
    }
}


void WebServer::close_conn(int fd) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);

    std::lock_guard<std::mutex> lock(users_mutex_);
    auto it = users_.find(fd);
    if (it != users_.end()) {
        it->second.close_conn();
        users_.erase(it);
    }
    last_active_.erase(fd);
}

// 修改 fd 关注的事件（比如从读切到写）
void WebServer::modfd(int fd, int ev) {
    epoll_event event{};
    event.data.fd = fd;
    event.events = static_cast<uint32_t>(ev);
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
}
