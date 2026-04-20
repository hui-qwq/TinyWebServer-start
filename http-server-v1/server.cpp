#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <set>
#include <string>
#include <fstream>
#include <sstream>

#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std;

struct Request {
    string method;
    string url;
    string version;
};
string root = "html/";
set<int> clients;

string make_response(const string& status, const string& html) {
    string res =
        "HTTP/1.1 " + status + "\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: " + to_string(html.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" +
        html;

    return res;
}

Request parse_request_line(const string& msg) {
    size_t pos = msg.find("\r\n");
    if (pos == string::npos) return {"", "",""};

    string line = msg.substr(0, pos);

    string method, url, version;
    stringstream ss(line);
    ss >> method >> url >> version;

    Request res = {method, url, version};
    return res;
}



string read_file(const string& filename) {
    ifstream fin(filename);
    if(!fin.is_open()) return "";
    stringstream buffer;
    buffer << fin.rdbuf();
    return buffer.str();
}

string make_time_page() {
    time_t now = time(nullptr);
    string t = ctime(&now);

    return
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<title>Current Time</title>"
        "<style>"
        "body { font-family: Arial, sans-serif; text-align: center; margin-top: 100px; background: #eef6ff; }"
        ".box { width: 520px; margin: 0 auto; background: white; padding: 30px; border-radius: 16px; box-shadow: 0 4px 20px rgba(0,0,0,0.08); }"
        "a { display: inline-block; margin-top: 20px; padding: 10px 20px; background: #ff9800; color: white; text-decoration: none; border-radius: 8px; }"
        "a:hover { background: #f57c00; }"
        "</style>"
        "</head>"
        "<body>"
        "<div class=\"box\">"
        "<h1>Current Time ⏰</h1>"
        "<p>" + t + "</p>"
        "<a href=\"/\">Back Home</a>"
        "</div>"
        "</body>"
        "</html>";
}

pair<string, string> route(const string& url) {
    if (url == "/" || url.empty()) {
        return { "200 OK",read_file(root + "index.html")};
    } else if (url == "/hello") {
        return {"200 OK", read_file(root + "hello.html")};
    } else if (url == "/time") {
        return { "200 OK", make_time_page()};
    } else {
        return { "404 Not Found", read_file(root + "404.html")};

    }


}
void set_nonblock(int fd) {
    int flag = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flag | O_NONBLOCK);
}

void ins(int fd, int epfd) {
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    int res = epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    if (res < 0) {
        cerr << "epoll_ctl add error" << endl;
        close(fd);
        return;
    }
    clients.insert(fd);
}

void era(int fd, int epfd) {
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    clients.erase(fd);
    close(fd);
}

void  handle_accept(int listenfd, int epfd) {
    sockaddr_in cliaddr{};
    socklen_t len = sizeof(cliaddr);
    int clfd = accept(listenfd, (sockaddr*)&cliaddr, &len);
    if (clfd < 0) {
        cerr << "accept error" << endl;
        return ;
    }
    set_nonblock(clfd);

    ins(clfd, epfd);
    cout << "client[" << clfd << "] connected" << endl;
}

bool read_request(int fd, string& req) {
    char buf[4096];
    while(1) {
        int rn = recv(fd, buf, sizeof(buf), 0);
        if(rn > 0) {
            req.append(buf, rn);
            if(req.find("\r\n\r\n") != string::npos) {
                return true;
            }
        }else if(rn == 0) {
            return false;
        }else {
            if(errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return false;
        }
    }
    return req.find("\r\n\r\n") != string::npos;
}

void handle_request(int fd, int epfd) {
    string req;
    bool ok = read_request(fd, req);

    if (!ok) {
        cout << "client[" << fd << "] disconnected" << endl;
        era(fd, epfd);
        return;
    }

    cout << "======= request from client[" << fd << "] =======" << endl;
    cout << req << endl;
    cout << "==============================================" << endl;

    Request r = parse_request_line(req);
    if (r.method.empty() || r.url.empty() || r.version.empty()) {
        string body = "<h1>400 Bad Request</h1>";
        string res = make_response("400 Bad Request", body);
        send(fd, res.c_str(), res.size(), 0);
        era(fd, epfd);
        return;
    }

    if (r.method != "GET") {
        string body = "<h1>405 Method Not Allowed</h1>";
        string res = make_response("405 Method Not Allowed", body);
        send(fd, res.c_str(), res.size(), 0);
        era(fd, epfd);
        return;
    }

    cout << "parsed url: " << r.url << endl;

    auto [status, body] = route(r.url);

    if (body.empty()) {
        body = "<h1>500 Internal Server Error</h1>";
        status = "500 Internal Server Error";
    }

    string res = make_response(status, body);
    send(fd, res.c_str(), res.size(), 0);

    era(fd, epfd);
}

int main() {
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(8888);

    assert(bind(listenfd, (sockaddr*)&server_addr, sizeof(server_addr)) == 0);
    assert(listen(listenfd, 5) == 0);

    cout << "listening on 8888..." << endl;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        close(listenfd);
        cerr << "epoll_create error" << endl;
        return 1;
    }

    epoll_event ev{}, events[100];
    ev.data.fd = listenfd;
    ev.events = EPOLLIN;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) {
        cerr << "epoll_ctl add listenfd error" << endl;
        close(listenfd);
        close(epfd);
        return 1;
    }

    while (1) {
        int n = epoll_wait(epfd, events, 100, -1);
        if (n < 0) break;

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            auto e = events[i].events;

            if (fd == listenfd) {
                
                handle_accept(listenfd, epfd);

            } else if (e & (EPOLLERR | EPOLLHUP)) {
                cout << "client[" << fd << "] disconnected" << endl;
                era(fd, epfd);

            } else if (e & EPOLLIN) {
                handle_request(fd, epfd);
            }
        }
    }

    close(listenfd);
    close(epfd);
    return 0;
}