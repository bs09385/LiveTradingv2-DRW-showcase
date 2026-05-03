#include "logger/metrics.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace lt {

LatencyPercentiles compute_percentiles(int64_t* buf, int count) {
    LatencyPercentiles result{};
    if (count <= 0) return result;
    result.count = count;

    // Compute average before sorting
    int64_t sum = 0;
    for (int i = 0; i < count; ++i) sum += buf[i];
    result.avg_ns = sum / count;

    std::sort(buf, buf + count);
    result.p50_ns = buf[count / 2];
    result.p95_ns = buf[std::min(count - 1, count * 95 / 100)];
    result.p99_ns = buf[std::min(count - 1, count * 99 / 100)];
    return result;
}

Metrics::Metrics() {
    for (auto& c : counters_) {
        c.value.store(0, std::memory_order_relaxed);
        c.max_ns.store(0, std::memory_order_relaxed);
    }
}

const char* Metrics::metric_name(MetricId id) {
    switch (id) {
        case MetricId::WS_FRAMES_RECEIVED: return "ws.frames_received";
        case MetricId::WS_BYTES_RECEIVED: return "ws.bytes_received";
        case MetricId::WS_RECONNECTS: return "ws.reconnects";
        case MetricId::WS_ERRORS: return "ws.errors";
        case MetricId::PARSE_OK: return "parse.ok";
        case MetricId::PARSE_ERRORS: return "parse.errors";
        case MetricId::PARSE_BOOK: return "parse.book";
        case MetricId::PARSE_PRICE_CHANGE: return "parse.price_change";
        case MetricId::PARSE_BEST_BID_ASK: return "parse.best_bid_ask";
        case MetricId::PARSE_TICK_SIZE_CHANGE: return "parse.tick_size_change";
        case MetricId::PARSE_LAST_TRADE_PRICE: return "parse.last_trade_price";
        case MetricId::PARSE_PONG: return "parse.pong";
        case MetricId::PARSE_UNKNOWN: return "parse.unknown";
        case MetricId::BOOK_SNAPSHOTS: return "book.snapshots";
        case MetricId::BOOK_UPDATES: return "book.updates";
        case MetricId::BOOK_ERRORS: return "book.errors";
        case MetricId::QUEUE_PUSHES: return "queue.pushes";
        case MetricId::QUEUE_POPS: return "queue.pops";
        case MetricId::QUEUE_OVERFLOWS: return "queue.overflows";
        case MetricId::SCHED_EVENTS: return "sched.events";
        case MetricId::PARSE_LATENCY_NS: return "parse.latency_ns";
        case MetricId::PARSE_LATENCY_COUNT: return "parse.latency_count";
        case MetricId::BOOK_LATENCY_NS: return "book.latency_ns";
        case MetricId::BOOK_LATENCY_COUNT: return "book.latency_count";
        case MetricId::E2E_LATENCY_NS: return "e2e.latency_ns";
        case MetricId::E2E_LATENCY_COUNT: return "e2e.latency_count";
        // Scheduler M2
        case MetricId::SCHED_CYCLES: return "sched.cycles";
        case MetricId::SCHED_EVENTS_MARKET: return "sched.events_market";
        case MetricId::SCHED_EVENTS_USER: return "sched.events_user";
        case MetricId::SCHED_EVENTS_EXEC: return "sched.events_exec";
        case MetricId::SCHED_EVENTS_CONTROL: return "sched.events_control";
        case MetricId::SCHED_EMPTY_POLLS: return "sched.empty_polls";
        case MetricId::SCHED_STRATEGY_CALLS: return "sched.strategy_calls";
        case MetricId::SCHED_RISK_CHECKS: return "sched.risk_checks";
        case MetricId::SCHED_INTENTS_PRODUCED: return "sched.intents_produced";
        case MetricId::SCHED_INTENTS_ALLOWED: return "sched.intents_allowed";
        case MetricId::SCHED_QUEUE_OVERFLOWS_M2: return "sched.queue_overflows";
        case MetricId::SCHED_RECV_TO_PROC_NS: return "sched.recv_to_proc_ns";
        case MetricId::SCHED_RECV_TO_PROC_COUNT: return "sched.recv_to_proc_count";
        case MetricId::SCHED_STRAT_LATENCY_NS: return "sched.strat_latency_ns";
        case MetricId::SCHED_STRAT_LATENCY_COUNT: return "sched.strat_latency_count";
        case MetricId::SCHED_LOOP_NS: return "sched.loop_ns";
        case MetricId::SCHED_LOOP_COUNT: return "sched.loop_count";
        case MetricId::SCHED_MAX_BACKLOG: return "sched.max_backlog";
        case MetricId::SCHED_QUOTE_CONVERSIONS: return "sched.quote_conversions";
        // User WS M3
        case MetricId::USER_WS_MESSAGES: return "user_ws.messages";
        case MetricId::USER_WS_PARSE_OK: return "user_ws.parse_ok";
        case MetricId::USER_WS_PARSE_FAIL: return "user_ws.parse_fail";
        case MetricId::USER_WS_ORDER_UPDATES: return "user_ws.order_updates";
        case MetricId::USER_WS_TRADE_UPDATES: return "user_ws.trade_updates";
        case MetricId::USER_WS_FILLS: return "user_ws.fills";
        case MetricId::USER_WS_POSITION_DELTAS: return "user_ws.position_deltas";
        case MetricId::USER_WS_RECONNECTS: return "user_ws.reconnects";
        case MetricId::USER_WS_STALE_DETECTED: return "user_ws.stale_detected";
        case MetricId::USER_WS_DUPLICATES: return "user_ws.duplicates";
        case MetricId::USER_WS_QUEUE_OVERFLOW: return "user_ws.queue_overflow";
        // Execution Gateway M4
        case MetricId::EXEC_REST_REQUESTS_ORDER: return "exec.rest_requests_order";
        case MetricId::EXEC_REST_REQUESTS_CANCEL: return "exec.rest_requests_cancel";
        case MetricId::EXEC_REST_REQUESTS_HEARTBEAT: return "exec.rest_requests_heartbeat";
        case MetricId::EXEC_REST_REQUESTS_BATCH: return "exec.rest_requests_batch";
        case MetricId::EXEC_BATCH_ORDERS_SENT: return "exec.batch_orders_sent";
        case MetricId::EXEC_REST_SUCCESS: return "exec.rest_success";
        case MetricId::EXEC_REST_ERRORS: return "exec.rest_errors";
        case MetricId::EXEC_HTTP_429: return "exec.http_429";
        case MetricId::EXEC_HTTP_503: return "exec.http_503";
        case MetricId::EXEC_HTTP_5XX: return "exec.http_5xx";
        case MetricId::EXEC_HTTP_425: return "exec.http_425";
        case MetricId::EXEC_HTTP_4XX: return "exec.http_4xx";
        case MetricId::EXEC_RTT_NS: return "exec.rtt_ns";
        case MetricId::EXEC_RTT_COUNT: return "exec.rtt_count";
        case MetricId::EXEC_RATE_THROTTLED: return "exec.rate_throttled";
        case MetricId::EXEC_HEARTBEAT_OK: return "exec.heartbeat_ok";
        case MetricId::EXEC_HEARTBEAT_FAIL: return "exec.heartbeat_fail";
        case MetricId::EXEC_GATEWAY_DEGRADED_COUNT: return "exec.gateway_degraded";
        case MetricId::EXEC_INTENT_QUEUE_PUSHES: return "exec.intent_queue_pushes";
        case MetricId::EXEC_INTENT_QUEUE_OVERFLOW: return "exec.intent_queue_overflow";
        case MetricId::EXEC_LOCAL_INVENTORY_REJECTS: return "exec.local_inventory_rejects";
        case MetricId::EXEC_TIMEOUT_COUNT: return "exec.timeout_count";
        case MetricId::EXEC_AMBIGUOUS_COUNT: return "exec.ambiguous_count";
        case MetricId::EXEC_CORRELATION_MATCHED: return "exec.correlation_matched";
        // Strategy / Risk M5
        case MetricId::STRAT_DESIRED_QUOTES: return "strat.desired_quotes";
        case MetricId::STRAT_ACTUAL_WORKING: return "strat.actual_working";
        case MetricId::STRAT_REPLACES: return "strat.replaces";
        case MetricId::STRAT_CHURN_THROTTLED: return "strat.churn_throttled";
        case MetricId::STRAT_DRY_RUN_LOGGED: return "strat.dry_run_logged";
        case MetricId::STRAT_MODE_BLOCKED: return "strat.mode_blocked";
        case MetricId::STRAT_RISK_DENIED: return "strat.risk_denied";
        case MetricId::STRAT_CANCEL_ALL_TRIGGERED: return "strat.cancel_all_triggered";
        case MetricId::STRAT_INVENTORY_REJECTIONS: return "strat.inventory_rejections";
        case MetricId::STRAT_DEGRADED_CYCLES: return "strat.degraded_cycles";
        case MetricId::STRAT_INTENT_LATENCY_NS: return "strat.intent_latency_ns";
        case MetricId::STRAT_INTENT_LATENCY_COUNT: return "strat.intent_latency_count";
        case MetricId::STRAT_WORKING_ORDERS: return "strat.working_orders";
        case MetricId::STRAT_AMBIGUOUS_ORDERS: return "strat.ambiguous_orders";
        case MetricId::STRAT_TRACKER_DROPS: return "strat.tracker_drops";
        case MetricId::STRAT_NO_TRADE_ZONE: return "strat.no_trade_zone";
        case MetricId::STRAT_TIER1_ACTIVATIONS: return "strat.tier1_activations";
        case MetricId::STRAT_TIER2_FAK_FIRES: return "strat.tier2_fak_fires";
        case MetricId::STRAT_CASCADE_BREAKER_FIRES: return "strat.cascade_breaker_fires";
        case MetricId::STRAT_TIME_GUARD_KILLS: return "strat.time_guard_kills";
        // UI Bridge M6
        case MetricId::UI_SNAPSHOTS_SENT: return "ui.snapshots_sent";
        case MetricId::UI_SNAPSHOTS_DROPPED: return "ui.snapshots_dropped";
        case MetricId::UI_COMMANDS_RECEIVED: return "ui.commands_received";
        case MetricId::UI_COMMANDS_INVALID: return "ui.commands_invalid";
        case MetricId::UI_WS_CONNECTED: return "ui.ws_connected";
        case MetricId::UI_WS_DISCONNECTED: return "ui.ws_disconnected";
        case MetricId::UI_BOOK_PUSHES: return "ui.book_pushes";
        case MetricId::UI_COMMANDS_DROPPED: return "ui.commands_dropped";
        case MetricId::UI_BOOK_DROPS: return "ui.book_drops";
        case MetricId::UI_STATE_DROPS: return "ui.state_drops";
        // M7 Latency
        case MetricId::STRAT_TO_EXEC_ENQUEUE_NS: return "m7.strat_to_exec_enqueue_ns";
        case MetricId::STRAT_TO_EXEC_ENQUEUE_COUNT: return "m7.strat_to_exec_enqueue_count";
        case MetricId::EXEC_ENQUEUE_TO_SEND_NS: return "m7.exec_enqueue_to_send_ns";
        case MetricId::EXEC_ENQUEUE_TO_SEND_COUNT: return "m7.exec_enqueue_to_send_count";
        case MetricId::FULL_PIPELINE_NS: return "m7.full_pipeline_ns";
        case MetricId::FULL_PIPELINE_COUNT: return "m7.full_pipeline_count";
        // M7 Queue depth
        case MetricId::Q_MARKET_DEPTH: return "m7.q_market_depth";
        case MetricId::Q_USER_DEPTH: return "m7.q_user_depth";
        case MetricId::Q_EXEC_DEPTH: return "m7.q_exec_depth";
        case MetricId::Q_CONTROL_DEPTH: return "m7.q_control_depth";
        case MetricId::Q_STRATEGY_TO_EXEC_DEPTH: return "m7.q_strategy_to_exec_depth";
        case MetricId::Q_BINANCE_MD_DEPTH: return "m7.q_binance_md_depth";
        case MetricId::Q_MARKET_HIGH_WATER: return "m7.q_market_high_water";
        case MetricId::Q_USER_HIGH_WATER: return "m7.q_user_high_water";
        case MetricId::Q_EXEC_HIGH_WATER: return "m7.q_exec_high_water";
        case MetricId::Q_BINANCE_MD_HIGH_WATER: return "m7.q_binance_md_high_water";
        // Market WS hardening
        case MetricId::BBO_DIVERGENCE: return "ws.bbo_divergence";
        case MetricId::NEW_MARKETS_RECEIVED: return "ws.new_markets_received";
        case MetricId::MARKETS_RESOLVED: return "ws.markets_resolved";
        // User WS hardening
        case MetricId::USER_WS_AUTH_FAILURES: return "user_ws.auth_failures";
        case MetricId::USER_WS_SERVER_ERRORS: return "user_ws.server_errors";
        case MetricId::USER_WS_UNKNOWN_EVENT_TYPES: return "user_ws.unknown_event_types";
        case MetricId::USER_WS_FIELD_TRUNCATIONS: return "user_ws.field_truncations";
        case MetricId::USER_WS_CLOSE_FRAMES: return "user_ws.close_frames";
        // Book delta queue
        case MetricId::BOOK_DELTA_PUSHES: return "book_delta.pushes";
        case MetricId::BOOK_DELTA_DROPS: return "book_delta.drops";
        case MetricId::BOOK_DELTA_SNAPSHOT_CHUNKS: return "book_delta.snapshot_chunks";
        // RTDS
        case MetricId::RTDS_MESSAGES: return "rtds.messages";
        case MetricId::RTDS_PARSE_OK: return "rtds.parse_ok";
        case MetricId::RTDS_PARSE_FAIL: return "rtds.parse_fail";
        case MetricId::RTDS_PONG: return "rtds.pong";
        case MetricId::RTDS_UNKNOWN: return "rtds.unknown";
        case MetricId::RTDS_RECONNECTS: return "rtds.reconnects";
        case MetricId::RTDS_QUEUE_PUSHES: return "rtds.queue_pushes";
        case MetricId::RTDS_QUEUE_DROPS: return "rtds.queue_drops";
        case MetricId::RTDS_RECORD_PUSHES: return "rtds.record_pushes";
        case MetricId::RTDS_RECORD_DROPS: return "rtds.record_drops";
        // Binance market-data
        case MetricId::BINANCE_MD_MESSAGES: return "binance_md.messages";
        case MetricId::BINANCE_MD_PARSE_OK: return "binance_md.parse_ok";
        case MetricId::BINANCE_MD_PARSE_FAIL: return "binance_md.parse_fail";
        case MetricId::BINANCE_MD_UNKNOWN: return "binance_md.unknown";
        case MetricId::BINANCE_MD_RECONNECTS: return "binance_md.reconnects";
        case MetricId::BINANCE_MD_QUEUE_PUSHES: return "binance_md.queue_pushes";
        case MetricId::BINANCE_MD_QUEUE_DROPS: return "binance_md.queue_drops";
        case MetricId::BINANCE_MD_BOOK_TICKERS: return "binance_md.book_tickers";
        case MetricId::BINANCE_MD_TRADES: return "binance_md.trades";
        case MetricId::MARKET_RECORD_PUSHES: return "market.record_pushes";
        case MetricId::MARKET_RECORD_DROPS: return "market.record_drops";
        case MetricId::USER_RECORD_PUSHES: return "user.record_pushes";
        case MetricId::USER_RECORD_DROPS: return "user.record_drops";
        case MetricId::USER_RECV_TO_PROC_NS: return "user.recv_to_proc_ns";
        case MetricId::USER_RECV_TO_PROC_COUNT: return "user.recv_to_proc_count";
        case MetricId::JOURNAL_RECORDS_PUSHED: return "journal.records_pushed";
        case MetricId::JOURNAL_RECORDS_DROPPED: return "journal.records_dropped";
        // Split order vs cancel RTT
        case MetricId::EXEC_ORDER_RTT_NS: return "exec.order_rtt_ns";
        case MetricId::EXEC_ORDER_RTT_COUNT: return "exec.order_rtt_count";
        case MetricId::EXEC_CANCEL_RTT_NS: return "exec.cancel_rtt_ns";
        case MetricId::EXEC_CANCEL_RTT_COUNT: return "exec.cancel_rtt_count";
        default: return "unknown";
    }
}

std::string Metrics::dump() const {
    std::ostringstream ss;
    ss << "=== Metrics ===\n";
    for (int i = 0; i < kMetricCount; ++i) {
        auto id = static_cast<MetricId>(i);
        int64_t val = counters_[i].value.load(std::memory_order_relaxed);
        if (val != 0) {
            ss << "  " << metric_name(id) << " = " << val;

            // Compute average latency for latency pairs
            if (id == MetricId::PARSE_LATENCY_NS) {
                auto cnt = get(MetricId::PARSE_LATENCY_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::BOOK_LATENCY_NS) {
                auto cnt = get(MetricId::BOOK_LATENCY_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::E2E_LATENCY_NS) {
                auto cnt = get(MetricId::E2E_LATENCY_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::SCHED_RECV_TO_PROC_NS) {
                auto cnt = get(MetricId::SCHED_RECV_TO_PROC_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::SCHED_STRAT_LATENCY_NS) {
                auto cnt = get(MetricId::SCHED_STRAT_LATENCY_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::SCHED_LOOP_NS) {
                auto cnt = get(MetricId::SCHED_LOOP_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::EXEC_RTT_NS) {
                auto cnt = get(MetricId::EXEC_RTT_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::STRAT_INTENT_LATENCY_NS) {
                auto cnt = get(MetricId::STRAT_INTENT_LATENCY_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::STRAT_TO_EXEC_ENQUEUE_NS) {
                auto cnt = get(MetricId::STRAT_TO_EXEC_ENQUEUE_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::EXEC_ENQUEUE_TO_SEND_NS) {
                auto cnt = get(MetricId::EXEC_ENQUEUE_TO_SEND_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::FULL_PIPELINE_NS) {
                auto cnt = get(MetricId::FULL_PIPELINE_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::USER_RECV_TO_PROC_NS) {
                auto cnt = get(MetricId::USER_RECV_TO_PROC_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::EXEC_ORDER_RTT_NS) {
                auto cnt = get(MetricId::EXEC_ORDER_RTT_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            } else if (id == MetricId::EXEC_CANCEL_RTT_NS) {
                auto cnt = get(MetricId::EXEC_CANCEL_RTT_COUNT);
                if (cnt > 0) ss << " (avg " << (val / cnt) << " ns)";
            }

            ss << "\n";
        }
    }

    // Human-readable latency summary
    ss << "\n=== Latency Summary (averages) ===\n";
    auto fmt_latency = [&](const char* label, MetricId ns_id, MetricId cnt_id) {
        auto ns = counters_[static_cast<int>(ns_id)].value.load(std::memory_order_relaxed);
        auto cnt = counters_[static_cast<int>(cnt_id)].value.load(std::memory_order_relaxed);
        auto max = counters_[static_cast<int>(ns_id)].max_ns.load(std::memory_order_relaxed);
        if (cnt > 0) {
            double avg_us = static_cast<double>(ns) / cnt / 1000.0;
            double max_us = static_cast<double>(max) / 1000.0;
            char line[128];
            std::snprintf(line, sizeof(line), "  %-16s avg %8.1f us  max %8.1f us  (%lld samples)\n",
                          label, avg_us, max_us, static_cast<long long>(cnt));
            ss << line;
        }
    };
    fmt_latency("Parse:", MetricId::PARSE_LATENCY_NS, MetricId::PARSE_LATENCY_COUNT);
    fmt_latency("Book:", MetricId::BOOK_LATENCY_NS, MetricId::BOOK_LATENCY_COUNT);
    fmt_latency("E2E (T0):", MetricId::E2E_LATENCY_NS, MetricId::E2E_LATENCY_COUNT);
    fmt_latency("Queue (T2):", MetricId::SCHED_RECV_TO_PROC_NS, MetricId::SCHED_RECV_TO_PROC_COUNT);
    fmt_latency("User (T1>T2):", MetricId::USER_RECV_TO_PROC_NS, MetricId::USER_RECV_TO_PROC_COUNT);
    fmt_latency("Strategy:", MetricId::SCHED_STRAT_LATENCY_NS, MetricId::SCHED_STRAT_LATENCY_COUNT);
    fmt_latency("Order RTT:", MetricId::EXEC_ORDER_RTT_NS, MetricId::EXEC_ORDER_RTT_COUNT);
    fmt_latency("Cancel RTT:", MetricId::EXEC_CANCEL_RTT_NS, MetricId::EXEC_CANCEL_RTT_COUNT);
    fmt_latency("REST RTT:", MetricId::EXEC_RTT_NS, MetricId::EXEC_RTT_COUNT);
    fmt_latency("Full pipe:", MetricId::FULL_PIPELINE_NS, MetricId::FULL_PIPELINE_COUNT);

    return ss.str();
}

void Metrics::dump_to_file(const std::string& path) const {
    std::ofstream file(path, std::ios::app);
    if (file.is_open()) {
        file << dump() << "\n";
    }
}

}  // namespace lt
