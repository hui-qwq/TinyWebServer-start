#include "webserver.hpp"
#include "http_conn.hpp"

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// 把 fd 设置为非阻塞，配合 epoll 边缘/水平触发都更稳妥
void set_nonblock(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    if (flag >= 0) {
        fcntl(fd, F_SETFL, flag | O_NONBLOCK);
    }
}
}  // namespace

WebServer::WebServer() : port_(0), listenfd_(-1), epfd_(-1) {}

WebServer::~WebServer() {
    if (listenfd_ >= 0) {
        close(listenfd_);
    }
    if (epfd_ >= 0) {
        close(epfd_);
    }
}

void WebServer::init(int port) {
    port_ = port;
    event_listen();
}

void WebServer::event_listen() {
    // 1) 创建监听 socket
    listenfd_ = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd_ >= 0);

    int on = 1;
    setsockopt(listenfd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(static_cast<uint16_t>(port_));

    // 2) 绑定端口并开始监听
    assert(bind(listenfd_, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) == 0);
    assert(listen(listenfd_, 5) == 0);

    set_nonblock(listenfd_);

    // 3) 创建 epoll 并把监听 fd 加进去
    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        std::cerr << "epoll_create error" << std::endl;
        close(listenfd_);
        listenfd_ = -1;
        return;
    }

    epoll_event ev{};
    ev.data.fd = listenfd_;
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listenfd_, &ev) < 0) {
        std::cerr << "epoll_ctl add listenfd error" << std::endl;
        close(listenfd_);
        listenfd_ = -1;
        close(epfd_);
        epfd_ = -1;
        return;
    }

    std::cout << "listening on " << port_ << "..." << std::endl;
}

// epoll 主循环：分发连接、读写、异常断开事件
void WebServer::run() {
    if (listenfd_ < 0 || epfd_ < 0) {
        return;
    }

    while (true) {
        int n = epoll_wait(epfd_, events_, 1024, -1);
        if (n < 0) {
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events_[i].data.fd;
            uint32_t e = events_[i].events;

            if (fd == listenfd_) {
                handle_accept();
            } else if (e & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                std::cout << "client[" << fd << "] disconnected" << std::endl;
                close_conn(fd);
            } else if (e & EPOLLIN) {
                handle_read(fd);
            } else if (e & EPOLLOUT) {
                handle_write(fd);
            }
        }
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
            std::cerr << "accept error" << std::endl;
            break;
        }

        set_nonblock(clfd);

        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLRDHUP;
        ev.data.fd = clfd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, clfd, &ev) < 0) {
            std::cerr << "epoll_ctl add error" << std::endl;
            close(clfd);
            continue;
        }

        users_[clfd].init(clfd);
        std::cout << "client[" << clfd << "] connected" << std::endl;
    }
}

// 读请求并构造响应，然后切到可写事件发送数据
void WebServer::handle_read(int fd) {
    auto it = users_.find(fd);
    if (it == users_.end()) {
        close_conn(fd);
        return;
    }

    IOState st = it->second.read_once();
    if (st == IOState::CLOSED || st == IOState::ERROR) {
        std::cout << "client[" << fd << "] disconnected" << std::endl;
        close_conn(fd);
        return;
    }
    if (st == IOState::AGAIN) {
        std::cout << "client[" << fd << "] read AGAIN" << std::endl;
        return;
    }

    if (st == IOState::TOO_LARGE) {
        std::cout << "client[" << fd << "] memory exceeded" << std::endl;
        it->second.set_413_response();
        modfd(fd, EPOLLOUT | EPOLLRDHUP);
        return;
    }

    if (!it->second.process()) {
        close_conn(fd);
        return;
    }

    modfd(fd, EPOLLOUT | EPOLLRDHUP);
}

void WebServer::handle_write(int fd) {
    auto it = users_.find(fd);
    if (it == users_.end()) {
        close_conn(fd);
        return;
    }

    auto st = it->second.write();
    if (st == IOState::ERROR || st == IOState::CLOSED) {
        std::cout << "client[" << fd << "] write ERROR/CLOSED" << std::endl;
        close_conn(fd);
        return;
    }

    if (st == IOState::AGAIN) {
        // 等待下次可写事件继续发送
        return;
    }

    const bool keep_alive = it->second.keep_alive();
    std::cout << "[RES] fd=" << fd
              << " method=" << it->second.last_method()
              << " url=" << it->second.last_url()
              << " status=" << it->second.last_status()
              << " bytes=" << it->second.last_body_bytes()
              << " conn=" << (keep_alive ? "keep-alive" : "close")
              << std::endl;

    if (keep_alive) {
        // keep-alive: 清理本次请求状态，回到读事件等待下一个请求
        it->second.reset_for_next_request();

        if(it->second.has_complete_request()) {
            if(!it->second.process()) {
                close_conn(fd);
                return ;
            }
            modfd(fd, EPOLLOUT | EPOLLRDHUP);
        }
        else modfd(fd, EPOLLIN | EPOLLRDHUP);
        return;
    }

    close_conn(fd);
}

void WebServer::close_conn(int fd) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);

    auto it = users_.find(fd);
    if (it != users_.end()) {
        it->second.close_conn();
        users_.erase(it);
    } else {
        close(fd);
    }
}

// 修改 fd 关注的事件（比如从读切到写）
void WebServer::modfd(int fd, int ev) {
    epoll_event event{};
    event.data.fd = fd;
    event.events = static_cast<uint32_t>(ev);
    epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event);
}
