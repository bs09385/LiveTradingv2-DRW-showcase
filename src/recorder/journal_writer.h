#pragma once

#include <chrono>
#include <cstdint>

#include "common/types.h"
#include "events/scheduler_events.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "recorder/journal_types.h"
#include "scheduler/execution_mode.h"

namespace lt {

// ---------------------------------------------------------------------------
// JournalWriter: T2-owned helper that builds JournalRecord structs and pushes
// them to the journal SPSC queue consumed by DataRecorder (T_rec).
//
// All record_*() methods no-op when the queue pointer is nullptr or when the
// record's level exceeds the configured journal_level.
//
// Thread ownership: T2 (single producer). No thread safety needed.
// ---------------------------------------------------------------------------
class JournalWriter {
public:
    JournalWriter() = default;

    explicit JournalWriter(SpscQueue<JournalRecord>* queue, Metrics* metrics,
                           int level = kJournalLevelFull, bool is_dry_run = false)
        : queue_(queue), metrics_(metrics), level_(level), is_dry_run_(is_dry_run) {}

    void set_queue(SpscQueue<JournalRecord>* q) { queue_ = q; }
    void set_metrics(Metrics* m) { metrics_ = m; }
    void set_level(int level) { level_ = level; }
    void set_dry_run(bool dry_run) { is_dry_run_ = dry_run; }
    bool enabled() const { return queue_ != nullptr; }

    // --- Level 1: Strategy/Risk records ---

    void record_strategy_eval(const SchedulerEvent& event,
                              Price_t bbo_bid, Price_t bbo_ask,
                              Price_t desired_bid, Price_t desired_ask,
                              Qty_t qty, int intent_count) {
        if (!queue_ || level_ < kJournalLevelFull) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::STRATEGY_EVAL, event.recv_ts);
        rec.asset_id = JournalId(event.asset_id);

        auto& p = rec.payload.strategy_eval;
        p.bbo_bid = bbo_bid;
        p.bbo_ask = bbo_ask;
        p.desired_bid = desired_bid;
        p.desired_ask = desired_ask;
        p.qty = qty;
        p.intent_count = static_cast<uint8_t>(intent_count);
        p.trigger_source = static_cast<uint8_t>(event.source);
        p.trigger_kind = static_cast<uint8_t>(event.kind);

        push(rec);
    }

    void record_risk_decision(const ExecutionIntent& intent,
                              RiskDecision decision, RiskDenyReason reason,
                              Qty_t position, int64_t notional) {
        if (!queue_ || level_ < kJournalLevelFull) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::RISK_DECISION, intent.recv_ts);
        rec.asset_id = JournalId(intent.asset_id);

        auto& p = rec.payload.risk_decision;
        p.action = static_cast<uint8_t>(intent.action);
        p.decision = static_cast<uint8_t>(decision);
        p.deny_reason = static_cast<uint8_t>(reason);
        p.side = (intent.action == IntentAction::WOULD_PLACE_BID ||
                  intent.action == IntentAction::WOULD_CANCEL_BID)
                     ? static_cast<uint8_t>(Side::BID)
                     : static_cast<uint8_t>(Side::ASK);
        p.price = intent.price;
        p.qty = intent.qty;
        p.position = position;
        p.notional = notional;

        push(rec);
    }

    // --- Level 0: Lifecycle records ---

    void record_order_sent(const ExecutionIntent& intent,
                           Price_t bbo_bid, Price_t bbo_ask) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::ORDER_SENT, intent.recv_ts);
        rec.asset_id = JournalId(intent.asset_id);

        auto& p = rec.payload.order_sent;
        p.action = static_cast<uint8_t>(intent.action);
        p.side = (intent.action == IntentAction::WOULD_PLACE_BID ||
                  intent.action == IntentAction::WOULD_CANCEL_BID)
                     ? static_cast<uint8_t>(Side::BID)
                     : static_cast<uint8_t>(Side::ASK);
        p.level = intent.level;
        p.price = intent.price;
        p.qty = intent.qty;
        p.bbo_bid = bbo_bid;
        p.bbo_ask = bbo_ask;
        p.client_order_id = JournalId(intent.client_order_id);

        push(rec);
    }

    void record_exec_feedback(const SchedulerEvent& event) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::EXEC_FEEDBACK, event.recv_ts);
        rec.asset_id = JournalId(event.asset_id);

        auto& p = rec.payload.exec_feedback;
        p.feedback_kind = event.exec_feedback_kind;
        p.http_status = event.exec_http_status;
        // Note: latency_ns left as 0 — ExecFeedback.latency_ns is not propagated
        // through SchedulerEvent. Would require adding a field to plumb it.
        // Prefer exchange order_id, fall back to client_order_id
        p.order_id = event.order_id.len > 0
            ? JournalId(event.order_id)
            : JournalId(event.client_order_id);

        push(rec);
    }

    void record_order_status(const SchedulerEvent& event) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::ORDER_STATUS, event.recv_ts);
        rec.asset_id = JournalId(event.asset_id);

        auto& p = rec.payload.order_status;
        p.order_id = JournalId(event.order_id);
        p.status = event.order_status_raw;
        p.side = static_cast<uint8_t>(event.user_side);
        p.price = event.user_price;
        p.original_size = event.user_original_size;
        p.filled_size = event.user_cumulative_filled;

        push(rec);
    }

    void record_fill(const SchedulerEvent& event, Qty_t net_position_after) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::FILL, event.recv_ts);
        rec.asset_id = JournalId(event.asset_id);

        auto& p = rec.payload.fill;
        p.trade_id = JournalId(event.trade_id);
        p.fill_price = event.user_price;
        p.fill_size = event.user_fill_size;
        p.net_position_after = net_position_after;
        p.side = static_cast<uint8_t>(event.user_side);

        push(rec);
    }

    void record_mode_change(ExecutionMode old_mode, ExecutionMode new_mode) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::MODE_CHANGE, 0);

        auto& p = rec.payload.mode_change;
        p.old_mode = static_cast<uint8_t>(old_mode);
        p.new_mode = static_cast<uint8_t>(new_mode);

        push(rec);
    }

    void record_cancel_all(EventSource trigger_source, int working_count) {
        if (!queue_) return;

        JournalRecord rec{};
        fill_header(rec, JournalRecordType::CANCEL_ALL, 0);

        auto& p = rec.payload.cancel_all;
        p.trigger_source = static_cast<uint8_t>(trigger_source);
        p.working_count = working_count;

        push(rec);
    }

private:
    void fill_header(JournalRecord& rec, JournalRecordType type, int64_t recv_ts) {
        rec.wall_clock_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        rec.recv_ts = recv_ts;
        rec.proc_ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        rec.seq = seq_++;
        rec.type = static_cast<uint8_t>(type);
        rec.flags = is_dry_run_ ? kJournalFlagDryRun : 0;
    }

    void push(const JournalRecord& rec) {
        if (queue_->try_push(rec)) {
            if (metrics_) metrics_->inc(MetricId::JOURNAL_RECORDS_PUSHED);
        } else {
            if (metrics_) metrics_->inc(MetricId::JOURNAL_RECORDS_DROPPED);
        }
    }

    SpscQueue<JournalRecord>* queue_ = nullptr;
    Metrics* metrics_ = nullptr;
    int level_ = kJournalLevelFull;
    bool is_dry_run_ = false;
    uint16_t seq_ = 0;
};

}  // namespace lt
