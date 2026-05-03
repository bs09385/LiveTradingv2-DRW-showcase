#pragma once

#include "scheduler/exec_sink.h"
#include "scheduler/execution_mode.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"

namespace lt {

class DryRunSimulator;

// ExecSink wrapper that enforces execution mode policy.
// Strategy always expresses what it wants; mode is a policy layer.
//
// Policy per mode:
//   DRY_RUN:  Log "would send", return ACCEPTED, never forward. Cancels also blocked.
//   LIVE:     Forward everything unconditionally.
//
// Cancels always pass in LIVE mode.
// Runtime set_mode() for control events.
//
// T2-owned: no thread safety needed.
class ModeFilteredSink : public ExecSink {
public:
    ModeFilteredSink(ExecSink* inner, Metrics* metrics,
                     ExecutionMode mode = ExecutionMode::DRY_RUN);

    SinkResult accept(const ExecutionIntent& intent) override;

    void set_mode(ExecutionMode mode) { mode_ = mode; }
    ExecutionMode mode() const { return mode_; }

    void set_dry_run_simulator(DryRunSimulator* sim) { dry_sim_ = sim; }
    void set_logger(AsyncLogger* logger, ProducerHandle handle) {
        logger_ = logger;
        log_handle_ = handle;
    }

    int64_t dry_run_logged() const { return dry_run_logged_; }
    int64_t mode_blocked() const { return mode_blocked_; }
    int64_t forwarded() const { return forwarded_; }

private:
    static bool is_cancel(IntentAction action) {
        return action == IntentAction::WOULD_CANCEL_BID ||
               action == IntentAction::WOULD_CANCEL_ASK;
    }

    static bool is_placement(IntentAction action) {
        return action == IntentAction::WOULD_PLACE_BID ||
               action == IntentAction::WOULD_PLACE_ASK;
    }

    ExecSink* inner_;
    Metrics* metrics_;
    ExecutionMode mode_;
    DryRunSimulator* dry_sim_ = nullptr;
    AsyncLogger* logger_ = nullptr;
    ProducerHandle log_handle_{};

    int64_t dry_run_logged_ = 0;
    int64_t mode_blocked_ = 0;
    int64_t forwarded_ = 0;
};

}  // namespace lt
