#pragma once

#include <atomic>
#include <cstdint>

#include "common/types.h"
#include "events/event_variant.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"

namespace lt {

class StrategySchedulerStub {
public:
    StrategySchedulerStub(SpscQueue<MarketNotification>& queue, Metrics& metrics,
                          AsyncLogger& logger, int64_t stats_interval_ms = 5000);

    // Run loop (blocks, call from dedicated thread)
    void run();

    // Request shutdown (thread-safe)
    void request_shutdown();

    // Stats accessors
    int64_t total_events() const { return total_events_.load(std::memory_order_relaxed); }
    int64_t events_by_kind(NotificationKind kind) const {
        return kind_counts_[static_cast<int>(kind)].load(std::memory_order_relaxed);
    }

private:
    void dump_stats();

    SpscQueue<MarketNotification>& queue_;
    Metrics& metrics_;
    AsyncLogger& logger_;
    ProducerHandle log_handle_;
    int64_t stats_interval_ms_;

    std::atomic<bool> running_{false};
    std::atomic<int64_t> total_events_{0};
    std::atomic<int64_t> kind_counts_[5]{};

    // Latency tracking
    int64_t latency_sum_ns_ = 0;
    int64_t latency_count_ = 0;
    int64_t latency_max_ns_ = 0;

    Timestamp_ns last_stats_time_ = 0;
};

}  // namespace lt
