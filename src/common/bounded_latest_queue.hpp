#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stop_token>
#include <utility>

template <typename T>
class BoundedLatestQueue {
public:
    struct PushResult {
        bool accepted = false;
        bool dropped_oldest = false;
    };

    explicit BoundedLatestQueue(std::size_t capacity) : capacity_(capacity) {}

    PushResult push(T value)
    {
        const std::lock_guard lock(mutex_);
        if (closed_ || capacity_ == 0) {
            return {};
        }
        bool dropped_oldest = false;
        if (queue_.size() == capacity_) {
            queue_.pop_front();
            dropped_oldest = true;
        }
        queue_.push_back(std::move(value));
        condition_.notify_one();
        return {true, dropped_oldest};
    }

    bool waitPop(T &value, std::stop_token stop_token)
    {
        std::unique_lock lock(mutex_);
        const bool ready = condition_.wait(
            lock, stop_token, [this] { return closed_ || !queue_.empty(); });
        if (!ready || queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close()
    {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        condition_.notify_all();
    }

    std::size_t size() const
    {
        const std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    std::size_t capacity_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<T> queue_;
    bool closed_ = false;
};
