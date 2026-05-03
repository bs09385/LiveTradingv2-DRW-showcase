#pragma once

#include <cstdint>

#include "events/scheduler_events.h"

namespace lt {

// ---------------------------------------------------------------------------
// RiskGateStub: placeholder for real risk checks (future milestones).
//
// API shape matches future use: evaluate(intent) -> allow/deny.
// For M2: always allows, records stats.
//
// Future milestones will add: max position, max notional, max loss, etc.
//
// T2-owned: no thread safety needed. Synchronous and cheap.
// ---------------------------------------------------------------------------
class RiskGateStub {
public:
    RiskDecision evaluate(const ExecutionIntent& intent);

    int64_t check_count() const { return check_count_; }
    int64_t allow_count() const { return allow_count_; }
    int64_t deny_count() const { return deny_count_; }

private:
    int64_t check_count_ = 0;
    int64_t allow_count_ = 0;
    int64_t deny_count_ = 0;
};

}  // namespace lt
