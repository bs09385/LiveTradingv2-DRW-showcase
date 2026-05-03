#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "logger/log_entry.h"
#include "queue/spsc_queue.h"

namespace lt {

// Handle returned to each producer thread for non-blocking logging
struct ProducerHandle {
    uint8_t id;
    SpscQueue<LogEntry>* queue;
};

class AsyncLogger {
public:
    explicit AsyncLogger(const std::string& log_file, std::size_t queue_capacity = 8192,
                         LogLevel min_level = LogLevel::INFO);
    ~AsyncLogger();

    // Create a producer handle for a specific thread
    ProducerHandle create_producer(const std::string& name);

    // Non-blocking log call (hot path for producer threads)
    static void log(ProducerHandle& handle, LogLevel level, std::string_view msg);

    // Start/stop the background writer thread
    void start();
    void stop();

    void set_min_level(LogLevel lvl) { min_level_.store(lvl, std::memory_order_relaxed); }

private:
    void writer_loop();

    std::string log_file_;
    std::size_t queue_capacity_;
    std::atomic<LogLevel> min_level_;

    struct ProducerInfo {
        std::string name;
        std::unique_ptr<SpscQueue<LogEntry>> queue;
    };
    std::vector<ProducerInfo> producers_;
    uint8_t next_id_ = 0;
    mutable std::mutex producers_mu_;

    std::thread writer_thread_;
    std::atomic<bool> running_{false};
    std::ofstream file_;
};

}  // namespace lt
