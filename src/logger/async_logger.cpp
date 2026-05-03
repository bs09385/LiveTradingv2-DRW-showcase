#include "logger/async_logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>

#include "common/clock.h"

namespace lt {

AsyncLogger::AsyncLogger(const std::string& log_file, std::size_t queue_capacity,
                         LogLevel min_level)
    : log_file_(log_file), queue_capacity_(queue_capacity), min_level_(min_level) {}

AsyncLogger::~AsyncLogger() { stop(); }

ProducerHandle AsyncLogger::create_producer(const std::string& name) {
    std::lock_guard<std::mutex> lk(producers_mu_);
    auto id = next_id_++;
    auto queue = std::make_unique<SpscQueue<LogEntry>>(queue_capacity_);
    ProducerHandle handle{id, queue.get()};
    producers_.push_back({name, std::move(queue)});
    return handle;
}

void AsyncLogger::log(ProducerHandle& handle, LogLevel level, std::string_view msg) {
    if (handle.queue == nullptr) return;
    LogEntry entry;
    entry.timestamp = SteadyClock::now();
    entry.level = level;
    entry.producer_id = handle.id;
    entry.set_message(msg);
    handle.queue->try_push(entry);
}

void AsyncLogger::start() {
    if (running_.exchange(true)) return;

    file_.open(log_file_, std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        std::cerr << "Failed to open log file: " << log_file_ << "\n";
    }

    writer_thread_ = std::thread([this] {
        try {
            writer_loop();
        } catch (const std::exception& ex) {
            std::cerr << "AsyncLogger writer thread exception: " << ex.what() << "\n";
            running_.store(false, std::memory_order_relaxed);
        } catch (...) {
            std::cerr << "AsyncLogger writer thread unknown exception\n";
            running_.store(false, std::memory_order_relaxed);
        }
    });
}

void AsyncLogger::stop() {
    if (!running_.exchange(false)) return;
    if (writer_thread_.joinable()) writer_thread_.join();

    // Drain remaining entries
    {
        std::lock_guard<std::mutex> lk(producers_mu_);
        for (auto& p : producers_) {
            while (auto* entry = p.queue->front()) {
                // Write remaining
                char buf[512];
                auto ts = entry->timestamp;
                auto secs = ts / 1'000'000'000LL;
                auto frac_ms = (ts % 1'000'000'000LL) / 1'000'000LL;
                std::snprintf(buf, sizeof(buf), "[%lld.%03lld] [%s] [%s] %s\n",
                              static_cast<long long>(secs),
                              static_cast<long long>(frac_ms),
                              log_level_str(entry->level),
                              p.name.c_str(), entry->message);
                if (file_.is_open()) file_ << buf;
                p.queue->pop();
            }
        }
    }

    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void AsyncLogger::writer_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        bool found_any = false;

        {
            std::lock_guard<std::mutex> lk(producers_mu_);
            for (auto& p : producers_) {
                auto current_min_level = min_level_.load(std::memory_order_relaxed);
                while (auto* entry = p.queue->front()) {
                    if (entry->level < current_min_level) {
                        p.queue->pop();
                        continue;
                    }

                    char buf[512];
                    auto ts = entry->timestamp;
                    auto secs = ts / 1'000'000'000LL;
                    auto frac_ms = (ts % 1'000'000'000LL) / 1'000'000LL;
                    std::snprintf(buf, sizeof(buf), "[%lld.%03lld] [%s] [%s] %s\n",
                                  static_cast<long long>(secs),
                                  static_cast<long long>(frac_ms),
                                  log_level_str(entry->level),
                                  p.name.c_str(), entry->message);

                    if (file_.is_open()) file_ << buf;

                    p.queue->pop();
                    found_any = true;
                }
            }
        }

        if (!found_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            if (file_.is_open()) file_.flush();
        }
    }
}

}  // namespace lt
