#pragma once

#include <mysql/mysql.h>

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>

class Sql_Connection_Pool {
public:
    static Sql_Connection_Pool& instance();
    
    bool init(const std::string& host,
            int port,
            const std::string& user,
            const std::string& password,
            const std::string& database,
            int pool_size);
    MYSQL* get_connection();
    void release_connection(MYSQL* conn);
    void shutdown();

private:
    Sql_Connection_Pool() = default;
    ~Sql_Connection_Pool();

    Sql_Connection_Pool(const Sql_Connection_Pool&) = delete;
    Sql_Connection_Pool& operator=(const Sql_Connection_Pool&) = delete;

private:
    std::queue<MYSQL*> conns_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool inited_ = false;
    bool stopping_ = false;
};

class Sql_Connection_Guard {
public:

    explicit Sql_Connection_Guard(Sql_Connection_Pool& pool) : pool_(pool), conn_(pool_.get_connection()) {};
    ~Sql_Connection_Guard() {
        if(conn_ != nullptr) {
            pool_.release_connection(conn_);
        }
    }
    Sql_Connection_Guard(const Sql_Connection_Guard&) = delete;
    Sql_Connection_Guard& operator=(const Sql_Connection_Guard&) = delete;

    MYSQL* get() const {return conn_;}
private:
    Sql_Connection_Pool& pool_;
    MYSQL* conn_;
};