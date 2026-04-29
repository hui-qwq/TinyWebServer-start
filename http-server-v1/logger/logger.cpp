#include "logger.hpp"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>


Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    shutdown();
}

void Logger::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_) {
            return ;
        }
        stopping_ = true;
    }

    cv_.notify_all();

    if(worker_.joinable()) {
        worker_.join();
    }
}
void Logger::worker_loop() {
    while(true) {
        Task task_;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return dq_.size() || stopping_;
            });

            if(dq_.empty() && stopping_) return ;

            task_ = std::move(dq_.front()); 
            dq_.pop_front();
        }
        task_();
    }
}

void Logger::init(const std::string& log_dir, bool to_stdout)  {

    bool need_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        log_dir_ = log_dir;
        to_stdout_ = to_stdout;
        stopping_ = false;

        std::error_code ec;
        std::filesystem::create_directories(log_dir, ec);
        if (ec) {
            std::cerr << "[LOGGER][ERROR] create_directories failed: " << ec.message() << '\n';
        }
        open_log_file_locked(now_date_str());

        inited_ = true;

        need_start = !worker_.joinable();
    }

    if (need_start) {
        worker_ = std::thread([this]() {
            worker_loop();
        });
    }
}


void Logger::info(const std::string& msg) { 
    enqueue([this, msg]() {
        log(LogLevel::INFO, msg);
    });
}
void Logger::warn(const std::string& msg) {
    enqueue([this, msg]() {
        log(LogLevel::WARN, msg); 
    });
}
void Logger::error(const std::string& msg) {
    enqueue([this, msg]() {
        log(LogLevel::ERROR, msg);
    });
}

void Logger::enqueue(Task task) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || dq_.size() >= max_tasks_) {
            return ;
        }
        dq_.push_back(std::move(task));
    }

    cv_.notify_one();
}

void Logger::log(LogLevel level, const std::string& msg) {

    if (!inited_) {
        log_dir_ = "logs";
        to_stdout_ = true;
        std::error_code ec;
        std::filesystem::create_directories(log_dir_, ec);
        open_log_file_locked(now_date_str());
        inited_ = true;
    }

    const std::string date_str = now_date_str();
    if (date_str != cur_date_ || !file_.is_open()) {
        open_log_file_locked(date_str);
    }

    const std::string line =
        "[" + now_time_str() + "][" + level_str(level) + "] " + msg;

    if (file_.is_open()) {
        file_ << line << '\n';
        file_.flush();
    }

    if (to_stdout_) {
        if (level == LogLevel::ERROR) {
            std::cerr << line << '\n';
        } else {
            std::cout << line << '\n';
        }
    }
}


void Logger::open_log_file_locked(const std::string& date_str) {
    cur_date_ = date_str;

    if (file_.is_open()) {
        file_.close();
    }

    const std::string path = log_dir_ + "/" + cur_date_ + ".log";
    file_.open(path, std::ios::app);
    if (!file_.is_open()) {
        std::cerr << "[LOGGER][ERROR] open log file failed: " << path << '\n';
    }
}


std::string Logger::now_time_str() const {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}


std::string Logger::now_date_str() const {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y-%m-%d");
    return oss.str();
}


std::string Logger::level_str(LogLevel level) const {
    switch (level) {
        case LogLevel::INFO:
            return "INFO";
        case LogLevel::WARN:
            return "WARN";
        case LogLevel::ERROR:
            return "ERROR";
    }
    return "INFO";
}
