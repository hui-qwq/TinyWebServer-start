#include "thread_pool.hpp"
#include "../logger/logger.hpp"
#include <cstddef>
#include <mutex>

ThreadPool::ThreadPool(size_t thread_count, size_t max_tasks) {
    max_tasks_ = max_tasks;
    stopping_ = false;
    for(size_t i = 0; i < thread_count; ++ i) {
        workers_.emplace_back([this]() {
            worker_loop();
        });
    }
}


ThreadPool::~ThreadPool() {
    shutdown();
}


bool ThreadPool::enqueue(Task task) {
    if(!task) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if(stopping_ || tasks_.size() >= max_tasks_) {
            return false;
        }

        tasks_.emplace_back(std::move(task));
    }

    cv_.notify_one();
    return true;
}


void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_) return ;
        stopping_ = true;
    }

    for(auto& it : workers_) {
        if(it.joinable()) {
            it.join();
        }
    }

    workers_.clear();
}

void ThreadPool::worker_loop() {
    while(true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stopping_ || !tasks_.empty();
            });

            if(stopping_ && tasks_.empty()) return ;
            
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        try {
            task();
        } catch(...) {
            Logger::instance().error("thread pool task threw exception");
        }
    }
}
