#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

#include "rigtorp/SPSCQueue.h"

namespace lt {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity) : queue_(capacity) {}

    // Non-copyable, non-movable
    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    // Returns true if push succeeded
    bool try_push(const T& item) {
        bool ok = queue_.try_push(item);
        if (ok) {
            push_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            overflow_count_.fetch_add(1, std::memory_order_relaxed);
        }
        return ok;
    }

    // Returns item if available
    T* front() { return queue_.front(); }

    void pop() {
        queue_.pop();
        pop_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // Convenience: try_pop into optional
    std::optional<T> try_pop() {
        T* ptr = queue_.front();
        if (ptr) {
            T val = std::move(*ptr);
            queue_.pop();
            pop_count_.fetch_add(1, std::memory_order_relaxed);
            return val;
        }
        return std::nullopt;
    }

    std::size_t size() const { return queue_.size(); }
    bool empty() const { return queue_.empty(); }

    // Spin-yield push: retries up to max_spins times with yield between attempts.
    // Returns true if push eventually succeeded, false if exhausted.
    bool push_spin(const T& item, int max_spins = 1000) {
        if (queue_.try_push(item)) {
            push_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        for (int i = 0; i < max_spins; ++i) {
            std::this_thread::yield();
            if (queue_.try_push(item)) {
                push_count_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Stats (relaxed reads, suitable for periodic reporting)
    int64_t push_count() const { return push_count_.load(std::memory_order_relaxed); }
    int64_t pop_count() const { return pop_count_.load(std::memory_order_relaxed); }
    int64_t overflow_count() const { return overflow_count_.load(std::memory_order_relaxed); }

private:
    rigtorp::SPSCQueue<T> queue_;
    alignas(64) std::atomic<int64_t> push_count_{0};
    alignas(64) std::atomic<int64_t> pop_count_{0};
    alignas(64) std::atomic<int64_t> overflow_count_{0};
};

}  // namespace lt
