#pragma once

#include <cstdint>
#include <array>
#include "common/types.h"
#include "exec/exec_intent.h"

namespace lt {

struct RateLimitConfig {
    int global_tokens_per_10s = 9000;
    int order_tokens_per_10s = 3500;
    int cancel_tokens_per_10s = 3000;
    int heartbeat_tokens_per_10s = 200;
    int64_t backoff_base_ms = 500;
    int64_t backoff_max_ms = 30000;
    int max_consecutive_429s = 5;
};

class RateLimiter {
public:
    explicit RateLimiter(const RateLimitConfig& config);

    // Returns true if a request of this type is allowed right now
    bool try_acquire(ExecIntentType type, Timestamp_ns now);

    // When is the next request of this type allowed?
    Timestamp_ns next_allowed(ExecIntentType type, Timestamp_ns now) const;

    // Record an HTTP response (adjusts backoff on 429/503)
    void record_response(int http_status, Timestamp_ns now);

    // Record RTT for pressure detection
    void record_rtt(int64_t rtt_ns);

    // Is the gateway in degraded state?
    bool is_degraded() const;

    // Is the exchange unavailable (503)?
    bool is_exchange_unavailable() const { return exchange_unavailable_; }

    // Is the matching engine down (10 consecutive 425s)?
    bool is_matching_engine_down() const { return consecutive_425s_ >= k425MaxConsecutive; }

    // Reset backoff (e.g., after successful request)
    void reset_backoff();

    // Metrics
    int64_t throttled_count() const { return throttled_count_; }
    int64_t backoff_count() const { return backoff_count_; }

private:
    static constexpr int kNumBuckets = 10;       // 10 sub-windows
    static constexpr int64_t kWindowMs = 10000;   // 10-second window
    static constexpr int64_t kBucketMs = 1000;    // 1-second sub-window

    struct SlidingWindow {
        int limit_per_10s = 0;
        std::array<int, kNumBuckets> buckets{};
        int current_bucket = 0;
        Timestamp_ns last_slide_ts = 0;

        void slide(Timestamp_ns now);
        int total() const;
        bool try_consume(Timestamp_ns now);
    };

    RateLimitConfig config_;
    SlidingWindow global_window_;
    SlidingWindow order_window_;
    SlidingWindow cancel_window_;
    SlidingWindow heartbeat_window_;

    SlidingWindow& window_for(ExecIntentType type);
    const SlidingWindow& window_for(ExecIntentType type) const;

    // Backoff state
    int consecutive_429s_ = 0;
    bool exchange_unavailable_ = false;
    Timestamp_ns backoff_until_ = 0;
    int64_t current_backoff_ms_ = 0;

    // 425 matching-engine-restart state (separate from 429 ladder)
    static constexpr int64_t k425BackoffBaseMs = 1500;   // midpoint of 1-2s per docs
    static constexpr int k425MaxConsecutive = 10;         // fatal after 10 per docs
    int consecutive_425s_ = 0;
    int64_t current_425_backoff_ms_ = k425BackoffBaseMs;

    // RTT tracking (simple exponential moving average)
    int64_t rtt_ema_ns_ = 0;
    int64_t rtt_baseline_ns_ = 0;
    int64_t rtt_count_ = 0;

    // Metrics
    int64_t throttled_count_ = 0;
    int64_t backoff_count_ = 0;
};

}  // namespace lt
