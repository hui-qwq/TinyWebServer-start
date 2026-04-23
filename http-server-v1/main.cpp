#include "webserver.hpp"

int main() {
    WebServer server;
    if(!server.init(8888)) return 1;
    server.run();
    return 0;
}