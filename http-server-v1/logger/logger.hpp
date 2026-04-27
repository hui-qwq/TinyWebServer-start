#pragma once

#include <fstream>
#include <mutex>
#include <string>

enum class LogLevel { INFO, WARN, ERROR };

class Logger {
public:
    static Logger& instance();

    void init(const std::string& log_dir = "logs", bool to_stdout = true);
    void info(const std::string& msg);
    void warn(const std::string& msg);
    void error(const std::string& msg);

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogLevel level, const std::string& msg);
    void open_log_file_locked(const std::string& date_str);
    std::string now_time_str() const;
    std::string now_date_str() const;
    std::string level_str(LogLevel level) const;

private:
    std::mutex mutex_;
    std::ofstream file_;
    std::string log_dir_;
    std::string cur_date_;
    bool to_stdout_ = true;
    bool inited_ = false;
};
