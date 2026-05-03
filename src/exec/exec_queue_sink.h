#pragma once

#include "scheduler/exec_sink.h"
#include "exec/exec_intent.h"
#include "queue/spsc_queue.h"
#include "common/clock.h"
#include "logger/metrics.h"

namespace lt {

// Real execution sink that converts M2 ExecutionIntents to ExecIntents
// and pushes them to the T2->T3 SPSC queue.
class ExecQueueSink : public ExecSink {
public:
    explicit ExecQueueSink(SpscQueue<ExecIntent>& queue, Metrics* metrics = nullptr)
        : queue_(queue), metrics_(metrics) {}

    SinkResult accept(const ExecutionIntent& intent) override {
        ExecIntent exec;
        exec.asset_id = intent.asset_id;
        exec.intent_id = intent.intent_id;
        exec.created_ts = intent.created_ts;
        exec.recv_ts = intent.recv_ts;  // M7: propagate original receive timestamp
        exec.price = intent.price;
        exec.size = intent.qty;
        exec.market_id = intent.market_id;
        exec.exchange_order_id = intent.exchange_order_id;
        exec.client_order_id = intent.client_order_id;
        exec.neg_risk = intent.neg_risk;
        exec.fee_rate_bps = intent.fee_rate_bps;
        exec.order_type = intent.order_type;

        // Map M2 intent actions to M4 intent types
        switch (intent.action) {
            case IntentAction::WOULD_PLACE_BID:
                exec.type = ExecIntentType::PLACE_ORDER;
                exec.side = Side::BID;
                break;
            case IntentAction::WOULD_PLACE_ASK:
                exec.type = ExecIntentType::PLACE_ORDER;
                exec.side = Side::ASK;
                break;
            case IntentAction::WOULD_CANCEL_BID:
                exec.type = ExecIntentType::CANCEL_ORDER;
                exec.side = Side::BID;
                break;
            case IntentAction::WOULD_CANCEL_ASK:
                exec.type = ExecIntentType::CANCEL_ORDER;
                exec.side = Side::ASK;
                break;
            case IntentAction::WOULD_CANCEL_ALL:
                exec.type = ExecIntentType::CANCEL_ALL;
                break;
        }

        // M7: Record strategy-to-enqueue latency
        if (intent.created_ts > 0 && metrics_) {
            auto now = SteadyClock::now();
            metrics_->record_latency(MetricId::STRAT_TO_EXEC_ENQUEUE_NS,
                                     MetricId::STRAT_TO_EXEC_ENQUEUE_COUNT,
                                     now - intent.created_ts);
        }

        if (queue_.try_push(exec)) {
            ++intent_count_;
            if (metrics_) metrics_->inc(MetricId::EXEC_INTENT_QUEUE_PUSHES);
            return SinkResult::ACCEPTED;
        }
        ++overflow_count_;
        if (metrics_) metrics_->inc(MetricId::EXEC_INTENT_QUEUE_OVERFLOW);
        return SinkResult::OVERFLOW;
    }

    int64_t intent_count() const { return intent_count_; }
    int64_t overflow_count() const { return overflow_count_; }

private:
    SpscQueue<ExecIntent>& queue_;
    Metrics* metrics_ = nullptr;
    int64_t intent_count_ = 0;
    int64_t overflow_count_ = 0;
};

}  // namespace lt
