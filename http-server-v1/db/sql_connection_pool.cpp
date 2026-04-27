#include "sql_connection_pool.hpp"
#include "../logger/logger.hpp"
#include <mutex>
#include <mysql/mysql.h>

Sql_Connection_Pool& Sql_Connection_Pool::instance() {
    static Sql_Connection_Pool pool;
    return pool;
}

Sql_Connection_Pool::~Sql_Connection_Pool() {
    shutdown();
}

bool Sql_Connection_Pool::init(const std::string& host,
        int port,
        const std::string& user,
        const std::string& password,
        const std::string& database,
        int pool_size)
{
    if (pool_size <= 0) {
        Logger::instance().error("mysql pool_size must be > 0");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if(inited_) {
        return true;
    }

    for(int i = 0; i < pool_size; ++ i) {
        MYSQL* conn = mysql_init(nullptr);
        if(conn == nullptr) {
            Logger::instance().error("mysql_init failed");
            return false;
        }

        MYSQL* connected = mysql_real_connect(
            conn,
            host.c_str(),
            user.c_str(),
            password.c_str(),
            database.c_str(),
            port,
            nullptr,
            0
        );

        if(connected == nullptr) {
            Logger::instance().error("mysql_real_connect failed: " + std::string(mysql_error(conn)));
            mysql_close(conn);
            return false;
        }

        conns_.push(connected);
    }

    inited_ = true;
    stopping_ = false;
    Logger::instance().info("mysql connection pool init success, size=" + std::to_string(pool_size));
    return true;
}


MYSQL* Sql_Connection_Pool::get_connection() {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this]() {
        return stopping_ || !conns_.empty();
    });

    if(stopping_ || conns_.empty()) return nullptr;

    MYSQL* conn = conns_.front();
    conns_.pop();
    return conn;
}

void Sql_Connection_Pool::release_connection(MYSQL* conn) {
    if(conn == nullptr) return ;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_) {
            mysql_close(conn);
            return ;
        }
        conns_.push(conn);
    }

    cv_.notify_one();
}

void Sql_Connection_Pool::shutdown() {
    std::queue<MYSQL*> to_close;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_) return ;

        stopping_ = true;
        inited_ = false;
        std::swap(to_close, conns_);
    }

    cv_.notify_all();

    while(!to_close.empty()) {
        MYSQL* conn = to_close.front(); 
        to_close.pop();
        mysql_close(conn);
    }

    Logger::instance().info("mysql connection pool shutdown");
}
