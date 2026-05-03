#include "exec/rate_limiter.h"

#include <algorithm>
#include <random>

namespace lt {

// --- SlidingWindow ---

void RateLimiter::SlidingWindow::slide(Timestamp_ns now) {
    if (last_slide_ts == 0) {
        last_slide_ts = now;
        return;
    }

    int64_t now_ms = now / 1000000;
    int64_t last_ms = last_slide_ts / 1000000;
    int64_t elapsed_ms = now_ms - last_ms;

    if (elapsed_ms <= 0) return;

    int buckets_to_advance = static_cast<int>(elapsed_ms / kBucketMs);
    if (buckets_to_advance <= 0) return;

    if (buckets_to_advance >= kNumBuckets) {
        // Clear all buckets
        buckets.fill(0);
        current_bucket = 0;
    } else {
        // Clear expired buckets
        for (int i = 0; i < buckets_to_advance; ++i) {
            current_bucket = (current_bucket + 1) % kNumBuckets;
            buckets[current_bucket] = 0;
        }
    }
    last_slide_ts = now;
}

int RateLimiter::SlidingWindow::total() const {
    int sum = 0;
    for (int b : buckets) sum += b;
    return sum;
}

bool RateLimiter::SlidingWindow::try_consume(Timestamp_ns now) {
    slide(now);
    if (total() >= limit_per_10s) return false;
    buckets[current_bucket]++;
    return true;
}

// --- RateLimiter ---

RateLimiter::RateLimiter(const RateLimitConfig& config)
    : config_(config) {
    global_window_.limit_per_10s = config_.global_tokens_per_10s;
    order_window_.limit_per_10s = config_.order_tokens_per_10s;
    cancel_window_.limit_per_10s = config_.cancel_tokens_per_10s;
    heartbeat_window_.limit_per_10s = config_.heartbeat_tokens_per_10s;
    current_backoff_ms_ = config_.backoff_base_ms;
}

RateLimiter::SlidingWindow& RateLimiter::window_for(ExecIntentType type) {
    switch (type) {
        case ExecIntentType::PLACE_ORDER:
        case ExecIntentType::REPLACE_ORDER:
            return order_window_;
        case ExecIntentType::CANCEL_ORDER:
        case ExecIntentType::CANCEL_ALL:
            return cancel_window_;
        case ExecIntentType::HEARTBEAT:
            return heartbeat_window_;
    }
    return order_window_;
}

const RateLimiter::SlidingWindow& RateLimiter::window_for(ExecIntentType type) const {
    switch (type) {
        case ExecIntentType::PLACE_ORDER:
        case ExecIntentType::REPLACE_ORDER:
            return order_window_;
        case ExecIntentType::CANCEL_ORDER:
        case ExecIntentType::CANCEL_ALL:
            return cancel_window_;
        case ExecIntentType::HEARTBEAT:
            return heartbeat_window_;
    }
    return order_window_;
}

bool RateLimiter::try_acquire(ExecIntentType type, Timestamp_ns now) {
    // Check backoff
    if (now < backoff_until_) {
        ++throttled_count_;
        return false;
    }

    // Exchange unavailable mode: only allow cancels and heartbeats
    if (exchange_unavailable_ &&
        type != ExecIntentType::CANCEL_ORDER &&
        type != ExecIntentType::CANCEL_ALL &&
        type != ExecIntentType::HEARTBEAT) {
        ++throttled_count_;
        return false;
    }

    // Check global limit
    if (!global_window_.try_consume(now)) {
        ++throttled_count_;
        return false;
    }

    // Check per-type limit
    if (!window_for(type).try_consume(now)) {
        ++throttled_count_;
        return false;
    }

    return true;
}

Timestamp_ns RateLimiter::next_allowed(ExecIntentType type, Timestamp_ns now) const {
    if (now < backoff_until_) return backoff_until_;
    (void)type;
    // Conservative: next bucket boundary
    return now + kBucketMs * 1000000LL;
}

bool RateLimiter::is_degraded() const {
    bool response_degraded = consecutive_429s_ >= config_.max_consecutive_429s;

    // RTT pressure signal: require enough samples and sustained 3x baseline latency.
    constexpr int64_t kMinRttSamples = 20;
    constexpr int64_t kRttDegradeMultiplier = 3;
    bool latency_degraded = false;
    if (rtt_count_ >= kMinRttSamples && rtt_baseline_ns_ > 0) {
        latency_degraded = rtt_ema_ns_ > (rtt_baseline_ns_ * kRttDegradeMultiplier);
    }

    return response_degraded || latency_degraded;
}

// Add jitter: +/- 25% of the backoff value
static int64_t apply_jitter(int64_t base_ms) {
    static thread_local std::mt19937 rng(std::random_device{}());
    int64_t jitter_range = base_ms / 4;  // 25%
    if (jitter_range <= 0) return base_ms;
    std::uniform_int_distribution<int64_t> dist(-jitter_range, jitter_range);
    return std::max(int64_t(1), base_ms + dist(rng));
}

void RateLimiter::record_response(int http_status, Timestamp_ns now) {
    if (http_status == 429) {
        ++consecutive_429s_;
        ++backoff_count_;
        // Exponential backoff with jitter
        int64_t jittered_ms = apply_jitter(current_backoff_ms_);
        backoff_until_ = now + jittered_ms * 1000000LL;
        current_backoff_ms_ = std::min(current_backoff_ms_ * 2, config_.backoff_max_ms);
    } else if (http_status == 425) {
        // Matching engine restarting — separate backoff ladder (1.5s base, 30s cap)
        ++consecutive_425s_;
        ++backoff_count_;
        int64_t jittered_ms = apply_jitter(current_425_backoff_ms_);
        backoff_until_ = std::max(backoff_until_, now + jittered_ms * static_cast<Timestamp_ns>(1000000));
        current_425_backoff_ms_ = std::min(current_425_backoff_ms_ * 2, config_.backoff_max_ms);
    } else if (http_status == 503) {
        exchange_unavailable_ = true;
        ++backoff_count_;
        int64_t jittered_ms = apply_jitter(config_.backoff_base_ms);
        backoff_until_ = now + jittered_ms * 1000000LL;
    } else if (http_status >= 200 && http_status < 300) {
        // Success: reset consecutive 429 count, 425 state, exit exchange-unavailable
        consecutive_429s_ = 0;
        exchange_unavailable_ = false;
        current_backoff_ms_ = config_.backoff_base_ms;
        consecutive_425s_ = 0;
        current_425_backoff_ms_ = k425BackoffBaseMs;
    }
}

void RateLimiter::record_rtt(int64_t rtt_ns) {
    ++rtt_count_;
    if (rtt_count_ <= 10) {
        // Build baseline from first 10 samples
        rtt_baseline_ns_ = ((rtt_baseline_ns_ * (rtt_count_ - 1)) + rtt_ns) / rtt_count_;
        rtt_ema_ns_ = rtt_baseline_ns_;
    } else {
        // EMA with alpha = 0.1
        rtt_ema_ns_ = (rtt_ema_ns_ * 9 + rtt_ns) / 10;
    }
}

void RateLimiter::reset_backoff() {
    consecutive_429s_ = 0;
    exchange_unavailable_ = false;
    backoff_until_ = 0;
    current_backoff_ms_ = config_.backoff_base_ms;
    consecutive_425s_ = 0;
    current_425_backoff_ms_ = k425BackoffBaseMs;
}

}  // namespace lt
