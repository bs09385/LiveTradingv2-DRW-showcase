#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/market_pair.h"
#include "common/types.h"
#include "ui_bridge/ui_types.h"
#include "ui_bridge/watcher_types.h"
#include "ui_bridge/watcher_book_store.h"

namespace lt {

struct UiMarketSnapshot {
    AssetId condition_id;
    AssetId token_id_up;
    AssetId token_id_down;
    const char* series_label = "";   // e.g., "BTC 5M"
    const UiBookUpdate* book_up = nullptr;   // may be null if no data
    const UiBookUpdate* book_down = nullptr;  // may be null if no data
    Qty_t position_up = 0;
    Qty_t position_down = 0;
};

struct UiGatewayHealth {
    bool degraded = false;
    int64_t heartbeat_ok = 0;
    int64_t heartbeat_fail = 0;
};

struct UiMetricsSnapshot {
    int64_t ws_frames = 0;
    int64_t parse_ok = 0;
    int64_t sched_cycles = 0;
    int64_t sched_events = 0;
    int64_t rest_requests = 0;
    int64_t rest_errors = 0;
    int64_t ui_snapshots_dropped = 0;
    int64_t ui_book_drops = 0;
    int64_t ui_state_drops = 0;
};

struct UiRotationInfo {
    const char* market_condition = "";
    int64_t window_start = 0;
    int64_t window_end = 0;
    int rotation_count = 0;
    bool in_no_trade = false;
};

struct UiAccountBalance {
    double position_value = 0.0;
    int64_t usdc_balance = 0;   // micro-USDC (fp6)
    int64_t realized_pnl = 0;   // micro-USDC (fp6), FIFO trading PnL
    double pol_balance = 0.0;   // POL/MATIC balance (full units, e.g. 10.0 = 10 POL)
    bool available = false;
    const char* error = "";
};

struct UiLatencySnapshot {
    int64_t order_rtt_avg_ns = 0;
    int64_t order_rtt_p95_ns = 0;
    int64_t cancel_rtt_avg_ns = 0;
    int64_t cancel_rtt_p95_ns = 0;
    int64_t engine_avg_ns = 0;
    int64_t engine_p95_ns = 0;
    int64_t pipeline_avg_ns = 0;
    int64_t pipeline_p95_ns = 0;
    int64_t ws_book_avg_ns = 0;
    int64_t ws_book_p95_ns = 0;
    int64_t exchange_to_recv_avg_ns = 0;
    int64_t exchange_to_recv_p95_ns = 0;
    int64_t binance_md_avg_ns = 0;
    int64_t binance_md_p95_ns = 0;
    int64_t probe_order_rtt_ns = 0;
    int64_t probe_cancel_rtt_ns = 0;
    int64_t probe_roundtrip_ns = 0;
    uint8_t probe_status = 0;  // 0=READY, 1=RUNNING, 2=DONE, 3=FAILED
};

struct EngineSnapshot {
    Timestamp_ns timestamp_ns = 0;
    std::vector<UiMarketSnapshot> markets;
    const UiStateSnapshot* state = nullptr;
    UiMetricsSnapshot metrics;
    UiGatewayHealth gateway;
    const char* account_name = "";
    const char* account_address = "";
    UiRotationInfo rotation;
    UiAccountBalance account_balance;
    UiLatencySnapshot latency;
};

// Serialize an EngineSnapshot to JSON string.
// Now includes "type":"engine_snapshot" field for backward-compatible protocol extension.
std::string serialize_engine_snapshot(const EngineSnapshot& snap);

// Convert a 10000x fixed-point price to a decimal string (e.g., 5200 -> "0.5200")
void format_price(char* buf, std::size_t buf_size, Price_t price);

// --- Watcher serialization (BTC ladder watch feature) ---

struct SeriesListEntry {
    BtcTimeframe timeframe = BtcTimeframe::BTC_5M;
    const char* condition_id = "";
    WatcherState status = WatcherState::DISCONNECTED;
    bool has_next = false;
};

// Serialize a series list message.
// {"type":"series_list","series":[{series_key, condition_id, status, has_next},...]}
std::string serialize_series_list(const std::vector<SeriesListEntry>& entries);

// Serialize a watcher books message with merged ladder, trades, and tick size.
// {"type":"watcher_books","series_key":"BTC_5m","condition_id":"...",
//  "buy_levels":[...],"sell_levels":[...],"trades":[...],"tick_size":100}
std::string serialize_watcher_books(BtcTimeframe tf,
                                     const char* condition_id,
                                     const WatcherBookStore::MergedLadder& ladder,
                                     const std::vector<WatcherBookLevel>& trades,
                                     TickSize_t tick_size);

// Serialize a watcher status message.
// {"type":"watcher_status","series_key":"BTC_5m","status":"CONNECTED"}
std::string serialize_watcher_status(BtcTimeframe tf, WatcherState state);

}  // namespace lt
