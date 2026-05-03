#pragma once

#include <memory>

#include "binance/binance_types.h"
#include "common/error.h"

namespace lt {

// Parser for Binance Spot WebSocket stream frames.
//
// Supports both combined-stream envelopes
//   {"stream":"btcusdt@bookTicker","data":{...}}
// and raw single-stream payloads (the data object directly).
//
// Stream types decoded:
//   bookTicker -> BinanceStreamType::BOOK_TICKER
//   trade      -> BinanceStreamType::TRADE
//   aggTrade   -> BinanceStreamType::AGG_TRADE
//
// Return values:
//   OK                  -- market-data update written to `out`
//   UNKNOWN_EVENT_TYPE  -- subscribe/error reply, unknown stream, or
//                          unrecognized shape (non-fatal; caller may ignore)
//   JSON_PARSE_ERROR    -- malformed JSON or required numeric field missing
//
// Thread ownership: T_binance_md (same thread as BinanceWsClient).
// Uses a pre-allocated simdjson parser internally.
class BinanceMessageParser {
public:
    BinanceMessageParser();
    ~BinanceMessageParser();

    BinanceMessageParser(const BinanceMessageParser&) = delete;
    BinanceMessageParser& operator=(const BinanceMessageParser&) = delete;

    ErrorCode parse(std::string_view payload, Timestamp_ns recv_ts,
                    BinanceMarketUpdate& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
