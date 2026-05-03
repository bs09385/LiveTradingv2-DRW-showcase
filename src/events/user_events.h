#pragma once

#include <cstdint>
#include <variant>

#include "common/types.h"

namespace lt {

// Order lifecycle status (internal tracked state, not wire enum)
enum class OrderStatus : uint8_t {
    UNKNOWN = 0,
    LIVE,       // placed, waiting for fills
    PARTIAL,    // partially filled
    FILLED,     // fully filled
    CANCELED,   // canceled by user or system
    FAILED,     // order rejected/failed
};

// Trade status from Polymarket: MATCHED -> MINED -> CONFIRMED (or RETRYING -> FAILED)
enum class TradeStatus : uint8_t {
    UNKNOWN = 0,
    MATCHED,
    MINED,
    CONFIRMED,
    RETRYING,
    FAILED,
};

// Wire event type for order messages
enum class OrderEventType : uint8_t {
    PLACEMENT = 0,
    UPDATE,
    CANCELLATION,
};

// Parsed order update from User WS
struct UserOrderUpdate {
    OrderId order_id;
    OrderId client_order_id;    // client-assigned ID for REST correlation (M4)
    AssetId asset_id;
    AssetId market_id;          // condition ID
    OrderEventType event_type = OrderEventType::PLACEMENT;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t size_matched = 0;     // cumulative matched from exchange
    Timestamp_ns exchange_ts = 0;
    bool size_matched_exceeds_original = false;
};

// Maker order entry from trade event's maker_orders array
struct MakerOrderEntry {
    OrderId order_id;
    Qty_t matched_amount = 0;
};
inline constexpr int kMaxMakerOrders = 64;

// Parsed trade update from User WS
struct UserTradeUpdate {
    TradeId trade_id;
    OrderId taker_order_id;
    AssetId asset_id;
    AssetId market_id;
    TradeStatus status = TradeStatus::UNKNOWN;
    Side side = Side::BID;          // YOUR action: BUY=BID, SELL=ASK
    Price_t fill_price = 0;
    Qty_t fill_size = 0;            // top-level "size" (taker's total)
    Timestamp_ns match_ts = 0;
    Timestamp_ns last_update_ts = 0;
    MakerOrderEntry maker_entries[kMaxMakerOrders]{};
    uint8_t maker_entry_count = 0;
};

// Variant of all user event payloads
using UserEvent = std::variant<UserOrderUpdate, UserTradeUpdate>;

// Envelope wrapping a parsed user event with metadata
struct UserMessageEvent {
    UserEvent payload;
    Timestamp_ns recv_ts = 0;
    SeqNum_t seq = 0;
    char server_error_msg[240]{};  // populated on USER_WS_SERVER_ERROR or UNKNOWN_EVENT_TYPE
    uint8_t truncated_fields = 0;  // count of FixedString truncations detected
};

}  // namespace lt
