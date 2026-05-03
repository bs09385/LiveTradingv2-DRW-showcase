#pragma once

#include "common/types.h"

namespace lt {

struct BBO {
    Price_t best_bid = kInvalidPrice;
    Price_t best_ask = kInvalidPrice;
    Qty_t bid_size = 0;
    Qty_t ask_size = 0;

    bool valid() const { return best_bid != kInvalidPrice && best_ask != kInvalidPrice; }
    Price_t spread() const { return valid() ? (best_ask - best_bid) : 0; }
    bool crossed() const { return valid() && best_bid >= best_ask; }
};

enum class BookStatus : uint8_t {
    EMPTY = 0,
    LIVE,
    STALE,
    CROSSED,
};

}  // namespace lt
