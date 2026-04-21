#include "webserver.hpp"

int main() {
    WebServer server;
    server.init(8888);
    server.run();
    return 0;
}