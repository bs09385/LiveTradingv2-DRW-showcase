#include "scheduler/exec_intent_stub_sink.h"

#include "common/clock.h"

namespace lt {

ExecIntentStubSink::ExecIntentStubSink(SpscQueue<SchedulerEvent>* exec_feedback_queue)
    : exec_feedback_queue_(exec_feedback_queue) {}

SinkResult ExecIntentStubSink::accept(const ExecutionIntent& intent) {
    ++intent_count_;

    if (!exec_feedback_queue_) {
        return SinkResult::NO_QUEUE;
    }

    SchedulerEvent feedback;
    feedback.source = EventSource::EXEC_INTERNAL;
    feedback.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    feedback.asset_id = intent.asset_id;
    feedback.recv_ts = SteadyClock::now();
    feedback.seq = next_feedback_seq_++;
    feedback.intent_ref_id = intent.intent_id;
    feedback.exec_accepted = true;

    if (exec_feedback_queue_->try_push(feedback)) {
        ++feedback_count_;
        return SinkResult::ACCEPTED;
    }

    // Overflow: feedback was NOT enqueued. Caller must surface this.
    ++overflow_count_;
    return SinkResult::OVERFLOW;
}

}  // namespace lt
