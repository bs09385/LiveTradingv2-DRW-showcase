#pragma once

#include <cstdint>

#include "events/scheduler_events.h"
#include "scheduler/strategy_state_stub.h"

namespace lt {

// ---------------------------------------------------------------------------
// StrategyStub: placeholder for real strategy logic (future milestones).
//
// Called by StrategyScheduler on eligible triggers.
// Inputs: the triggering event + read-only view of T2-owned state.
// Outputs: zero or more simulated ExecutionIntents.
//
// For M2: deterministic, no real trading actions.
// When emit_intents is true, generates "would_quote" intents for any
// market event with a valid BBO (both bid and ask present).
//
// T2-owned: no thread safety needed.
// ---------------------------------------------------------------------------
class StrategyStub {
public:
    explicit StrategyStub(bool emit_intents = false);

    // Evaluate an event and optionally produce simulated intents
    IntentBatch evaluate(const SchedulerEvent& event, const StrategyStateStub& state);

    int64_t invocation_count() const { return invocation_count_; }

private:
    bool emit_intents_;
    int64_t invocation_count_ = 0;
    uint32_t next_intent_id_ = 0;
};

}  // namespace lt
