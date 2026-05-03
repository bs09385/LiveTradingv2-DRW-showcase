#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include "common/types.h"

namespace lt {

struct HeartbeatConfig {
    int64_t interval_ms = 5000;
    int max_consecutive_failures = 3;
    bool cancel_all_on_failure = false;
};

class HeartbeatManager {
public:
    explicit HeartbeatManager(const HeartbeatConfig& config);

    // Should we send a heartbeat now?
    bool is_due(Timestamp_ns now) const;

    // Record successful heartbeat
    void on_success(std::string_view heartbeat_id, Timestamp_ns now);

    // Record failed heartbeat
    void on_failure(Timestamp_ns now);

    // Should we cancel all orders due to heartbeat failure?
    bool should_cancel_all() const;

    // Has the heartbeat failure threshold been reached?
    // Used by gateway to drive GATEWAY_DEGRADED signaling.
    bool is_failed() const {
        return consecutive_failures_ >= config_.max_consecutive_failures;
    }

    // Get current heartbeat ID for chaining (empty if first)
    std::string current_heartbeat_id() const { return current_id_; }

    // Metrics
    int64_t success_count() const { return success_count_; }
    int64_t failure_count() const { return failure_count_; }
    int consecutive_failures() const { return consecutive_failures_; }

private:
    HeartbeatConfig config_;
    Timestamp_ns last_sent_ts_ = 0;
    Timestamp_ns last_success_ts_ = 0;
    std::string current_id_;
    int consecutive_failures_ = 0;
    int64_t success_count_ = 0;
    int64_t failure_count_ = 0;
};

}  // namespace lt
