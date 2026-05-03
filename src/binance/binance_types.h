#pragma once

#include <cstdint>

#include "common/types.h"
#include "rtds/rtds_types.h"  // reuse CryptoSymbol

namespace lt {

// Binance Spot WebSocket stream types we parse.
// See: https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams
enum class BinanceStreamType : uint8_t {
    UNKNOWN     = 0,
    BOOK_TICKER = 1,  // <symbol>@bookTicker  (best bid/ask on change)
    TRADE       = 2,  // <symbol>@trade       (individual trades)
    AGG_TRADE   = 3,  // <symbol>@aggTrade    (aggregated trades)
};

// In-band update kind. DATA frames carry parsed market data; CONNECTED /
// DISCONNECTED are sentinel frames pushed by the WS client on transitions
// so the strategy sees connection state changes ordered with the data.
enum class BinanceUpdateKind : uint8_t {
    DATA          = 0,
    CONNECTED     = 1,  // feed transitioned DOWN -> UP
    DISCONNECTED  = 2,  // feed transitioned UP -> DOWN
};

// POD market-data update from Binance, produced by T_binance_md and
// consumed by T2 (scheduler) and T_rec (recorder).
//
// Unified struct for all supported stream types. Consumers dispatch on `kind`
// first (DATA vs sentinel), then on `type` (BinanceStreamType) for DATA frames:
//   - BOOK_TICKER: bid_price/bid_qty/ask_price/ask_qty + update_id
//   - TRADE / AGG_TRADE: last_price/last_qty + update_id + buyer_is_maker
//
// CONNECTED / DISCONNECTED sentinels carry only `kind` and `recv_ts`; all
// other fields are zero.
//
// All numeric values are decoded from Binance's stringified decimals via
// std::from_chars at parse time. Symbol is lowercased to match the
// canonical `btcusdt` form used by RTDS.
struct BinanceMarketUpdate {
    CryptoSymbol symbol;          // lowercase (e.g. "btcusdt"); empty on sentinels
    uint8_t type = 0;             // BinanceStreamType (only for kind == DATA)
    uint8_t kind = 0;             // BinanceUpdateKind
    uint8_t buyer_is_maker = 0;   // 0/1, TRADE/AGG_TRADE only
    uint8_t pad_[4]{};

    double bid_price = 0.0;       // BOOK_TICKER
    double bid_qty = 0.0;
    double ask_price = 0.0;
    double ask_qty = 0.0;

    double last_price = 0.0;      // TRADE / AGG_TRADE
    double last_qty = 0.0;

    int64_t exchange_ts_ms = 0;   // E/T field (milliseconds since epoch)
    int64_t update_id = 0;        // u (bookTicker) / t (trade) / a (aggTrade)

    Timestamp_ns recv_ts = 0;     // local monotonic receive time (ns)
    int64_t recv_wall_ms = 0;     // wall-clock at receive (ms since epoch); 0 if unstamped.
                                  // Used by the strategy_scheduler drain to compute
                                  // exchange-to-recv latency for trade/aggTrade frames.
};

static_assert(std::is_trivially_copyable_v<BinanceMarketUpdate>,
              "BinanceMarketUpdate must be trivially copyable for SPSC transport");

}  // namespace lt
