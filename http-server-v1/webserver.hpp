#pragma once

#include "http_conn.hpp"
#include <sys/epoll.h>
#include <unordered_map>

class WebServer {
public:
    WebServer();
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
    epoll_event events_[1024];
    std::unordered_map<int, HttpConn> users_;
};
