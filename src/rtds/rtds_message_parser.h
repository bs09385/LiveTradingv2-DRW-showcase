#pragma once

#include <memory>

#include "common/error.h"
#include "rtds/rtds_types.h"

namespace lt {

// Parses RTDS (Real-Time Data Socket) messages from Polymarket.
//
// Expected message format:
// {
//   "topic": "crypto_prices",
//   "type": "update",
//   "timestamp": 1753314088421,
//   "payload": {
//     "symbol": "btcusdt",
//     "timestamp": 1753314088395,
//     "value": 67234.50
//   }
// }
//
// Thread ownership: T_rtds (same thread as RTDS WS client).
// Uses pre-allocated simdjson parser internally.
class RtdsMessageParser {
public:
    RtdsMessageParser();
    ~RtdsMessageParser();

    RtdsMessageParser(const RtdsMessageParser&) = delete;
    RtdsMessageParser& operator=(const RtdsMessageParser&) = delete;

    // Parse a raw RTDS WebSocket frame into a CryptoPriceUpdate.
    // Returns OK on success, PONG_MESSAGE for pong frames,
    // UNKNOWN_EVENT_TYPE for non-crypto_prices messages,
    // PARSE_ERROR for malformed JSON.
    ErrorCode parse(std::string_view payload, Timestamp_ns recv_ts,
                    CryptoPriceUpdate& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
