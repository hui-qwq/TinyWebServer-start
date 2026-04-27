#include "../webserver/webserver.hpp"
#include "../logger/logger.hpp"
#include "../db/sql_connection_pool.hpp"
#include <cstdlib>
#include <iostream>
#include <mysql/mysql.h>
#include <string>

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
    int idle_timeout_sec = 30;

    if (argc > 4) {
        std::cerr << "Usage: " << argv[0] << " [port] [threads] [idle_timeout_sec]\n";
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

    if (argc >= 4 && !parse_int_in_range(argv[3], 1, 3600, idle_timeout_sec)) {
        std::cerr << "invalid idle_timeout_sec: " << argv[3] << " `(1-3600)\n";
        return 1;
    }

    Logger::instance().init("logs", true);
    Logger::instance().info(
        "starting server: port=" + std::to_string(port) + 
        ", thread=" + std::to_string(threads) +
        ", idle_timeout_sec=" + std::to_string(idle_timeout_sec)
    );
    auto& mysql_pool = Sql_Connection_Pool::instance();
    if(!mysql_pool.init("127.0.0.1",
                        3306,
                        "tiny",
                        "tiny123",
                        "tinywebserver",
                        4)) 
    {
        std::cerr << "mysql pool init failed\n";
        return 1;
    }

    {
        Sql_Connection_Guard guard(mysql_pool);
        MYSQL* conn = guard.get();

        if(conn == nullptr) {
            std::cerr << "mysql get connection failed\n";
            return 1;
        }

        std::string create_users_sql = 
        "CREATE TABLE IF NOT EXISTS users("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "username VARCHAR(50) NOT NULL UNIQUE,"
        "password VARCHAR(64) NOT NULL"
        ");";

        if(mysql_query(conn, create_users_sql.c_str())) {
            std::cerr << "create users table failed: " << mysql_error(conn) << "\n";
            return 1;
        }
    }

    WebServer server(static_cast<size_t>(threads), idle_timeout_sec);
    if (!server.init(port)) return 1;
    server.run();
    return 0;
}
