#include <doctest/doctest.h>
#include "exec/rate_limiter.h"

TEST_SUITE("RateLimiter") {

TEST_CASE("allows requests within limits") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 50;
    cfg.cancel_tokens_per_10s = 50;
    cfg.heartbeat_tokens_per_10s = 20;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;  // 1 second in ns

    // Should allow several requests
    for (int i = 0; i < 10; ++i) {
        CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now));
    }
}

TEST_CASE("blocks when per-type limit exceeded") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 1000;
    cfg.order_tokens_per_10s = 5;
    cfg.cancel_tokens_per_10s = 1000;
    cfg.heartbeat_tokens_per_10s = 1000;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    // Exhaust order limit
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now));
    }

    // Next order should be blocked
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now));

    // But cancels should still work
    CHECK(limiter.try_acquire(lt::ExecIntentType::CANCEL_ORDER, now));
}

TEST_CASE("blocks when global limit exceeded") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 5;
    cfg.order_tokens_per_10s = 100;
    cfg.cancel_tokens_per_10s = 100;
    cfg.heartbeat_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now));
    }
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::CANCEL_ORDER, now));
}

TEST_CASE("429 response triggers backoff") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.backoff_base_ms = 500;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(429, now);

    // Should be blocked during backoff period
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now + 1000000LL));

    // After backoff period, should be allowed.
    // Jitter adds +/-25% to backoff_base_ms=500, so max backoff is 625ms.
    // Use 700ms to safely exceed worst-case jitter.
    lt::Timestamp_ns after_backoff = now + 700LL * 1000000LL;  // 700ms
    CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, after_backoff));
}

TEST_CASE("503 triggers cancel-only mode") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.cancel_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(503, now);
    CHECK(limiter.is_exchange_unavailable());

    // After backoff, orders should be blocked but cancels allowed
    lt::Timestamp_ns later = now + 1000LL * 1000000LL;
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, later));
    CHECK(limiter.try_acquire(lt::ExecIntentType::CANCEL_ORDER, later));
    CHECK(limiter.try_acquire(lt::ExecIntentType::HEARTBEAT, later));
}

TEST_CASE("successful response resets backoff") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.max_consecutive_429s = 3;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(429, now);
    limiter.record_response(429, now);
    CHECK_FALSE(limiter.is_degraded());

    limiter.record_response(200, now);
    CHECK_FALSE(limiter.is_degraded());
}

TEST_CASE("degraded state after max consecutive 429s") {
    lt::RateLimitConfig cfg;
    cfg.max_consecutive_429s = 3;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(429, now);
    limiter.record_response(429, now);
    CHECK_FALSE(limiter.is_degraded());

    limiter.record_response(429, now);
    CHECK(limiter.is_degraded());
}

TEST_CASE("sustained high RTT marks degraded") {
    lt::RateLimitConfig cfg;
    cfg.max_consecutive_429s = 100;  // isolate latency-based degradation
    lt::RateLimiter limiter(cfg);

    // Establish baseline at 10ms over first 20 samples.
    for (int i = 0; i < 20; ++i) {
        limiter.record_rtt(10LL * 1000000LL);
    }
    CHECK_FALSE(limiter.is_degraded());

    // Push EMA above 3x baseline with sustained 40ms RTT samples.
    for (int i = 0; i < 12; ++i) {
        limiter.record_rtt(40LL * 1000000LL);
    }
    CHECK(limiter.is_degraded());
}

TEST_CASE("throttled count increments on blocked requests") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 2;
    cfg.order_tokens_per_10s = 2;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    CHECK(limiter.throttled_count() == 0);

    limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now);
    limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now);
    CHECK(limiter.throttled_count() == 0);

    limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now);
    CHECK(limiter.throttled_count() == 1);
}

TEST_CASE("reset_backoff clears all backoff state") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.max_consecutive_429s = 2;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(429, now);
    limiter.record_response(429, now);
    limiter.record_response(503, now);

    CHECK(limiter.is_degraded());
    CHECK(limiter.is_exchange_unavailable());

    limiter.reset_backoff();

    CHECK_FALSE(limiter.is_degraded());
    CHECK_FALSE(limiter.is_exchange_unavailable());
}

TEST_CASE("425 triggers backoff (~1.5s initial)") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.backoff_max_ms = 30000;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    limiter.record_response(425, now);

    // Should be blocked during backoff period (1.5s base +/- 25% jitter => 1125-1875ms)
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now + 500LL * 1000000LL));

    // After 2s, should be allowed (safely past worst-case jitter of 1875ms)
    lt::Timestamp_ns after_backoff = now + 2000LL * 1000000LL;
    CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, after_backoff));
}

TEST_CASE("425 does not increment consecutive_429s (independent escalation)") {
    lt::RateLimitConfig cfg;
    cfg.max_consecutive_429s = 2;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    // Send 5 consecutive 425s — should NOT trigger 429-based degraded
    for (int i = 0; i < 5; ++i) {
        limiter.record_response(425, now);
    }
    CHECK_FALSE(limiter.is_degraded());
}

TEST_CASE("10 consecutive 425s triggers is_matching_engine_down") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    for (int i = 0; i < 9; ++i) {
        limiter.record_response(425, now);
        CHECK_FALSE(limiter.is_matching_engine_down());
    }

    limiter.record_response(425, now);
    CHECK(limiter.is_matching_engine_down());
}

TEST_CASE("2xx resets 425 counter and backoff multiplier") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    // Accumulate 8 consecutive 425s
    for (int i = 0; i < 8; ++i) {
        limiter.record_response(425, now);
    }
    CHECK_FALSE(limiter.is_matching_engine_down());

    // 2xx resets everything
    limiter.record_response(200, now + 30000LL * 1000000LL);
    CHECK_FALSE(limiter.is_matching_engine_down());

    // Now 2 more 425s should NOT reach 10
    limiter.record_response(425, now + 60000LL * 1000000LL);
    limiter.record_response(425, now + 90000LL * 1000000LL);
    CHECK_FALSE(limiter.is_matching_engine_down());
}

TEST_CASE("reset_backoff clears 425 state") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    for (int i = 0; i < 10; ++i) {
        limiter.record_response(425, now);
    }
    CHECK(limiter.is_matching_engine_down());

    limiter.reset_backoff();
    CHECK_FALSE(limiter.is_matching_engine_down());
}

TEST_CASE("mixed 425 + 429 have independent escalation") {
    lt::RateLimitConfig cfg;
    cfg.max_consecutive_429s = 3;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    // Interleave 425s and 429s
    limiter.record_response(425, now);
    limiter.record_response(429, now);
    limiter.record_response(425, now);
    limiter.record_response(429, now);
    limiter.record_response(425, now);

    // Only 2 consecutive 429s (< threshold of 3), so not degraded
    CHECK_FALSE(limiter.is_degraded());
    // Only 3 consecutive 425s (< threshold of 10), so matching engine not "down"
    CHECK_FALSE(limiter.is_matching_engine_down());
}

TEST_CASE("425 backoff escalates exponentially and caps at backoff_max_ms") {
    lt::RateLimitConfig cfg;
    cfg.global_tokens_per_10s = 100;
    cfg.order_tokens_per_10s = 100;
    cfg.backoff_max_ms = 30000;

    lt::RateLimiter limiter(cfg);
    lt::Timestamp_ns now = 1000000000LL;

    // First 425: ~1.5s base. Verify initial backoff is in 1.5s range.
    limiter.record_response(425, now);
    // Blocked at 500ms (well within 1125-1875ms jitter range)
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now + 500LL * 1000000LL));
    // Allowed at 2s (past 1875ms worst case)
    CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, now + 2000LL * 1000000LL));

    // Now send more 425s to escalate. Each doubles: 3s, 6s, 12s, 24s, capped at 30s.
    // Send them spaced out so previous backoffs have expired.
    lt::Timestamp_ns t = now + 50000LL * 1000000LL;  // 50s later, all backoffs expired
    for (int i = 0; i < 5; ++i) {
        limiter.record_response(425, t);
    }
    // After 5 more doublings from 3000: 3000->6000->12000->24000->30000(cap)
    // Last backoff is 30s +/- 25% jitter = 22.5s to 37.5s from t
    // Should be blocked at 10s
    CHECK_FALSE(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, t + 10000LL * 1000000LL));
    // Should be allowed at 40s (safely past 37.5s worst case)
    CHECK(limiter.try_acquire(lt::ExecIntentType::PLACE_ORDER, t + 40000LL * 1000000LL));
}

}  // TEST_SUITE
