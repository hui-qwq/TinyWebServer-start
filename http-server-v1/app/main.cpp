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
    auto env = [](const char* name, const char* def) -> std::string {
        const char* val = std::getenv(name);
        return val ? std::string(val) : std::string(def);
    };

    auto& mysql_pool = Sql_Connection_Pool::instance();
    if(!mysql_pool.init(env("DB_HOST", "127.0.0.1"),
                        std::stoi(env("DB_PORT", "3306")),
                        env("DB_USER", "tiny"),
                        env("DB_PASSWORD", "tiny123"),
                        env("DB_NAME", "tinywebserver"),
                        std::stoi(env("DB_POOL_SIZE", "4"))))
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

        const char* create_users_sql =
        "CREATE TABLE IF NOT EXISTS users("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "username VARCHAR(50) NOT NULL UNIQUE,"
        "password VARCHAR(64) NOT NULL,"
        "salt VARCHAR(64) NOT NULL DEFAULT ''"
        ")";

        if(mysql_query(conn, create_users_sql)) {
            std::cerr << "create users table failed: " << mysql_error(conn) << "\n";
            return 1;
        }

        // 兼容旧表：无 salt 列时添加（Mariadb 不支持 ADD COLUMN IF NOT EXISTS）
        if (mysql_query(conn, "SHOW COLUMNS FROM users LIKE 'salt'") == 0) {
            MYSQL_RES* res = mysql_store_result(conn);
            bool has_salt = (res && mysql_num_rows(res) > 0);
            if (res) mysql_free_result(res);
            if (!has_salt) {
                mysql_query(conn,
                    "ALTER TABLE users ADD COLUMN salt VARCHAR(64) NOT NULL DEFAULT ''");
            }
        }

        const char* create_articles_sql =
        "CREATE TABLE IF NOT EXISTS articles("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "title VARCHAR(200) NOT NULL,"
        "content MEDIUMTEXT NOT NULL,"
        "category VARCHAR(50) DEFAULT '',"
        "is_published TINYINT DEFAULT 1,"
        "view_count INT DEFAULT 0,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,"
        "updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP"
        ")";

        if(mysql_query(conn, create_articles_sql)) {
            std::cerr << "create articles table failed: " << mysql_error(conn) << "\n";
            return 1;
        }

        const char* create_sessions_sql =
        "CREATE TABLE IF NOT EXISTS sessions("
        "id INT PRIMARY KEY AUTO_INCREMENT,"
        "user_id INT NOT NULL,"
        "token VARCHAR(64) NOT NULL UNIQUE,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")";

        if(mysql_query(conn, create_sessions_sql)) {
            std::cerr << "create sessions table failed: " << mysql_error(conn) << "\n";
            return 1;
        }
    }

    WebServer server(static_cast<size_t>(threads), idle_timeout_sec);
    if (!server.init(port)) return 1;
    server.run();
    return 0;
}
