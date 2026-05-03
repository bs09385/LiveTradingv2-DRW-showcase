#include "ui_bridge/watcher_book_store.h"

#include <algorithm>

namespace lt {

void WatcherBookStore::apply_book_snapshot(const std::string& token_id,
                                            const WatcherBookLevel* bids, int bid_count,
                                            const WatcherBookLevel* asks, int ask_count) {
    auto& book = books_[token_id];

    book.bids.assign(bids, bids + bid_count);
    book.asks.assign(asks, asks + ask_count);

    // Ensure sort invariants
    std::sort(book.bids.begin(), book.bids.end(),
              [](const WatcherBookLevel& a, const WatcherBookLevel& b) {
                  return a.price > b.price;  // descending
              });
    std::sort(book.asks.begin(), book.asks.end(),
              [](const WatcherBookLevel& a, const WatcherBookLevel& b) {
                  return a.price < b.price;  // ascending
              });
}

void WatcherBookStore::apply_price_change(const std::string& token_id,
                                           Price_t price, Qty_t new_size,
                                           bool is_bid) {
    auto& book = books_[token_id];
    auto& side = is_bid ? book.bids : book.asks;

    // Find existing level
    auto it = std::find_if(side.begin(), side.end(),
                           [price](const WatcherBookLevel& l) {
                               return l.price == price;
                           });

    if (new_size == 0) {
        // Remove level
        if (it != side.end()) {
            side.erase(it);
        }
    } else if (it != side.end()) {
        // Update existing
        it->size = new_size;
    } else {
        // Insert new level maintaining sort order
        WatcherBookLevel level{price, new_size};
        if (is_bid) {
            auto pos = std::lower_bound(side.begin(), side.end(), level,
                                         [](const WatcherBookLevel& a,
                                            const WatcherBookLevel& b) {
                                             return a.price > b.price;
                                         });
            side.insert(pos, level);
        } else {
            auto pos = std::lower_bound(side.begin(), side.end(), level,
                                         [](const WatcherBookLevel& a,
                                            const WatcherBookLevel& b) {
                                             return a.price < b.price;
                                         });
            side.insert(pos, level);
        }
    }
}

void WatcherBookStore::clear_book(const std::string& token_id) {
    books_.erase(token_id);
}

bool WatcherBookStore::has_book(const std::string& token_id) const {
    return books_.count(token_id) > 0;
}

const WatcherBookStore::TokenBook* WatcherBookStore::find_book(
    const std::string& token_id) const {
    auto it = books_.find(token_id);
    return it != books_.end() ? &it->second : nullptr;
}

std::vector<WatcherBookLevel> WatcherBookStore::complement_levels(
    const std::vector<WatcherBookLevel>& levels) {
    std::vector<WatcherBookLevel> result;
    result.reserve(levels.size());
    for (const auto& l : levels) {
        Price_t comp = kPriceMax - l.price;
        if (comp >= kPriceMin && comp <= kPriceMax) {
            result.push_back({comp, l.size});
        }
    }
    return result;
}

void WatcherBookStore::merge_levels(std::vector<WatcherBookLevel>& dest,
                                     const std::vector<WatcherBookLevel>& src) {
    for (const auto& s : src) {
        auto it = std::find_if(dest.begin(), dest.end(),
                               [&s](const WatcherBookLevel& d) {
                                   return d.price == s.price;
                               });
        if (it != dest.end()) {
            it->size += s.size;
        } else {
            dest.push_back(s);
        }
    }
}

WatcherBookStore::MergedLadder WatcherBookStore::build_merged_ladder(
    const std::string& token_id_up,
    const std::string& token_id_down,
    int max_depth) const {

    MergedLadder ladder;

    const auto* up_book = find_book(token_id_up);
    const auto* down_book = find_book(token_id_down);

    // Buy levels = Up bids + complement(Down asks)
    if (up_book) {
        ladder.buy_levels = up_book->bids;
    }
    if (down_book) {
        auto comp_asks = complement_levels(down_book->asks);
        merge_levels(ladder.buy_levels, comp_asks);
    }

    // Sell levels = Up asks + complement(Down bids)
    if (up_book) {
        ladder.sell_levels = up_book->asks;
    }
    if (down_book) {
        auto comp_bids = complement_levels(down_book->bids);
        merge_levels(ladder.sell_levels, comp_bids);
    }

    // Sort: buy descending, sell ascending
    std::sort(ladder.buy_levels.begin(), ladder.buy_levels.end(),
              [](const WatcherBookLevel& a, const WatcherBookLevel& b) {
                  return a.price > b.price;
              });
    std::sort(ladder.sell_levels.begin(), ladder.sell_levels.end(),
              [](const WatcherBookLevel& a, const WatcherBookLevel& b) {
                  return a.price < b.price;
              });

    // Truncate to max_depth
    if (max_depth > 0) {
        if (static_cast<int>(ladder.buy_levels.size()) > max_depth) {
            ladder.buy_levels.resize(max_depth);
        }
        if (static_cast<int>(ladder.sell_levels.size()) > max_depth) {
            ladder.sell_levels.resize(max_depth);
        }
    }

    return ladder;
}

}  // namespace lt
