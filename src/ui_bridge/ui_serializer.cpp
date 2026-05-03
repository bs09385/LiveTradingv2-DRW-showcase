#include "ui_bridge/ui_serializer.h"

#include <cstdio>
#include <string_view>

#include "events/user_events.h"
#include "scheduler/execution_mode.h"
#include "scheduler/trading_session.h"

namespace lt {

void format_price(char* buf, std::size_t buf_size, Price_t price) {
    if (price < 0) {
        std::snprintf(buf, buf_size, "-1");
        return;
    }
    int whole = price / kPriceScale;
    int frac = price % kPriceScale;
    std::snprintf(buf, buf_size, "%d.%04d", whole, frac);
}

namespace {

void append(std::string& out, const char* s) { out.append(s); }

void append_key_int(std::string& out, const char* key, int64_t val) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\"%s\":%lld", key, static_cast<long long>(val));
    out.append(buf);
}

void append_key_bool(std::string& out, const char* key, bool val) {
    out.append("\"");
    out.append(key);
    out.append("\":");
    out.append(val ? "true" : "false");
}

void append_escaped_json_string(std::string& out, std::string_view val) {
    for (char ch : val) {
        switch (ch) {
            case '\\': out.append("\\\\"); break;
            case '"': out.append("\\\""); break;
            case '\b': out.append("\\b"); break;
            case '\f': out.append("\\f"); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            default: {
                auto uch = static_cast<unsigned char>(ch);
                if (uch < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(uch));
                    out.append(buf);
                } else {
                    out.push_back(ch);
                }
                break;
            }
        }
    }
}

void append_key_str(std::string& out, const char* key, std::string_view val) {
    out.append("\"");
    out.append(key);
    out.append("\":\"");
    append_escaped_json_string(out, val);
    out.append("\"");
}

void serialize_book_levels(std::string& out, const char* key,
                           const UiBookLevel* levels, int count) {
    out.append("\"");
    out.append(key);
    out.append("\":[");
    for (int i = 0; i < count; ++i) {
        if (i > 0) out.append(",");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"price\":%d,\"size\":%lld}",
                      levels[i].price, static_cast<long long>(levels[i].size));
        out.append(buf);
    }
    out.append("]");
}

void serialize_bbo(std::string& out, const char* key, const BBO& bbo) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "\"%s\":{\"best_bid\":%d,\"best_ask\":%d,\"bid_size\":%lld,\"ask_size\":%lld}",
                  key, bbo.best_bid, bbo.best_ask,
                  static_cast<long long>(bbo.bid_size),
                  static_cast<long long>(bbo.ask_size));
    out.append(buf);
}

void serialize_book_update(std::string& out, const char* key, const UiBookUpdate* book) {
    out.append("\"");
    out.append(key);
    out.append("\":{");
    if (book) {
        serialize_bbo(out, "bbo", book->bbo);
        out.append(",");
        serialize_book_levels(out, "bids", book->bids, book->bid_count);
        out.append(",");
        serialize_book_levels(out, "asks", book->asks, book->ask_count);
    } else {
        append(out, "\"bbo\":{\"best_bid\":-1,\"best_ask\":-1,\"bid_size\":0,\"ask_size\":0}");
        append(out, ",\"bids\":[],\"asks\":[]");
    }
    out.append("}");
}

const char* side_name(Side s) {
    return s == Side::BID ? "BID" : "ASK";
}

const char* order_lifecycle_name(uint8_t raw_state) {
    auto state = static_cast<UiOrderLifecycleState>(raw_state);
    switch (state) {
        case UiOrderLifecycleState::WORKING: return "WORKING";
        case UiOrderLifecycleState::FILLED: return "FILLED";
        case UiOrderLifecycleState::CANCELED_NO_FILL: return "CANCELED_NO_FILL";
        case UiOrderLifecycleState::CANCELED_WITH_FILL: return "CANCELED_WITH_FILL";
        case UiOrderLifecycleState::REJECTED: return "REJECTED";
    }
    return "WORKING";
}

const char* trade_status_name(uint8_t raw_status) {
    auto status = static_cast<TradeStatus>(raw_status);
    switch (status) {
        case TradeStatus::UNKNOWN: return "UNKNOWN";
        case TradeStatus::MATCHED: return "MATCHED";
        case TradeStatus::MINED: return "MINED";
        case TradeStatus::CONFIRMED: return "CONFIRMED";
        case TradeStatus::RETRYING: return "RETRYING";
        case TradeStatus::FAILED: return "FAILED";
    }
    return "UNKNOWN";
}

const UiMarketSnapshot* find_market_snapshot(const EngineSnapshot& snap,
                                             std::string_view market_id,
                                             std::string_view asset_id) {
    if (!market_id.empty()) {
        for (const auto& m : snap.markets) {
            if (m.condition_id.view() == market_id) return &m;
        }
    }

    if (!asset_id.empty()) {
        for (const auto& m : snap.markets) {
            if (m.token_id_up.view() == asset_id || m.token_id_down.view() == asset_id) {
                return &m;
            }
        }
    }

    return nullptr;
}

void append_market_label(std::string& out, const EngineSnapshot& snap,
                         std::string_view market_id, std::string_view asset_id) {
    const auto* m = find_market_snapshot(snap, market_id, asset_id);

    std::string label;
    if (m) {
        const char* prefix =
            (m->series_label && m->series_label[0] != '\0') ? m->series_label : "MARKET";
        label = prefix;
        if (!asset_id.empty()) {
            if (m->token_id_up.view() == asset_id) {
                label += " UP";
            } else if (m->token_id_down.view() == asset_id) {
                label += " DOWN";
            }
        }
    } else {
        label = "MARKET";
    }

    append_key_str(out, "market_label", label);
}

}  // namespace

std::string serialize_engine_snapshot(const EngineSnapshot& snap) {
    std::string out;
    out.reserve(4096);

    out.append("{");
    append_key_str(out, "type", "engine_snapshot");
    out.append(",");
    append_key_int(out, "timestamp_ns", snap.timestamp_ns);

    // Markets
    out.append(",\"markets\":[");
    for (std::size_t i = 0; i < snap.markets.size(); ++i) {
        if (i > 0) out.append(",");
        const auto& m = snap.markets[i];
        out.append("{");
        append_key_str(out, "condition_id", m.condition_id.view());
        out.append(",");
        append_key_str(out, "token_id_up", m.token_id_up.view());
        out.append(",");
        append_key_str(out, "token_id_down", m.token_id_down.view());
        out.append(",");
        append_key_str(out, "series_label", m.series_label ? m.series_label : "");
        out.append(",");
        serialize_book_update(out, "book_up", m.book_up);
        out.append(",");
        serialize_book_update(out, "book_down", m.book_down);
        out.append(",");
        append_key_int(out, "position_up", m.position_up);
        out.append(",");
        append_key_int(out, "position_down", m.position_down);
        out.append("}");
    }
    out.append("]");

    // State
    out.append(",\"state\":{");
    out.append("\"working_orders\":[");
    if (snap.state) {
        for (int i = 0; i < snap.state->working_order_count; ++i) {
            if (i > 0) out.append(",");
            const auto& wo = snap.state->working_orders[i];

            // Dual-bid display: DOWN BID orders appear as ASK at complement price
            // so the ladder shows the familiar bid/ask view.
            Side display_side = wo.side;
            Price_t display_price = wo.price;
            const auto* mkt = find_market_snapshot(snap, wo.market_id.view(), wo.asset_id.view());
            if (mkt && wo.side == Side::BID &&
                mkt->token_id_down.view() == wo.asset_id.view()) {
                display_side = Side::ASK;
                display_price = static_cast<Price_t>(kPriceScale - wo.price);
            }

            out.append("{");
            append_key_str(out, "client_order_id", wo.client_order_id.view());
            out.append(",");
            append_key_str(out, "exchange_order_id", wo.exchange_order_id.view());
            out.append(",");
            append_key_str(out, "asset_id", wo.asset_id.view());
            out.append(",");
            append_key_str(out, "market_id", wo.market_id.view());
            out.append(",");
            append_market_label(out, snap, wo.market_id.view(), wo.asset_id.view());
            out.append(",");
            append_key_str(out, "side", side_name(display_side));
            out.append(",");
            append_key_int(out, "price", display_price);
            out.append(",");
            append_key_int(out, "original_size", wo.original_size);
            out.append(",");
            append_key_int(out, "filled_size", wo.filled_size);
            out.append(",");
            append_key_bool(out, "is_live", wo.is_live);
            out.append(",");
            append_key_bool(out, "is_pending", wo.is_pending);
            out.append(",");
            append_key_str(out, "lifecycle_state", order_lifecycle_name(wo.lifecycle_state));
            out.append(",");
            append_key_int(out, "last_update_ts", wo.last_update_ts);
            out.append("}");
        }
    }
    out.append("]");

    out.append(",\"closed_orders\":[");
    if (snap.state) {
        for (int i = 0; i < snap.state->closed_order_count; ++i) {
            if (i > 0) out.append(",");
            const auto& co = snap.state->closed_orders[i];

            Side co_display_side = co.side;
            Price_t co_display_price = co.price;
            const auto* co_mkt = find_market_snapshot(snap, co.market_id.view(), co.asset_id.view());
            if (co_mkt && co.side == Side::BID &&
                co_mkt->token_id_down.view() == co.asset_id.view()) {
                co_display_side = Side::ASK;
                co_display_price = static_cast<Price_t>(kPriceScale - co.price);
            }

            out.append("{");
            append_key_str(out, "client_order_id", co.client_order_id.view());
            out.append(",");
            append_key_str(out, "exchange_order_id", co.exchange_order_id.view());
            out.append(",");
            append_key_str(out, "asset_id", co.asset_id.view());
            out.append(",");
            append_key_str(out, "market_id", co.market_id.view());
            out.append(",");
            append_market_label(out, snap, co.market_id.view(), co.asset_id.view());
            out.append(",");
            append_key_str(out, "side", side_name(co_display_side));
            out.append(",");
            append_key_int(out, "price", co_display_price);
            out.append(",");
            append_key_int(out, "original_size", co.original_size);
            out.append(",");
            append_key_int(out, "filled_size", co.filled_size);
            out.append(",");
            append_key_str(out, "lifecycle_state", order_lifecycle_name(co.lifecycle_state));
            out.append(",");
            append_key_int(out, "last_update_ts", co.last_update_ts);
            out.append("}");
        }
    }
    out.append("]");

    out.append(",\"trades\":[");
    if (snap.state) {
        for (int i = 0; i < snap.state->trade_count; ++i) {
            if (i > 0) out.append(",");
            const auto& tr = snap.state->trades[i];

            Side tr_display_side = tr.side;
            Price_t tr_display_price = tr.price;
            const auto* tr_mkt = find_market_snapshot(snap, tr.market_id.view(), tr.asset_id.view());
            if (tr_mkt && tr.side == Side::BID &&
                tr_mkt->token_id_down.view() == tr.asset_id.view()) {
                tr_display_side = Side::ASK;
                tr_display_price = static_cast<Price_t>(kPriceScale - tr.price);
            }

            out.append("{");
            append_key_str(out, "trade_id", tr.trade_id.view());
            out.append(",");
            append_key_str(out, "order_id", tr.order_id.view());
            out.append(",");
            append_key_str(out, "asset_id", tr.asset_id.view());
            out.append(",");
            append_key_str(out, "market_id", tr.market_id.view());
            out.append(",");
            append_market_label(out, snap, tr.market_id.view(), tr.asset_id.view());
            out.append(",");
            append_key_str(out, "side", side_name(tr_display_side));
            out.append(",");
            append_key_int(out, "price", tr_display_price);
            out.append(",");
            append_key_int(out, "size", tr.size);
            out.append(",");
            append_key_str(out, "status", trade_status_name(tr.trade_status));
            out.append(",");
            append_key_int(out, "last_update_ts", tr.last_update_ts);
            out.append("}");
        }
    }
    out.append("]");

    out.append(",\"positions\":[");
    if (snap.state) {
        for (int i = 0; i < snap.state->position_count; ++i) {
            if (i > 0) out.append(",");
            const auto& pos = snap.state->positions[i];
            out.append("{");
            append_key_str(out, "token_id", pos.token_id.view());
            out.append(",");
            append_key_int(out, "position", pos.position);
            out.append("}");
        }
    }
    out.append("]");

    if (snap.state) {
        out.append(",");
        append_key_bool(out, "strategy_enabled", snap.state->strategy_enabled);
        out.append(",");
        append_key_int(out, "spread_ticks", snap.state->spread_ticks);
        out.append(",");
        append_key_int(out, "quote_size", snap.state->quote_size);
        out.append(",");
        append_key_str(out, "execution_mode",
                       execution_mode_name(static_cast<ExecutionMode>(snap.state->execution_mode)));
        out.append(",");
        append_key_int(out, "risk_checks", snap.state->risk_checks);
        out.append(",");
        append_key_int(out, "risk_allows", snap.state->risk_allows);
        out.append(",");
        append_key_int(out, "risk_denies", snap.state->risk_denies);
        out.append(",");
        append_key_str(out, "session_state",
                       session_state_name(static_cast<SessionState>(snap.state->session_state)));
        out.append(",");
        append_key_int(out, "session_end_time_s", snap.state->session_end_time_s);
        out.append(",");
        append_key_int(out, "session_markets_entered", snap.state->session_markets_entered);
    } else {
        append(out, ",\"strategy_enabled\":false,\"spread_ticks\":0,\"quote_size\":0");
        append(out, ",\"execution_mode\":\"DRY_RUN\"");
        append(out, ",\"risk_checks\":0,\"risk_allows\":0,\"risk_denies\":0");
        append(out, ",\"session_state\":\"IDLE\",\"session_end_time_s\":0,\"session_markets_entered\":0");
    }
    out.append("}");

    // Metrics
    out.append(",\"metrics\":{");
    append_key_int(out, "ws_frames", snap.metrics.ws_frames);
    out.append(",");
    append_key_int(out, "parse_ok", snap.metrics.parse_ok);
    out.append(",");
    append_key_int(out, "sched_cycles", snap.metrics.sched_cycles);
    out.append(",");
    append_key_int(out, "sched_events", snap.metrics.sched_events);
    out.append(",");
    append_key_int(out, "rest_requests", snap.metrics.rest_requests);
    out.append(",");
    append_key_int(out, "rest_errors", snap.metrics.rest_errors);
    out.append(",");
    append_key_int(out, "ui_snapshots_dropped", snap.metrics.ui_snapshots_dropped);
    out.append(",");
    append_key_int(out, "ui_book_drops", snap.metrics.ui_book_drops);
    out.append(",");
    append_key_int(out, "ui_state_drops", snap.metrics.ui_state_drops);
    out.append("}");

    // Gateway
    out.append(",\"gateway\":{");
    append_key_bool(out, "degraded", snap.gateway.degraded);
    out.append(",");
    append_key_int(out, "heartbeat_ok", snap.gateway.heartbeat_ok);
    out.append(",");
    append_key_int(out, "heartbeat_fail", snap.gateway.heartbeat_fail);
    out.append("}");

    // Account
    out.append(",\"account\":{");
    append_key_str(out, "name", snap.account_name);
    out.append(",");
    append_key_str(out, "address", snap.account_address);
    out.append("}");

    // Rotation
    out.append(",\"rotation\":{");
    append_key_str(out, "market", snap.rotation.market_condition);
    out.append(",");
    append_key_int(out, "window_start", snap.rotation.window_start);
    out.append(",");
    append_key_int(out, "window_end", snap.rotation.window_end);
    out.append(",");
    append_key_int(out, "rotations", snap.rotation.rotation_count);
    out.append(",");
    append_key_bool(out, "no_trade", snap.rotation.in_no_trade);
    out.append("}");

    // Balance
    out.append(",\"balance\":{");
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "\"position_value\":%.2f", snap.account_balance.position_value);
        out.append(buf);
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"usdc_balance\":%.2f",
                      static_cast<double>(snap.account_balance.usdc_balance) / 1000000.0);
        out.append(buf);
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"realized_pnl\":%.2f",
                      static_cast<double>(snap.account_balance.realized_pnl) / 1000000.0);
        out.append(buf);
    }
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"pol_balance\":%.4f",
                      snap.account_balance.pol_balance);
        out.append(buf);
    }
    out.append(",");
    append_key_bool(out, "available", snap.account_balance.available);
    if (snap.account_balance.error && snap.account_balance.error[0] != '\0') {
        out.append(",");
        append_key_str(out, "error", snap.account_balance.error);
    }
    out.append("}");

    // Latency
    out.append(",\"latency\":{");
    append_key_int(out, "order_rtt_avg_ns", snap.latency.order_rtt_avg_ns);
    out.append(",");
    append_key_int(out, "order_rtt_p95_ns", snap.latency.order_rtt_p95_ns);
    out.append(",");
    append_key_int(out, "cancel_rtt_avg_ns", snap.latency.cancel_rtt_avg_ns);
    out.append(",");
    append_key_int(out, "cancel_rtt_p95_ns", snap.latency.cancel_rtt_p95_ns);
    out.append(",");
    append_key_int(out, "engine_avg_ns", snap.latency.engine_avg_ns);
    out.append(",");
    append_key_int(out, "engine_p95_ns", snap.latency.engine_p95_ns);
    out.append(",");
    append_key_int(out, "pipeline_avg_ns", snap.latency.pipeline_avg_ns);
    out.append(",");
    append_key_int(out, "pipeline_p95_ns", snap.latency.pipeline_p95_ns);
    out.append(",");
    append_key_int(out, "ws_book_avg_ns", snap.latency.ws_book_avg_ns);
    out.append(",");
    append_key_int(out, "ws_book_p95_ns", snap.latency.ws_book_p95_ns);
    out.append(",");
    append_key_int(out, "exchange_to_recv_avg_ns", snap.latency.exchange_to_recv_avg_ns);
    out.append(",");
    append_key_int(out, "exchange_to_recv_p95_ns", snap.latency.exchange_to_recv_p95_ns);
    out.append(",");
    append_key_int(out, "binance_md_avg_ns", snap.latency.binance_md_avg_ns);
    out.append(",");
    append_key_int(out, "binance_md_p95_ns", snap.latency.binance_md_p95_ns);
    out.append(",");
    append_key_int(out, "probe_order_rtt_ns", snap.latency.probe_order_rtt_ns);
    out.append(",");
    append_key_int(out, "probe_cancel_rtt_ns", snap.latency.probe_cancel_rtt_ns);
    out.append(",");
    append_key_int(out, "probe_roundtrip_ns", snap.latency.probe_roundtrip_ns);
    out.append(",");
    append_key_int(out, "probe_status", snap.latency.probe_status);
    out.append("}");

    out.append("}");
    return out;
}

std::string serialize_series_list(const std::vector<SeriesListEntry>& entries) {
    std::string out;
    out.reserve(1024);

    out.append("{");
    append_key_str(out, "type", "series_list");
    out.append(",\"series\":[");

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) out.append(",");
        const auto& e = entries[i];
        out.append("{");
        append_key_str(out, "series_key", timeframe_name(e.timeframe));
        out.append(",");
        append_key_str(out, "condition_id", e.condition_id);
        out.append(",");
        append_key_str(out, "status", watcher_state_name(e.status));
        out.append(",");
        append_key_bool(out, "has_next", e.has_next);
        out.append("}");
    }

    out.append("]}");
    return out;
}

std::string serialize_watcher_books(BtcTimeframe tf,
                                     const char* condition_id,
                                     const WatcherBookStore::MergedLadder& ladder,
                                     const std::vector<WatcherBookLevel>& trades,
                                     TickSize_t tick_size) {
    std::string out;
    out.reserve(4096);

    out.append("{");
    append_key_str(out, "type", "watcher_books");
    out.append(",");
    append_key_str(out, "series_key", timeframe_name(tf));
    out.append(",");
    append_key_str(out, "condition_id", condition_id);

    // Buy levels (descending by price)
    out.append(",\"buy_levels\":[");
    for (std::size_t i = 0; i < ladder.buy_levels.size(); ++i) {
        if (i > 0) out.append(",");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"price\":%d,\"size\":%lld}",
                      ladder.buy_levels[i].price,
                      static_cast<long long>(ladder.buy_levels[i].size));
        out.append(buf);
    }
    out.append("]");

    // Sell levels (ascending by price)
    out.append(",\"sell_levels\":[");
    for (std::size_t i = 0; i < ladder.sell_levels.size(); ++i) {
        if (i > 0) out.append(",");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"price\":%d,\"size\":%lld}",
                      ladder.sell_levels[i].price,
                      static_cast<long long>(ladder.sell_levels[i].size));
        out.append(buf);
    }
    out.append("]");

    // Trades since last snapshot
    out.append(",\"trades\":[");
    for (std::size_t i = 0; i < trades.size(); ++i) {
        if (i > 0) out.append(",");
        char buf[128];
        std::snprintf(buf, sizeof(buf), "{\"price\":%d,\"size\":%lld}",
                      trades[i].price,
                      static_cast<long long>(trades[i].size));
        out.append(buf);
    }
    out.append("]");

    // Tick size
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), ",\"tick_size\":%d", static_cast<int>(tick_size));
        out.append(buf);
    }

    out.append("}");
    return out;
}

std::string serialize_watcher_status(BtcTimeframe tf, WatcherState state) {
    std::string out;
    out.reserve(128);

    out.append("{");
    append_key_str(out, "type", "watcher_status");
    out.append(",");
    append_key_str(out, "series_key", timeframe_name(tf));
    out.append(",");
    append_key_str(out, "status", watcher_state_name(state));
    out.append("}");
    return out;
}

}  // namespace lt
