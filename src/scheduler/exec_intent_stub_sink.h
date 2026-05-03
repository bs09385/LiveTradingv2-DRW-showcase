#pragma once

#include <cstdint>

#include "events/scheduler_events.h"
#include "queue/spsc_queue.h"
#include "scheduler/exec_sink.h"

namespace lt {

// ---------------------------------------------------------------------------
// ExecIntentStubSink: accepts simulated intents from strategy/risk pipeline.
//
// Does NOT send REST requests (that's M4).
// Records counts and timing.
// Optionally publishes ExecInternal feedback events to Q_exec_to_strategy
// to test the scheduler priority / feedback loop behavior.
//
// IMPORTANT: exec feedback events must NOT be silently dropped. Overflow
// is surfaced via the return value and overflow_count() so the scheduler
// can log/metric it. In M4, this path is replaced by ExecQueueSink + T3.
//
// T2-owned: called only from T2 scheduler thread.
//
// SPSC safety note: when exec_feedback_loop_enabled is true, T2 is both
// producer and consumer on exec_queue (self-feedback loop). When a real
// T3 execution thread exists (M4), set exec_feedback_loop_enabled=false
// to prevent multi-producer SPSC violation.
// ---------------------------------------------------------------------------
class ExecIntentStubSink : public ExecSink {
public:
    // exec_feedback_queue: if non-null, accepted intents generate stub feedback
    // events back to the scheduler (for testing exec priority path).
    explicit ExecIntentStubSink(SpscQueue<SchedulerEvent>* exec_feedback_queue = nullptr);

    // Accept an intent. Returns the enqueue result.
    SinkResult accept(const ExecutionIntent& intent) override;

    int64_t intent_count() const { return intent_count_; }
    int64_t feedback_count() const { return feedback_count_; }
    int64_t overflow_count() const { return overflow_count_; }

private:
    SpscQueue<SchedulerEvent>* exec_feedback_queue_;
    int64_t intent_count_ = 0;
    int64_t feedback_count_ = 0;
    int64_t overflow_count_ = 0;
    uint32_t next_feedback_seq_ = 0;
};

}  // namespace lt
