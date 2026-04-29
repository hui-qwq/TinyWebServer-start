#pragma once

#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <functional>
#include <deque>
#include <thread>
enum class LogLevel { INFO, WARN, ERROR };

class Logger {
private:
    using Task = std::function<void()>;
    size_t max_tasks_ = 10000;
    std::deque<Task> dq_;
    std::thread worker_;
    std::condition_variable cv_;

    std::mutex mutex_;
    std::ofstream file_;
    std::string log_dir_;
    std::string cur_date_;
    bool to_stdout_ = true;
    bool inited_ = false;
    bool stopping_ = false;
public:
    static Logger& instance();
    void init(const std::string& log_dir = "logs", bool to_stdout = true);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);
    void enqueue(Task task);
private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& msg);
    void open_log_file_locked(const std::string& date_str);
    void worker_loop();
    void shutdown();
    std::string now_time_str() const;
    std::string now_date_str() const;
    std::string level_str(LogLevel level) const;
};

