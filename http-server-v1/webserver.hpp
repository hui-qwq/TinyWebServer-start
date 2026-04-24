#pragma once

#include "http_conn.hpp"
#include <sys/epoll.h>
#include <unordered_map>
#include "thread_pool.hpp"
#include <mutex>

class WebServer {
public:
    explicit WebServer(size_t thread_count = 4);
    ~WebServer();

    bool init(int port);
    void run();

private:
    bool event_listen();
    void handle_accept();
    void handle_read(int fd);
    void handle_write(int fd);
    void close_conn(int fd);
    void modfd(int fd, int ev);

private:
    int port_;
    int listenfd_;
    int epfd_;
    ThreadPool pool_;
    std::mutex users_mutex_;
    epoll_event events_[1024];
    std::unordered_map<int, HttpConn> users_;
};
