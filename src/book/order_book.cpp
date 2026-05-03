#include "book/order_book.h"

#include <algorithm>
#include <cstring>

namespace lt {

OrderBook::OrderBook() { clear(); }

void OrderBook::clear() {
    std::memset(bids_.data(), 0, sizeof(bids_));
    std::memset(asks_.data(), 0, sizeof(asks_));
    bbo_ = BBO{};
    status_ = BookStatus::EMPTY;
}

ErrorCode OrderBook::apply_snapshot(const BookSnapshot& snap) {
    clear();

    for (uint16_t i = 0; i < snap.bid_count; ++i) {
        Price_t p = snap.bids[i].price;
        Qty_t s = snap.bids[i].size;
        if (p < kPriceMin || p > kPriceMax) {
            clear();
            return ErrorCode::BOOK_INVALID_PRICE;
        }
        if (s < 0) {
            clear();
            return ErrorCode::BOOK_NEGATIVE_QTY;
        }
        bids_[p] = s;
    }

    for (uint16_t i = 0; i < snap.ask_count; ++i) {
        Price_t p = snap.asks[i].price;
        Qty_t s = snap.asks[i].size;
        if (p < kPriceMin || p > kPriceMax) {
            clear();
            return ErrorCode::BOOK_INVALID_PRICE;
        }
        if (s < 0) {
            clear();
            return ErrorCode::BOOK_NEGATIVE_QTY;
        }
        asks_[p] = s;
    }

    recompute_bbo();
    refresh_status_from_bbo();

    return ErrorCode::OK;
}

ErrorCode OrderBook::apply_price_change(const AssetPriceChange& changes) {
    // Phase 1: Validate all changes before modifying the book.
    // Prevents partial-apply corruption if a later entry is invalid.
    for (uint16_t i = 0; i < changes.change_count; ++i) {
        const auto& ch = changes.changes[i];
        if (ch.price < kPriceMin || ch.price > kPriceMax) return ErrorCode::BOOK_INVALID_PRICE;
        if (ch.size < 0) return ErrorCode::BOOK_NEGATIVE_QTY;
    }

    // Phase 2: Apply all (guaranteed valid)
    bool touches_best = false;
    for (uint16_t i = 0; i < changes.change_count; ++i) {
        const auto& ch = changes.changes[i];
        auto& ladder = (ch.side == Side::BID) ? bids_ : asks_;
        Qty_t old_size = ladder[ch.price];
        ladder[ch.price] = ch.size;

        if (ch.side == Side::BID) {
            if (ch.price >= bbo_.best_bid || (old_size > 0 && ch.size == 0)) {
                touches_best = true;
            }
        } else {
            if (bbo_.best_ask == kInvalidPrice || ch.price <= bbo_.best_ask ||
                (old_size > 0 && ch.size == 0)) {
                touches_best = true;
            }
        }
    }

    if (touches_best) {
        recompute_bbo();
    }
    refresh_status_from_bbo();

    return ErrorCode::OK;
}

void OrderBook::set_tick_size(TickSize_t ts) { tick_size_ = ts; }

Qty_t OrderBook::bid_qty_at(Price_t price) const {
    if (price < kPriceMin || price > kPriceMax) return 0;
    return bids_[price];
}

Qty_t OrderBook::ask_qty_at(Price_t price) const {
    if (price < kPriceMin || price > kPriceMax) return 0;
    return asks_[price];
}

int OrderBook::for_each_bid(const std::function<void(Price_t, Qty_t)>& cb) const {
    int count = 0;
    // Bids: highest price first
    for (int p = kPriceMax; p >= kPriceMin; --p) {
        if (bids_[p] > 0) {
            cb(static_cast<Price_t>(p), bids_[p]);
            ++count;
        }
    }
    return count;
}

int OrderBook::for_each_ask(const std::function<void(Price_t, Qty_t)>& cb) const {
    int count = 0;
    // Asks: lowest price first
    for (int p = kPriceMin; p <= kPriceMax; ++p) {
        if (asks_[p] > 0) {
            cb(static_cast<Price_t>(p), asks_[p]);
            ++count;
        }
    }
    return count;
}

int OrderBook::for_each_bid_n(int max_count, const std::function<void(Price_t, Qty_t)>& cb) const {
    int count = 0;
    for (int p = kPriceMax; p >= kPriceMin && count < max_count; --p) {
        if (bids_[p] > 0) {
            cb(static_cast<Price_t>(p), bids_[p]);
            ++count;
        }
    }
    return count;
}

int OrderBook::for_each_ask_n(int max_count, const std::function<void(Price_t, Qty_t)>& cb) const {
    int count = 0;
    for (int p = kPriceMin; p <= kPriceMax && count < max_count; ++p) {
        if (asks_[p] > 0) {
            cb(static_cast<Price_t>(p), asks_[p]);
            ++count;
        }
    }
    return count;
}

Qty_t OrderBook::total_bid_qty() const {
    Qty_t total = 0;
    for (int p = kPriceMin; p <= kPriceMax; ++p) total += bids_[p];
    return total;
}

Qty_t OrderBook::total_ask_qty() const {
    Qty_t total = 0;
    for (int p = kPriceMin; p <= kPriceMax; ++p) total += asks_[p];
    return total;
}

void OrderBook::recompute_bbo() {
    bbo_.best_bid = kInvalidPrice;
    bbo_.bid_size = 0;
    for (int p = kPriceMax; p >= kPriceMin; --p) {
        if (bids_[p] > 0) {
            bbo_.best_bid = static_cast<Price_t>(p);
            bbo_.bid_size = bids_[p];
            break;
        }
    }

    bbo_.best_ask = kInvalidPrice;
    bbo_.ask_size = 0;
    for (int p = kPriceMin; p <= kPriceMax; ++p) {
        if (asks_[p] > 0) {
            bbo_.best_ask = static_cast<Price_t>(p);
            bbo_.ask_size = asks_[p];
            break;
        }
    }
}

void OrderBook::refresh_status_from_bbo() {
    if (bbo_.valid()) {
        status_ = bbo_.crossed() ? BookStatus::CROSSED : BookStatus::LIVE;
        return;
    }
    if (bbo_.best_bid != kInvalidPrice || bbo_.best_ask != kInvalidPrice) {
        status_ = BookStatus::LIVE;
    } else {
        status_ = BookStatus::EMPTY;
    }
}

}  // namespace lt
