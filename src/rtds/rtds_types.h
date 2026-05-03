#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include "common/types.h"

namespace lt {

// Fixed-size crypto symbol for hot path (e.g. "btcusdt", "ethusdt")
inline constexpr std::size_t kMaxCryptoSymbolLen = 24;

struct CryptoSymbol {
    char data[kMaxCryptoSymbolLen]{};
    uint8_t len = 0;

    CryptoSymbol() = default;

    explicit CryptoSymbol(std::string_view sv) {
        len = static_cast<uint8_t>(std::min(sv.size(), kMaxCryptoSymbolLen - 1));
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const { return {data, len}; }

    bool operator==(const CryptoSymbol& o) const {
        return len == o.len && std::memcmp(data, o.data, len) == 0;
    }
    bool operator!=(const CryptoSymbol& o) const { return !(*this == o); }
};

// POD crypto price update from RTDS WebSocket.
// Produced by T_rtds, consumed by T2 (scheduler) and T_rec (recorder).
struct CryptoPriceUpdate {
    CryptoSymbol symbol;
    double value = 0.0;
    int64_t exchange_ts_ms = 0;   // exchange timestamp (milliseconds since epoch)
    Timestamp_ns recv_ts = 0;     // local monotonic receive timestamp (nanoseconds)
};

}  // namespace lt
