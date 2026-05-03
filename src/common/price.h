#pragma once

#include <string_view>

#include "common/error.h"
#include "common/types.h"

namespace lt {

// Parse a decimal price string (e.g. "0.52", ".48", "0.001") to 10000x fixed-point
Expected<Price_t> parse_price(std::string_view sv);

// Parse a quantity string (e.g. "30", "100") to int64
Expected<Qty_t> parse_qty(std::string_view sv);

// Parse side string ("BUY"/"SELL" or "BID"/"ASK") to Side enum
Expected<Side> parse_side(std::string_view sv);

// Format a 10000x fixed-point price back to decimal string
std::string format_price(Price_t price);

// Format a kQtyScale-scaled quantity back to decimal string
// e.g. 219217767 → "219.217767", 30000000 → "30"
std::string format_qty(Qty_t qty);

// Check if a price is on the given tick size (in 10000x units)
bool is_on_tick(Price_t price, TickSize_t tick_size);

}  // namespace lt
