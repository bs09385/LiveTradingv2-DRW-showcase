#pragma once

#include "ui_bridge/watcher_types.h"

namespace lt {

// Tracks current and next market instances per BTC timeframe.
// Updated by REST discovery polling on T_watch, consumed by T6.
// Not thread-safe: must be accessed from a single thread (T6).
class BtcSeriesRegistry {
public:
    BtcSeriesRegistry() = default;

    // Update discovery results for a timeframe.
    // next may be nullptr if no next market has been discovered.
    void update_from_discovery(BtcTimeframe tf,
                               const SeriesMarketInfo& current,
                               const SeriesMarketInfo* next);

    // Get current market info for a timeframe (null if none).
    const SeriesMarketInfo* current(BtcTimeframe tf) const;

    // Get next market info for a timeframe (null if none).
    const SeriesMarketInfo* next(BtcTimeframe tf) const;

    // Whether a next market has been discovered.
    bool has_next(BtcTimeframe tf) const;

    // Promote next to current. Clears next.
    void promote_next(BtcTimeframe tf);

    // Mark current market as closed.
    void mark_closed(BtcTimeframe tf);

    // Clear all state for a timeframe.
    void clear(BtcTimeframe tf);

    // Check if a timeframe has any known market.
    bool has_current(BtcTimeframe tf) const;

private:
    struct Entry {
        SeriesMarketInfo current{};
        SeriesMarketInfo next_market{};
        bool has_current = false;
        bool has_next_market = false;
    };

    Entry entries_[kBtcTimeframeCount]{};

    int idx(BtcTimeframe tf) const { return static_cast<int>(tf); }
};

}  // namespace lt
