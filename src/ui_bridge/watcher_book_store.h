#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

// Full-depth order book storage for watcher markets.
// T6-owned, no SPSC needed. Stores per-token books and builds merged ladders.
//
// Merge mapping rule (Up-centric view):
//   Buy levels  = Up bids + Down asks at complemented price (10000 - P)
//   Sell levels = Up asks + Down bids at complemented price (10000 - P)
//   Same-price levels merged by summing quantities.
class WatcherBookStore {
public:
    WatcherBookStore() = default;

    // Apply a full book snapshot for a token.
    void apply_book_snapshot(const std::string& token_id,
                             const WatcherBookLevel* bids, int bid_count,
                             const WatcherBookLevel* asks, int ask_count);

    // Apply a single price change for a token.
    void apply_price_change(const std::string& token_id,
                            Price_t price, Qty_t new_size, bool is_bid);

    // Clear book for a token.
    void clear_book(const std::string& token_id);

    // Check if we have any data for a token.
    bool has_book(const std::string& token_id) const;

    struct MergedLadder {
        std::vector<WatcherBookLevel> buy_levels;   // sorted descending by price
        std::vector<WatcherBookLevel> sell_levels;   // sorted ascending by price
    };

    // Build a merged ladder for a series (Up + complemented Down).
    // max_depth: max levels per side in output.
    MergedLadder build_merged_ladder(const std::string& token_id_up,
                                      const std::string& token_id_down,
                                      int max_depth) const;

private:
    struct TokenBook {
        std::vector<WatcherBookLevel> bids;  // sorted descending by price
        std::vector<WatcherBookLevel> asks;  // sorted ascending by price
    };

    std::unordered_map<std::string, TokenBook> books_;

    const TokenBook* find_book(const std::string& token_id) const;

    // Merge two level vectors (assumed same sort order) by summing qty at same price.
    static void merge_levels(std::vector<WatcherBookLevel>& dest,
                             const std::vector<WatcherBookLevel>& src);

    // Complement transform: each level's price becomes (10000 - price).
    static std::vector<WatcherBookLevel> complement_levels(
        const std::vector<WatcherBookLevel>& levels);
};

}  // namespace lt
