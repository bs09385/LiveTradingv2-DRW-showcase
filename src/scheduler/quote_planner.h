#pragma once

#include <cstdint>
#include <unordered_map>

#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "events/scheduler_events.h"
#include "logger/metrics.h"

namespace lt {

// Round price down to nearest tick boundary (conservative for BUY).
inline Price_t round_down_to_tick(Price_t price, TickSize_t tick) {
    if (tick <= 1) return price;
    return (price / tick) * tick;
}

// Round price up to nearest tick boundary (conservative for SELL).
inline Price_t round_up_to_tick(Price_t price, TickSize_t tick) {
    if (tick <= 1) return price;
    return ((price + tick - 1) / tick) * tick;
}

// QuotePlanner currently preserves strategy intents exactly (pass-through).
// It remains the central place for future intent-level planning logic.
class QuotePlanner {
public:
    QuotePlanner(const MarketPairRegistry* market_pairs = nullptr,
                 const InventoryView* inventory = nullptr,
                 Metrics* metrics = nullptr)
        : market_pairs_(market_pairs), inventory_(inventory), metrics_(metrics) {
        tick_sizes_.reserve(32);
    }

    IntentBatch plan(const ExecutionIntent& intent);

    void set_market_pairs(const MarketPairRegistry* market_pairs) { market_pairs_ = market_pairs; }
    void set_inventory(const InventoryView* inventory) { inventory_ = inventory; }
    void set_metrics(Metrics* metrics) { metrics_ = metrics; }

    // Per-token tick size (updated from market data events)
    void set_tick_size(const AssetId& token_id, TickSize_t tick) {
        tick_sizes_[token_id] = tick;
    }

    TickSize_t tick_size_for(const AssetId& token_id) const {
        auto it = tick_sizes_.find(token_id);
        if (it != tick_sizes_.end()) return it->second;
        return default_tick_size_;
    }

    void set_default_tick_size(TickSize_t tick) { default_tick_size_ = tick; }
    TickSize_t default_tick_size() const { return default_tick_size_; }

private:
    const MarketPairRegistry* market_pairs_ = nullptr;
    const InventoryView* inventory_ = nullptr;
    Metrics* metrics_ = nullptr;

    // Default tick size: 100 = 0.01 in 10000x fixed-point (common Polymarket tick)
    TickSize_t default_tick_size_ = 100;
    std::unordered_map<AssetId, TickSize_t, AssetIdHash> tick_sizes_;
};

}  // namespace lt
