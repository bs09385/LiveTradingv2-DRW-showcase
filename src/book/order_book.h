#pragma once

#include <array>
#include <cstdint>
#include <functional>

#include "book/book_types.h"
#include "common/error.h"
#include "common/types.h"
#include "events/market_events.h"

namespace lt {

class OrderBook {
public:
    OrderBook();

    // Apply a full book snapshot: clears everything, populates from snapshot
    ErrorCode apply_snapshot(const BookSnapshot& snap);

    // Apply incremental price changes for this asset
    ErrorCode apply_price_change(const AssetPriceChange& changes);

    // Update tick size (just stores it, no rebuild needed)
    void set_tick_size(TickSize_t ts);

    // Accessors
    const BBO& bbo() const { return bbo_; }
    BookStatus status() const { return status_; }
    TickSize_t tick_size() const { return tick_size_; }

    // Set book status (e.g. STALE on market resolution)
    void set_status(BookStatus s) { status_ = s; }

    // Get quantity at a specific price level
    Qty_t bid_qty_at(Price_t price) const;
    Qty_t ask_qty_at(Price_t price) const;

    // Iterate non-zero levels (callback: price, qty). Returns count visited.
    int for_each_bid(const std::function<void(Price_t, Qty_t)>& cb) const;
    int for_each_ask(const std::function<void(Price_t, Qty_t)>& cb) const;

    // Top-N variants with early break after max_count levels.
    int for_each_bid_n(int max_count, const std::function<void(Price_t, Qty_t)>& cb) const;
    int for_each_ask_n(int max_count, const std::function<void(Price_t, Qty_t)>& cb) const;

    // Total quantity across all levels
    Qty_t total_bid_qty() const;
    Qty_t total_ask_qty() const;

    // Clear the entire book
    void clear();

private:
    std::array<Qty_t, kLadderSize> bids_{};
    std::array<Qty_t, kLadderSize> asks_{};
    BBO bbo_;
    BookStatus status_ = BookStatus::EMPTY;
    TickSize_t tick_size_ = 100;  // default 0.01

    void recompute_bbo();
    void refresh_status_from_bbo();
};

}  // namespace lt
