#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"
#include "events/event_variant.h"
#include "events/user_events.h"

namespace lt {

// ---------------------------------------------------------------------------
// Event source: identifies which input queue / subsystem produced the event.
// Priority order (highest first): USER_WS > EXEC_INTERNAL > MARKET_WS > CONTROL
// ---------------------------------------------------------------------------
enum class EventSource : uint8_t {
    MARKET_WS = 0,
    USER_WS = 1,
    EXEC_INTERNAL = 2,
    CONTROL = 3,
};

inline constexpr int kEventSourceCount = 4;

// Lower value = higher priority
inline constexpr int source_priority(EventSource s) {
    switch (s) {
        case EventSource::USER_WS: return 0;
        case EventSource::EXEC_INTERNAL: return 1;
        case EventSource::MARKET_WS: return 2;
        case EventSource::CONTROL: return 3;
    }
    return 99;
}

// ---------------------------------------------------------------------------
// Event kinds within each source
// ---------------------------------------------------------------------------
enum class SchedulerEventKind : uint8_t {
    // Market WS events (mapped from M1 NotificationKind)
    MARKET_BOOK_SNAPSHOT = 0,
    MARKET_PRICE_CHANGE,
    MARKET_BBO_UPDATE,
    MARKET_TICK_SIZE_CHANGE,
    MARKET_LAST_TRADE,

    // User WS events (stub for future M3)
    USER_ORDER_UPDATE,
    USER_TRADE_UPDATE,
    USER_BALANCE_UPDATE,

    // Execution internal events (stub for future M4)
    EXEC_ORDER_ACK,
    EXEC_ORDER_REJECT,
    EXEC_FILL,

    // Control events
    CONTROL_SHUTDOWN,
    CONTROL_PAUSE,
    CONTROL_RESUME,
    CONTROL_CONFIG_UPDATE,

    // M5 control events
    CONTROL_CANCEL_ALL,
    CONTROL_SET_MODE,
    CONTROL_SET_SPREAD,
    CONTROL_SET_SIZE,
    CONTROL_ENABLE_STRATEGY,
    CONTROL_INVENTORY_SPLIT,
    CONTROL_INVENTORY_MERGE,
    CONTROL_INVENTORY_REDEEM,

    // Market lifecycle events
    MARKET_NEW_MARKET,
    MARKET_RESOLVED,

    // Slot lifecycle events (T7 -> T2 via slot_queue)
    SLOT_ACTIVATED,     // Slot is now active for quoting
    SLOT_CLOSING,       // Window ended — cancel all working orders for this slot
    SLOT_PROMOTED,      // NEXT relabeled to CURRENT (inventory follows automatically)
    SLOT_DEMOTED,       // CURRENT relabeled to PREVIOUS (active=false)
    SLOT_REMOVED,       // PREVIOUS cleared — unsubscribe, remove from map

    // Trading session events
    CONTROL_START_SESSION,   // Start a trading session (IDLE → PENDING)
    CONTROL_STOP_SESSION,    // Stop session immediately (any → IDLE)

    // Latency probe
    CONTROL_LATENCY_PROBE,   // UI requests a latency probe order+cancel

    // Emergency flatten
    CONTROL_MARKET_SELL_ALL,  // FAK sweep sell all UP tokens across active markets
    CONTROL_MARKET_SELL_DOWN, // FAK sweep sell all DOWN tokens across active markets
};

inline constexpr int kSchedulerEventKindCount = 33;

// ---------------------------------------------------------------------------
// SchedulerEvent: unified, fixed-size event envelope consumed by T2.
//
// All fields are POD, no heap allocation, trivially copyable.
// Payload fields are populated based on the source; unused fields are zero.
//
// Thread ownership: produced by various threads (via SPSC queues),
// consumed exclusively by T2 (StrategyScheduler).
// ---------------------------------------------------------------------------
struct SchedulerEvent {
    EventSource source = EventSource::MARKET_WS;
    SchedulerEventKind kind = SchedulerEventKind::MARKET_BOOK_SNAPSHOT;
    AssetId asset_id;
    Timestamp_ns recv_ts = 0;   // monotonic receive timestamp (for latency tracking)
    SeqNum_t seq = 0;           // source sequence number if available

    // --- Market WS payload (source == MARKET_WS) ---
    BBOSnapshot bbo;
    Timestamp_ns exchange_ts = 0;    // exchange timestamp (ms since epoch), 0 if absent
    TickSize_t market_tick_size = 0;  // non-zero for MARKET_TICK_SIZE_CHANGE

    // --- Exec internal payload (source == EXEC_INTERNAL) ---
    uint32_t intent_ref_id = 0;   // references the original ExecutionIntent
    bool exec_accepted = false;
    uint8_t exec_feedback_kind = 0;  // cast of ExecFeedbackKind (M4)
    int exec_http_status = 0;        // HTTP status from REST response (M4)

    // --- User WS payload (source == USER_WS) ---
    OrderId order_id;
    OrderId client_order_id;    // client-assigned ID for REST correlation (M4)
    TradeId trade_id;
    uint8_t order_status_raw = 0;    // cast of OrderStatus
    uint8_t trade_status_raw = 0;    // cast of TradeStatus
    Side user_side = Side::BID;
    AssetId user_market_id;
    Price_t user_price = 0;
    Qty_t user_original_size = 0;
    Qty_t user_fill_size = 0;        // fill size for trades, size_matched for orders
    Qty_t user_cumulative_filled = 0;
    bool is_new_fill = false;         // true when trade MATCHED counted as new fill

    // --- Control payload (source == CONTROL) ---
    uint8_t control_cmd = 0;
    uint8_t control_mode = 0;         // M5: cast of ExecutionMode for CONTROL_SET_MODE
    int32_t control_int_param = 0;    // M5: integer param (spread ticks, size)
    bool control_bool_param = false;  // M5: bool param (enable/disable strategy)
    AssetId control_condition_id;      // inventory commands: target condition
    AssetId control_token_id;          // inventory redeem: optional winning token
    Qty_t control_qty_param = 0;       // inventory commands: scaled quantity

    // --- Market lifecycle payload (MARKET_RESOLVED) ---
    AssetId resolved_condition_id;
    AssetId resolved_winning_asset_id;

    // --- Slot lifecycle payload (SLOT_* events) ---
    uint8_t slot_name = 0;  // cast of SlotName

    // Factory: wrap a MarketNotification from M1's Q_market_to_strategy.
    // Unknown NotificationKind values map to MARKET_LAST_TRADE (non-triggering)
    // to avoid misclassification as BOOK_SNAPSHOT.
    static SchedulerEvent from_market(const MarketNotification& notif) {
        SchedulerEvent ev;
        ev.source = EventSource::MARKET_WS;
        ev.kind = SchedulerEventKind::MARKET_LAST_TRADE;  // safe non-triggering default
        ev.asset_id = notif.asset_id;
        ev.recv_ts = notif.recv_ts;
        ev.seq = notif.seq;
        ev.bbo = notif.bbo;
        ev.exchange_ts = notif.exchange_ts;
        ev.market_tick_size = notif.tick_size;

        switch (notif.kind) {
            case NotificationKind::BOOK_SNAPSHOT:
                ev.kind = SchedulerEventKind::MARKET_BOOK_SNAPSHOT;
                break;
            case NotificationKind::PRICE_CHANGE:
                ev.kind = SchedulerEventKind::MARKET_PRICE_CHANGE;
                break;
            case NotificationKind::BBO_UPDATE:
                ev.kind = SchedulerEventKind::MARKET_BBO_UPDATE;
                break;
            case NotificationKind::TICK_SIZE_CHANGE:
                ev.kind = SchedulerEventKind::MARKET_TICK_SIZE_CHANGE;
                break;
            case NotificationKind::LAST_TRADE:
                ev.kind = SchedulerEventKind::MARKET_LAST_TRADE;
                break;
            case NotificationKind::NEW_MARKET:
                ev.kind = SchedulerEventKind::MARKET_NEW_MARKET;
                break;
            case NotificationKind::MARKET_RESOLVED:
                ev.kind = SchedulerEventKind::MARKET_RESOLVED;
                ev.resolved_condition_id = notif.asset_id;
                ev.resolved_winning_asset_id = notif.resolved_winning_asset_id;
                break;
            default:
                break;  // keeps safe non-triggering default
        }
        return ev;
    }

    // Factory: wrap a UserOrderUpdate from T1's ExecStateStore.
    static SchedulerEvent from_order_update(const UserOrderUpdate& upd,
                                            Timestamp_ns recv_ts, SeqNum_t seq) {
        SchedulerEvent ev;
        ev.source = EventSource::USER_WS;
        ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
        ev.asset_id = upd.asset_id;
        ev.recv_ts = recv_ts;
        ev.seq = seq;
        ev.order_id = upd.order_id;
        ev.client_order_id = upd.client_order_id;
        ev.user_side = upd.side;
        ev.user_market_id = upd.market_id;
        ev.user_price = upd.price;
        ev.user_original_size = upd.original_size;
        ev.user_fill_size = upd.size_matched;
        ev.user_cumulative_filled = upd.size_matched;
        return ev;
    }

    // Factory: wrap a UserTradeUpdate from T1's ExecStateStore.
    static SchedulerEvent from_trade_update(const UserTradeUpdate& upd,
                                            Timestamp_ns recv_ts, SeqNum_t seq,
                                            bool new_fill) {
        SchedulerEvent ev;
        ev.source = EventSource::USER_WS;
        ev.kind = SchedulerEventKind::USER_TRADE_UPDATE;
        ev.asset_id = upd.asset_id;
        ev.recv_ts = recv_ts;
        ev.seq = seq;
        ev.trade_id = upd.trade_id;
        ev.order_id = upd.taker_order_id;
        ev.trade_status_raw = static_cast<uint8_t>(upd.status);
        ev.user_side = upd.side;
        ev.user_market_id = upd.market_id;
        ev.user_price = upd.fill_price;
        ev.user_fill_size = upd.fill_size;
        ev.is_new_fill = new_fill;
        return ev;
    }
    // --- M5 control event factories ---

    static SchedulerEvent make_cancel_all() {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_CANCEL_ALL;
        return ev;
    }

    static SchedulerEvent make_set_mode(uint8_t mode) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_SET_MODE;
        ev.control_mode = mode;
        return ev;
    }

    static SchedulerEvent make_set_spread(int32_t spread_ticks) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_SET_SPREAD;
        ev.control_int_param = spread_ticks;
        return ev;
    }

    static SchedulerEvent make_set_size(int32_t size) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_SET_SIZE;
        ev.control_int_param = size;
        return ev;
    }

    static SchedulerEvent make_enable_strategy(bool enabled) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_ENABLE_STRATEGY;
        ev.control_bool_param = enabled;
        return ev;
    }

    static SchedulerEvent make_inventory_split(const AssetId& condition_id, Qty_t qty) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_INVENTORY_SPLIT;
        ev.control_condition_id = condition_id;
        ev.control_qty_param = qty;
        return ev;
    }

    static SchedulerEvent make_inventory_merge(const AssetId& condition_id, Qty_t qty) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_INVENTORY_MERGE;
        ev.control_condition_id = condition_id;
        ev.control_qty_param = qty;
        return ev;
    }

    static SchedulerEvent make_inventory_redeem(const AssetId& condition_id,
                                                const AssetId& token_id,
                                                Qty_t qty) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_INVENTORY_REDEEM;
        ev.control_condition_id = condition_id;
        ev.control_token_id = token_id;
        ev.control_qty_param = qty;
        return ev;
    }

    // --- Slot lifecycle event factories ---

    static SchedulerEvent make_slot_event(SchedulerEventKind kind,
                                           uint8_t slot,
                                           const AssetId& condition_id) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = kind;
        ev.slot_name = slot;
        ev.control_condition_id = condition_id;
        return ev;
    }

    static SchedulerEvent make_slot_activated(uint8_t slot, const AssetId& cond) {
        return make_slot_event(SchedulerEventKind::SLOT_ACTIVATED, slot, cond);
    }
    static SchedulerEvent make_slot_closing(uint8_t slot, const AssetId& cond) {
        return make_slot_event(SchedulerEventKind::SLOT_CLOSING, slot, cond);
    }
    static SchedulerEvent make_slot_promoted(uint8_t slot, const AssetId& cond,
                                              int64_t window_end_s = 0) {
        auto ev = make_slot_event(SchedulerEventKind::SLOT_PROMOTED, slot, cond);
        ev.control_qty_param = window_end_s;
        return ev;
    }
    static SchedulerEvent make_slot_demoted(uint8_t slot, const AssetId& cond) {
        return make_slot_event(SchedulerEventKind::SLOT_DEMOTED, slot, cond);
    }
    static SchedulerEvent make_slot_removed(uint8_t slot, const AssetId& cond) {
        return make_slot_event(SchedulerEventKind::SLOT_REMOVED, slot, cond);
    }

    // --- Trading session event factories ---

    static SchedulerEvent make_start_session(int64_t end_time_s) {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_START_SESSION;
        ev.control_qty_param = end_time_s;
        return ev;
    }

    static SchedulerEvent make_stop_session() {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_STOP_SESSION;
        return ev;
    }

    static SchedulerEvent make_latency_probe() {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_LATENCY_PROBE;
        return ev;
    }

    static SchedulerEvent make_market_sell_all() {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_MARKET_SELL_ALL;
        return ev;
    }

    static SchedulerEvent make_market_sell_down() {
        SchedulerEvent ev;
        ev.source = EventSource::CONTROL;
        ev.kind = SchedulerEventKind::CONTROL_MARKET_SELL_DOWN;
        return ev;
    }
};

// ---------------------------------------------------------------------------
// Execution intent: simulated action produced by StrategyStub.
// In M2 these are "would-be" actions; real execution comes in M4.
// ---------------------------------------------------------------------------
enum class IntentAction : uint8_t {
    WOULD_PLACE_BID = 0,
    WOULD_PLACE_ASK,
    WOULD_CANCEL_BID,
    WOULD_CANCEL_ASK,
    WOULD_CANCEL_ALL,   // exchange-wide cancel (DELETE /cancel-all)
};

struct ExecutionIntent {
    IntentAction action = IntentAction::WOULD_PLACE_BID;
    AssetId asset_id;
    AssetId market_id;               // condition/market ID
    OrderId exchange_order_id;       // for single cancel: exchange-assigned order ID
    OrderId client_order_id;         // correlation ID
    Price_t price = 0;
    Qty_t qty = 0;
    Timestamp_ns created_ts = 0;
    Timestamp_ns recv_ts = 0;     // M7: original event receive timestamp for pipeline latency
    uint32_t intent_id = 0;
    bool neg_risk = false;           // true for neg-risk CTF exchange (multi-outcome markets)
    uint16_t fee_rate_bps = 0;       // maker fee rate from CLOB (part of signed payload)
    OrderType order_type = OrderType::GTC;  // GTC vs FAK etc. for order placement
    uint8_t level = 0;              // quote level index (0 = tightest, for layered quoting)
};

// Fixed-capacity batch of intents (no heap allocation)
struct IntentBatch {
    static constexpr int kMaxIntents = 32;
    ExecutionIntent intents[kMaxIntents];
    int count = 0;

    void add(const ExecutionIntent& intent) {
        if (count < kMaxIntents) {
            intents[count++] = intent;
        }
    }

    void clear() { count = 0; }
};

// Risk gate decision
enum class RiskDecision : uint8_t { ALLOW = 0, DENY };

}  // namespace lt
