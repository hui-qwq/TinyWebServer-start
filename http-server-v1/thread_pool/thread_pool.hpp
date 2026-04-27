#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool(size_t thread_count = 4, size_t max_tasks = 10000);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool enqueue(Task task);
    void shutdown();

private:
    void worker_loop();
private:
    size_t max_tasks_;
    bool stopping_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Task> tasks_;
    std::vector<std::thread> workers_;
};

