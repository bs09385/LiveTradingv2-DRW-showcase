#pragma once

#include <atomic>
#include <unordered_map>

#include "common/pnl_tracker.h"
#include "common/token_inventory.h"
#include "common/types.h"
#include "events/scheduler_events.h"
#include "events/user_events.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"

namespace lt {

// Result of applying an order or trade update
enum class ApplyResult : uint8_t {
    APPLIED = 0,     // state changed, event emitted
    DUPLICATE,       // no state change (idempotent skip)
    QUEUE_OVERFLOW,  // state changed but queue push failed
};

// Per-order tracked state
struct TrackedOrder {
    OrderId order_id;
    OrderId client_order_id;    // client-assigned ID for REST correlation (M4)
    AssetId asset_id;
    OrderStatus status = OrderStatus::UNKNOWN;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t size_matched = 0;     // cumulative filled
    Timestamp_ns first_seen_ts = 0;
    Timestamp_ns last_update_ts = 0;
    SeqNum_t last_seq = 0;
};

// Per-trade tracked state
struct TrackedTrade {
    TradeId trade_id;
    OrderId taker_order_id;
    AssetId asset_id;
    TradeStatus status = TradeStatus::UNKNOWN;
    Side side = Side::BID;
    Price_t fill_price = 0;
    Qty_t fill_size = 0;
    Timestamp_ns first_seen_ts = 0;
    Timestamp_ns last_update_ts = 0;
    int64_t pnl_delta = 0;  // PnL recorded by PnlTracker for this fill (for exact reversal)
};

// Position delta per asset
struct PositionDelta {
    Qty_t net_position = 0;    // +bought, -sold
    Qty_t total_bought = 0;
    Qty_t total_sold = 0;
    int64_t fill_count = 0;
};

struct ExecStateStoreConfig {
    std::size_t max_orders = 50'000;
    std::size_t max_trades = 50'000;
    std::size_t max_seen_trade_fills = 100'000;
    int64_t terminal_order_ttl_ms = 30LL * 60LL * 1000LL;
    int64_t terminal_trade_ttl_ms = 30LL * 60LL * 1000LL;
    int64_t seen_trade_fill_ttl_ms = 60LL * 60LL * 1000LL;
    int64_t sweep_interval_ms = 1000;
};

// ---------------------------------------------------------------------------
// ExecStateStore: order/trade lifecycle tracking + fill dedup + positions.
//
// T1-owned (single-writer). Pushes SchedulerEvents to user_queue for T2.
// ---------------------------------------------------------------------------
class ExecStateStore {
public:
    ExecStateStore(SpscQueue<SchedulerEvent>& user_queue, Metrics& metrics,
                   AsyncLogger* logger = nullptr,
                   std::atomic<bool>* fatal_flag = nullptr,
                   TokenInventory* inventory = nullptr,
                   PnlTracker* pnl_tracker = nullptr,
                   const ExecStateStoreConfig& config = {});

    ApplyResult apply_order_update(const UserOrderUpdate& upd,
                                   Timestamp_ns recv_ts, SeqNum_t seq);

    ApplyResult apply_trade_update(const UserTradeUpdate& upd,
                                   Timestamp_ns recv_ts, SeqNum_t seq);

    // Read-only accessors
    const TrackedOrder* get_order(const OrderId& id) const;
    const TrackedTrade* get_trade(const TradeId& id) const;
    const PositionDelta* get_position(const AssetId& id) const;

    std::size_t order_count() const { return orders_.size(); }
    std::size_t trade_count() const { return trades_.size(); }
    std::size_t position_count() const { return positions_.size(); }

private:
    int64_t record_fill(const AssetId& asset_id, Side side, Qty_t size, Price_t fill_price);
    void reverse_fill(const AssetId& asset_id, Side side, Qty_t size, Price_t fill_price, int64_t pnl_delta);
    void maybe_sweep(Timestamp_ns now);
    void evict_terminal_orders(Timestamp_ns now);
    void evict_terminal_trades(Timestamp_ns now);
    void evict_seen_trade_fills(Timestamp_ns now);

    std::unordered_map<OrderId, TrackedOrder, OrderIdHash> orders_;
    std::unordered_map<TradeId, TrackedTrade, TradeIdHash> trades_;
    std::unordered_map<TradeId, Timestamp_ns, TradeIdHash> seen_trade_fills_;
    std::unordered_map<AssetId, PositionDelta, AssetIdHash> positions_;

    SpscQueue<SchedulerEvent>& user_queue_;
    Metrics& metrics_;
    ProducerHandle log_handle_{};
    std::atomic<bool>* fatal_flag_ = nullptr;
    TokenInventory* inventory_ = nullptr;
    PnlTracker* pnl_tracker_ = nullptr;
    ExecStateStoreConfig config_;
    Timestamp_ns next_sweep_ts_ = 0;
};

}  // namespace lt
