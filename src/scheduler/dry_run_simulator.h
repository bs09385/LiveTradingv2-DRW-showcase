#pragma once

#include <cstdint>
#include <cstdio>

#include "events/scheduler_events.h"

namespace lt {

// T2-owned synthetic feedback generator for DRY_RUN mode.
// Converts WOULD_PLACE/CANCEL intents into fake ORDER_ACCEPTED/CANCEL_CONFIRMED
// events so the strategy's WorkingQuote lifecycle exercises correctly.
//
// Fixed-size ring buffer (no heap allocation). Drained every scheduler cycle.
class DryRunSimulator {
public:
    static constexpr int kMaxBuffered = 64;

    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }

    // Buffer synthetic feedback for an intent. Only buffers when enabled.
    void on_intent(const ExecutionIntent& intent);

    // Drain one event from the buffer. Returns false when empty.
    bool pop(SchedulerEvent& out);

    // Number of buffered events.
    int pending_count() const { return count_; }

    // Clear all buffered events (e.g. on mode change away from DRY_RUN).
    void reset();

private:
    SchedulerEvent buffer_[kMaxBuffered];
    int head_ = 0;
    int tail_ = 0;
    int count_ = 0;
    bool enabled_ = false;
    uint32_t seq_ = 0;
};

}  // namespace lt
