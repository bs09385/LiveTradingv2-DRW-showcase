#pragma once

#include <array>
#include <cstdint>

#include "common/types.h"

namespace lt {

// Single price level
struct PriceLevel {
    Price_t price = 0;
    Qty_t size = 0;
};

// Full order book snapshot (event_type: "book")
struct BookSnapshot {
    AssetId asset_id;
    AssetId market_id;          // condition_id from "market" field
    char hash[80]{};            // book hash
    uint8_t hash_len = 0;
    std::array<PriceLevel, 1024> bids{};
    std::array<PriceLevel, 1024> asks{};
    uint16_t bid_count = 0;
    uint16_t ask_count = 0;
    Timestamp_ns exchange_ts = 0;
};

// A single price change within a price_change event
struct PriceChange {
    Price_t price = 0;
    Side side = Side::BID;
    Qty_t size = 0;
};

// Price change event for a single asset within a batch
struct AssetPriceChange {
    AssetId asset_id;
    Price_t best_bid = kInvalidPrice;
    Price_t best_ask = kInvalidPrice;
    std::array<PriceChange, 512> changes{};
    uint16_t change_count = 0;
};

// Incremental delta (event_type: "price_change")
struct PriceChangeEvent {
    std::array<AssetPriceChange, 32> asset_changes{};
    uint16_t asset_count = 0;
    Timestamp_ns exchange_ts = 0;
};

// Informational BBO update (event_type: "best_bid_ask")
struct BestBidAskEvent {
    AssetId asset_id;
    Price_t best_bid = kInvalidPrice;
    Price_t best_ask = kInvalidPrice;
    Price_t spread = 0;
    Timestamp_ns exchange_ts = 0;
};

// Tick size change (event_type: "tick_size_change")
struct TickSizeChangeEvent {
    AssetId asset_id;
    TickSize_t old_tick_size = 0;
    TickSize_t new_tick_size = 0;
    Timestamp_ns exchange_ts = 0;
};

// Last trade price (event_type: "last_trade_price")
struct LastTradePriceEvent {
    AssetId asset_id;
    AssetId market_id;          // condition_id from "market" field
    Price_t price = 0;
    Qty_t size = 0;
    Side side = Side::BID;
    uint16_t fee_rate_bps = 0;  // fee rate from API
    Timestamp_ns exchange_ts = 0;
};

// New market event (event_type: "new_market")
struct NewMarketEvent {
    AssetId market_id;              // condition_id ("market" field)
    AssetId assets[2];              // the two token IDs
    uint8_t asset_count = 0;
    char outcomes[2][32]{};         // "Yes"/"No" or "Up"/"Down"
    uint8_t outcome_count = 0;
    Timestamp_ns exchange_ts = 0;
};

// Market resolved event (event_type: "market_resolved")
struct MarketResolvedEvent {
    AssetId market_id;              // condition_id
    AssetId winning_asset_id;       // which token won
    char winning_outcome[32]{};     // "Yes"/"No" etc.
    uint8_t winning_outcome_len = 0;
    AssetId assets[2];              // the two token IDs (for cleanup)
    uint8_t asset_count = 0;
    Timestamp_ns exchange_ts = 0;
};

}  // namespace lt
