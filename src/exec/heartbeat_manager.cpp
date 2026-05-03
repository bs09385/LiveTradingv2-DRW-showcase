#include "exec/heartbeat_manager.h"

namespace lt {

HeartbeatManager::HeartbeatManager(const HeartbeatConfig& config)
    : config_(config) {}

bool HeartbeatManager::is_due(Timestamp_ns now) const {
    if (config_.interval_ms <= 0) return false;
    if (last_sent_ts_ == 0) return true;  // never sent

    int64_t elapsed_ms = (now - last_sent_ts_) / 1000000;
    return elapsed_ms >= config_.interval_ms;
}

void HeartbeatManager::on_success(std::string_view heartbeat_id, Timestamp_ns now) {
    current_id_ = std::string(heartbeat_id);
    last_sent_ts_ = now;
    last_success_ts_ = now;
    consecutive_failures_ = 0;
    ++success_count_;
}

void HeartbeatManager::on_failure(Timestamp_ns now) {
    last_sent_ts_ = now;
    ++consecutive_failures_;
    ++failure_count_;
}

bool HeartbeatManager::should_cancel_all() const {
    return config_.cancel_all_on_failure &&
           consecutive_failures_ >= config_.max_consecutive_failures;
}

}  // namespace lt
