#include "../logger/logger.hpp"
#include "../db/sql_connection_pool.hpp"

#include <iostream>
#include <mysql/mysql.h>

int main() {
    Logger::instance().init("logs", true);

    auto& pool = Sql_Connection_Pool::instance();
    bool ok = pool.init("127.0.0.1",
                        3306,
                        "tiny",
                        "tiny123",
                        "tinywebserver",
                        2);
    if (!ok) {
        std::cerr << "pool init failed\n";
        return 1;
    }
    {
        Sql_Connection_Guard guard(pool);
        MYSQL* conn = guard.get();
        if (conn == nullptr) {
            std::cerr << "get_connection failed\n";
            return 1;
        }

        const char* sql =
            "CREATE TABLE IF NOT EXISTS users ("
            "id INT PRIMARY KEY AUTO_INCREMENT,"
            "username VARCHAR(50) NOT NULL UNIQUE,"
            "password VARCHAR(64) NOT NULL"
            ");";

        if (mysql_query(conn, sql) != 0) {
            std::cerr << "mysql_query failed: " << mysql_error(conn) << "\n";
            return 1;
        }

        std::cout << "mysql pool test ok\n";
    }
    pool.shutdown();
    return 0;
}
