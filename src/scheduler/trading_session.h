#pragma once

#include <cstdint>
#include <algorithm>

namespace lt {

enum class SessionState : uint8_t {
    IDLE    = 0,  // DRY_RUN, no active session
    PENDING = 1,  // Waiting for next market boundary to go LIVE
    ACTIVE  = 2,  // LIVE trading in progress
};

struct TradingSession {
    SessionState state = SessionState::IDLE;
    int64_t end_time_s = 0;           // 0 = indefinite, else UTC epoch seconds
    int64_t effective_end_s = 0;      // snapped to last complete market boundary
    int64_t started_at_ms = 0;        // wall clock when session went ACTIVE
    int32_t markets_entered = 0;      // count of SLOT_PROMOTED while ACTIVE

    bool is_active() const { return state == SessionState::ACTIVE; }
    bool is_pending() const { return state == SessionState::PENDING; }
    bool is_idle() const { return state == SessionState::IDLE; }

    // Check if a market with this window_end exceeds the session boundary
    bool exceeds_end(int64_t window_end_s) const {
        return effective_end_s > 0 && window_end_s > effective_end_s;
    }

    void reset() { *this = TradingSession{}; }
};

// Compute effective end time: snap user's end_time down to last complete market
// boundary across both 5M (300s) and 15M (900s) timeframes.
// Returns 0 if end_time_s is 0 (indefinite).
inline int64_t compute_effective_end(int64_t end_time_s) {
    if (end_time_s <= 0) return 0;

    constexpr int64_t kPeriods[] = {300, 900};  // 5M, 15M
    int64_t effective = 0;

    for (int64_t period : kPeriods) {
        int64_t window_start = (end_time_s / period) * period;
        int64_t window_end = window_start + period;
        int64_t tf_last_end;
        if (window_end <= end_time_s) {
            tf_last_end = window_end;      // end_time at or past boundary
        } else {
            tf_last_end = window_start;    // previous market was the last complete one
        }
        effective = std::max(effective, tf_last_end);
    }

    return effective;
}

inline const char* session_state_name(SessionState s) {
    switch (s) {
        case SessionState::IDLE:    return "IDLE";
        case SessionState::PENDING: return "PENDING";
        case SessionState::ACTIVE:  return "ACTIVE";
    }
    return "UNKNOWN";
}

}  // namespace lt
