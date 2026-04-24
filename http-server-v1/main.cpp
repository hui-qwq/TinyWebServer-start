#include "webserver.hpp"

#include <cstdlib>
#include <iostream>

namespace {
bool parse_int_in_range(const char* s, int min_v, int max_v, int& out) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    if (v < min_v || v > max_v) return false;
    out = static_cast<int>(v);
    return true;
}
}  // namespace

int main(int argc, char* argv[]) {
    int port = 8888;
    int threads = 4;

    if (argc > 3) {
        std::cerr << "Usage: " << argv[0] << " [port] [threads]\n";
        return 1;
    }

    if (argc >= 2 && !parse_int_in_range(argv[1], 1, 65535, port)) {
        std::cerr << "invalid port: " << argv[1] << " (1-65535)\n";
        return 1;
    }

    if (argc >= 3 && !parse_int_in_range(argv[2], 1, 256, threads)) {
        std::cerr << "invalid threads: " << argv[2] << " (1-256)\n";
        return 1;
    }

    std::cout << "starting server: port=" << port
              << ", threads=" << threads << std::endl;

    WebServer server(static_cast<size_t>(threads));
    if (!server.init(port)) return 1;
    server.run();
    return 0;
}
