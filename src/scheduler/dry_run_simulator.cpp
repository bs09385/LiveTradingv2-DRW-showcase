#include "scheduler/dry_run_simulator.h"

#include <cstdio>
#include <cstring>

#include "exec/exec_feedback.h"

namespace lt {

void DryRunSimulator::on_intent(const ExecutionIntent& intent) {
    if (!enabled_) return;
    if (count_ >= kMaxBuffered) return;  // drop on overflow

    bool is_place = (intent.action == IntentAction::WOULD_PLACE_BID ||
                     intent.action == IntentAction::WOULD_PLACE_ASK);
    bool is_cancel = (intent.action == IntentAction::WOULD_CANCEL_BID ||
                      intent.action == IntentAction::WOULD_CANCEL_ASK);

    if (!is_place && !is_cancel) return;  // CANCEL_ALL generates nothing

    SchedulerEvent ev{};
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_accepted = true;
    ev.client_order_id = intent.client_order_id;
    ev.asset_id = intent.asset_id;
    ev.user_price = intent.price;
    ev.user_original_size = intent.qty;

    if (is_place) {
        ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::ORDER_ACCEPTED);
        // Generate synthetic exchange order ID
        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "sim-%u", seq_++);
        ev.order_id = OrderId(id_buf);
    } else {
        ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::CANCEL_CONFIRMED);
        ev.order_id = intent.exchange_order_id;
    }

    buffer_[tail_] = ev;
    tail_ = (tail_ + 1) % kMaxBuffered;
    ++count_;
}

bool DryRunSimulator::pop(SchedulerEvent& out) {
    if (count_ == 0) return false;
    out = buffer_[head_];
    head_ = (head_ + 1) % kMaxBuffered;
    --count_;
    return true;
}

void DryRunSimulator::reset() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

}  // namespace lt
