#pragma once

#include <string>
#include <string_view>

#include "common/error.h"
#include "common/types.h"
#include "events/event_variant.h"
#include "simdjson.h"

namespace lt {

class MarketMessageParser {
public:
    MarketMessageParser();

    // HOT PATH: Parse a raw WS payload into a MarketEvent
    // Returns ErrorCode::PONG_MESSAGE for "PONG" strings (not an error)
    ErrorCode parse(std::string_view payload, Timestamp_ns recv_ts, SeqNum_t seq,
                    MarketEvent& out);

private:
    simdjson::ondemand::parser parser_;
    // Reused parse buffer to reduce steady-state allocations.
    std::string scratch_;

    ErrorCode parse_book(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_price_change(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_best_bid_ask(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_tick_size_change(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_last_trade_price(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_new_market(simdjson::ondemand::document& doc, MarketEvent& out);
    ErrorCode parse_market_resolved(simdjson::ondemand::document& doc, MarketEvent& out);
};

}  // namespace lt
