#include "scheduler/strategy_scheduler_stub.h"

#include <exception>
#include <cstdio>
#include <thread>

#include "common/clock.h"

namespace lt {

StrategySchedulerStub::StrategySchedulerStub(SpscQueue<MarketNotification>& queue, Metrics& metrics,
                                             AsyncLogger& logger, int64_t stats_interval_ms)
    : queue_(queue),
      metrics_(metrics),
      logger_(logger),
      log_handle_(logger.create_producer("scheduler")),
      stats_interval_ms_(stats_interval_ms) {
    for (auto& c : kind_counts_) c.store(0, std::memory_order_relaxed);
}

void StrategySchedulerStub::run() {
    running_.store(true);
    last_stats_time_ = SteadyClock::now();

    int spin_count = 0;
    constexpr int kSpinLimit = 1000;
    constexpr int kYieldLimit = 100;

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Scheduler stub started");

    try {
        while (running_.load(std::memory_order_relaxed)) {
            auto* notif = queue_.front();
            if (notif) {
                spin_count = 0;

                // Track latency
                auto now = SteadyClock::now();
                if (notif->recv_ts > 0) {
                    auto latency = now - notif->recv_ts;
                    latency_sum_ns_ += latency;
                    latency_count_++;
                    if (latency > latency_max_ns_) latency_max_ns_ = latency;
                    metrics_.record_latency(MetricId::E2E_LATENCY_NS, MetricId::E2E_LATENCY_COUNT,
                                            latency);
                }

                // Count by kind
                auto idx = static_cast<int>(notif->kind);
                if (idx >= 0 && idx < 5) {
                    kind_counts_[idx].fetch_add(1, std::memory_order_relaxed);
                }
                total_events_.fetch_add(1, std::memory_order_relaxed);
                metrics_.inc(MetricId::SCHED_EVENTS);
                metrics_.inc(MetricId::QUEUE_POPS);

                queue_.pop();

                // Periodic stats dump
                if (now - last_stats_time_ > stats_interval_ms_ * 1'000'000LL) {
                    dump_stats();
                    last_stats_time_ = now;
                }
            } else {
                // Spin-then-yield-then-sleep wait strategy
                ++spin_count;
                if (spin_count < kSpinLimit) {
                    // Busy spin
#if defined(_MSC_VER)
                    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
                    __builtin_ia32_pause();
#endif
                } else if (spin_count < kSpinLimit + kYieldLimit) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }

                // Check for stats dump even when idle
                auto now = SteadyClock::now();
                if (now - last_stats_time_ > stats_interval_ms_ * 1'000'000LL) {
                    dump_stats();
                    last_stats_time_ = now;
                }
            }
        }
    } catch (const std::exception& ex) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "Scheduler loop exception: %s", ex.what());
        AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
    } catch (...) {
        AsyncLogger::log(log_handle_, LogLevel::ERROR, "Scheduler loop unknown exception");
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Scheduler stub stopped");
}

void StrategySchedulerStub::request_shutdown() { running_.store(false, std::memory_order_relaxed); }

void StrategySchedulerStub::dump_stats() {
    char buf[LogEntry::kMaxMsg];
    int64_t avg_lat = latency_count_ > 0 ? (latency_sum_ns_ / latency_count_) : 0;

    std::snprintf(buf, sizeof(buf),
                  "Stats: total=%lld snap=%lld delta=%lld bbo=%lld tick=%lld trade=%lld "
                  "avg_lat=%lld ns max_lat=%lld ns",
                  static_cast<long long>(total_events_.load(std::memory_order_relaxed)),
                  static_cast<long long>(kind_counts_[0].load(std::memory_order_relaxed)),
                  static_cast<long long>(kind_counts_[1].load(std::memory_order_relaxed)),
                  static_cast<long long>(kind_counts_[2].load(std::memory_order_relaxed)),
                  static_cast<long long>(kind_counts_[3].load(std::memory_order_relaxed)),
                  static_cast<long long>(kind_counts_[4].load(std::memory_order_relaxed)),
                  static_cast<long long>(avg_lat),
                  static_cast<long long>(latency_max_ns_));

    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
}

}  // namespace lt
