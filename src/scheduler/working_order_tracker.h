#pragma once

#include <cstdint>

#include "common/types.h"
#include "events/scheduler_events.h"

namespace lt {

// Tracked state for a single working order.
struct WorkingOrder {
    OrderId client_order_id;
    OrderId exchange_order_id;
    AssetId asset_id;
    AssetId market_id;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t filled_size = 0;
    Timestamp_ns sent_ts = 0;
    bool is_live = false;      // exchange confirmed
    bool is_pending = true;    // sent but not yet confirmed
    bool is_terminal = false;  // filled, canceled, or rejected
    bool occupied = false;     // slot in use
    uint8_t level = 0;         // quote level index (0 = tightest, for layered quoting)
};

// T2-owned tracker for working orders. Fixed-size array, linear scan.
// Max ~64 working orders; linear scan is faster than hashmap for small N.
// No heap allocation.
class WorkingOrderTracker {
public:
    static constexpr int kMaxWorking = 256;

    // Called after ExecSink accepts a placement intent.
    // Returns false when tracker is full and cannot record the order.
    bool on_intent_sent(const ExecutionIntent& intent);

    // Called when exec feedback arrives (ORDER_ACCEPTED, ORDER_REJECTED, etc).
    void on_exec_feedback(const SchedulerEvent& event);

    // Called when user WS update arrives (FILLED, CANCELED, PARTIAL).
    void on_user_update(const SchedulerEvent& event);

    // --- Queries for strategy and risk gate ---

    int working_count() const;
    int working_count_for_market(const AssetId& market_id) const;

    // Returns working bid/ask price for a market. kInvalidPrice if none.
    Price_t working_bid_price(const AssetId& market_id) const;
    Price_t working_ask_price(const AssetId& market_id) const;

    // Total working notional: sum(price * remaining_size) for all live+pending.
    int64_t total_working_notional() const;

    // Produce cancel intents for all working orders.
    IntentBatch cancel_all_intents() const;

    // Cancel intents for all working orders in a specific market (by condition_id).
    IntentBatch cancel_intents_for_market(const AssetId& condition_id) const;

    // Find by client_order_id
    const WorkingOrder* find_by_client_id(const OrderId& client_order_id) const;

    // Find by exchange_order_id
    const WorkingOrder* find_by_exchange_id(const OrderId& exchange_order_id) const;

    // Find first working order matching market_id + side
    const WorkingOrder* find_by_market_side(const AssetId& market_id, Side side) const;

    // Find working order matching market_id + side + level (layered quoting)
    const WorkingOrder* find_by_market_side_level(const AssetId& market_id, Side side, int level) const;

    // Returns working price for a specific level. kInvalidPrice if none.
    Price_t working_price_at_level(const AssetId& market_id, Side side, int level) const;

    // Remove stale pending orders that have been unconfirmed longer than max_age_ns.
    // Returns number of slots removed. Called periodically from T2 scheduler.
    int gc_stale_pending(Timestamp_ns now, int64_t max_age_ns);

    // Net pending exposure for a token: sum(remaining) for buys - sum(remaining) for sells
    Qty_t pending_exposure_for_token(const AssetId& token_id) const;

    // Clear all tracked orders (used on mode switch DRY_RUN -> LIVE).
    void clear_all();

    // Access backing array (for testing)
    const WorkingOrder& slot(int idx) const { return orders_[idx]; }
    int capacity() const { return kMaxWorking; }

private:
    int find_slot_by_client_id(const OrderId& id) const;
    int find_slot_by_exchange_id(const OrderId& id) const;
    int find_slot_for_user_update_fallback(const SchedulerEvent& event) const;
    int find_free_slot() const;
    void remove_slot(int idx);

    WorkingOrder orders_[kMaxWorking]{};
};

}  // namespace lt
