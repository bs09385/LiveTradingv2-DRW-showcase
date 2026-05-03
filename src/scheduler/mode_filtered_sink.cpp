#include "scheduler/mode_filtered_sink.h"
#include "scheduler/dry_run_simulator.h"

#include <cstdio>

namespace lt {

ModeFilteredSink::ModeFilteredSink(ExecSink* inner, Metrics* metrics,
                                   ExecutionMode mode)
    : inner_(inner), metrics_(metrics), mode_(mode) {}

SinkResult ModeFilteredSink::accept(const ExecutionIntent& intent) {
    // CANCEL_ALL always passes through regardless of mode — it's a safety mechanism
    if (intent.action == IntentAction::WOULD_CANCEL_ALL) {
        ++forwarded_;
        return inner_ ? inner_->accept(intent) : SinkResult::NO_QUEUE;
    }

    switch (mode_) {
        case ExecutionMode::DRY_RUN: {
            ++dry_run_logged_;
            if (metrics_) metrics_->inc(MetricId::STRAT_DRY_RUN_LOGGED);
            if (dry_sim_) {
                if (logger_) {
                    char buf[LogEntry::kMaxMsg];
                    const char* action = is_placement(intent.action) ? "PLACE" : "CANCEL";
                    const char* side = (intent.action == IntentAction::WOULD_PLACE_BID ||
                                        intent.action == IntentAction::WOULD_CANCEL_BID) ? "BID" : "ASK";
                    std::snprintf(buf, sizeof(buf),
                        "[DRY-SIM] %s %s %.*s price=%d qty=%lld cid=%.*s",
                        action, side,
                        static_cast<int>(intent.asset_id.len), intent.asset_id.data,
                        intent.price, static_cast<long long>(intent.qty),
                        static_cast<int>(intent.client_order_id.len), intent.client_order_id.data);
                    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                }
                dry_sim_->on_intent(intent);
                return SinkResult::ACCEPTED;
            }
            return SinkResult::FILTERED;
        }

        case ExecutionMode::LIVE: {
            // Forward everything unconditionally
            ++forwarded_;
            return inner_ ? inner_->accept(intent) : SinkResult::NO_QUEUE;
        }
    }

    // Unreachable, but safe default
    ++mode_blocked_;
    return SinkResult::FILTERED;
}

}  // namespace lt
