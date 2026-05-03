#pragma once

#include <variant>

#include "common/types.h"
#include "events/market_events.h"

namespace lt {

// Variant of all possible market event payloads
using MarketEventPayload = std::variant<BookSnapshot, PriceChangeEvent, BestBidAskEvent,
                                        TickSizeChangeEvent, LastTradePriceEvent,
                                        NewMarketEvent, MarketResolvedEvent>;

// Envelope wrapping any market event with metadata
struct MarketEvent {
    MarketEventPayload payload;
    Timestamp_ns recv_ts = 0;  // local monotonic timestamp when WS frame was received
    SeqNum_t seq = 0;          // local sequence number
};

// Notification kind for compact queue messages
enum class NotificationKind : uint8_t {
    BOOK_SNAPSHOT = 0,
    PRICE_CHANGE,
    BBO_UPDATE,
    TICK_SIZE_CHANGE,
    LAST_TRADE,
    NEW_MARKET,
    MARKET_RESOLVED,
};

// BBO snapshot for notification
struct BBOSnapshot {
    Price_t best_bid = kInvalidPrice;
    Price_t best_ask = kInvalidPrice;
    Qty_t bid_size = 0;
    Qty_t ask_size = 0;
};

// Compact notification pushed through SPSC queue to strategy scheduler
struct MarketNotification {
    NotificationKind kind = NotificationKind::BOOK_SNAPSHOT;
    AssetId asset_id;
    Timestamp_ns recv_ts = 0;
    SeqNum_t seq = 0;
    BBOSnapshot bbo;
    Timestamp_ns exchange_ts = 0;  // exchange timestamp (ms since epoch), 0 if absent
    TickSize_t tick_size = 0;  // populated for TICK_SIZE_CHANGE notifications
    AssetId resolved_winning_asset_id;  // populated for MARKET_RESOLVED
};

}  // namespace lt
