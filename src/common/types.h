#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace lt {

// 10000x fixed-point price: 1 unit = 0.0001, "0.52" -> 5200
using Price_t = int32_t;
using Qty_t = int64_t;
using TickSize_t = int32_t;
using Timestamp_ns = int64_t;
using SeqNum_t = uint64_t;

// Max asset_id length from Polymarket
inline constexpr std::size_t kMaxAssetIdLen = 128;

// Fixed-size asset ID to avoid heap allocations on hot path
struct AssetId {
    char data[kMaxAssetIdLen]{};
    uint8_t len = 0;

    AssetId() = default;

    explicit AssetId(std::string_view sv) {
        len = static_cast<uint8_t>(std::min(sv.size(), kMaxAssetIdLen - 1));
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const { return {data, len}; }
    std::string str() const { return std::string(data, len); }

    bool operator==(const AssetId& o) const {
        return len == o.len && std::memcmp(data, o.data, len) == 0;
    }
    bool operator!=(const AssetId& o) const { return !(*this == o); }
};

inline std::size_t fnv1a_hash(const char* data, uint8_t len) {
    std::size_t h = 14695981039346656037ULL;
    for (uint8_t i = 0; i < len; ++i) {
        h ^= static_cast<std::size_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct AssetIdHash {
    std::size_t operator()(const AssetId& id) const { return fnv1a_hash(id.data, id.len); }
};

// Fixed-size order ID (Polymarket order hashes: "0x" + 64 hex = 66 chars)
inline constexpr std::size_t kMaxOrderIdLen = 80;

struct OrderId {
    char data[kMaxOrderIdLen]{};
    uint8_t len = 0;

    OrderId() = default;

    explicit OrderId(std::string_view sv) {
        len = static_cast<uint8_t>(std::min(sv.size(), kMaxOrderIdLen - 1));
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const { return {data, len}; }
    std::string str() const { return std::string(data, len); }

    bool operator==(const OrderId& o) const {
        return len == o.len && std::memcmp(data, o.data, len) == 0;
    }
    bool operator!=(const OrderId& o) const { return !(*this == o); }
};

struct OrderIdHash {
    std::size_t operator()(const OrderId& id) const { return fnv1a_hash(id.data, id.len); }
};

// Fixed-size trade ID (Polymarket trade IDs are UUIDs: 36 chars)
inline constexpr std::size_t kMaxTradeIdLen = 48;

struct TradeId {
    char data[kMaxTradeIdLen]{};
    uint8_t len = 0;

    TradeId() = default;

    explicit TradeId(std::string_view sv) {
        len = static_cast<uint8_t>(std::min(sv.size(), kMaxTradeIdLen - 1));
        std::memcpy(data, sv.data(), len);
        data[len] = '\0';
    }

    std::string_view view() const { return {data, len}; }
    std::string str() const { return std::string(data, len); }

    bool operator==(const TradeId& o) const {
        return len == o.len && std::memcmp(data, o.data, len) == 0;
    }
    bool operator!=(const TradeId& o) const { return !(*this == o); }
};

struct TradeIdHash {
    std::size_t operator()(const TradeId& id) const { return fnv1a_hash(id.data, id.len); }
};

enum class Side : uint8_t { BID = 0, ASK = 1 };

// Order type for placement (moved here from exec/exec_intent.h for cross-layer access)
enum class OrderType : uint8_t {
    GTC = 0,    // Good-Till-Cancelled
    GTD = 1,    // Good-Till-Date
    FOK = 2,    // Fill-Or-Kill
    FAK = 3,    // Fill-And-Kill (IOC)
};

// Price ladder constants
inline constexpr Price_t kPriceMin = 0;
inline constexpr Price_t kPriceMax = 10000;      // represents 1.0000
inline constexpr int kLadderSize = 10001;         // indices 0..10000
inline constexpr Price_t kInvalidPrice = -1;
inline constexpr Qty_t kInvalidQty = -1;

// Fixed-point scale factor
inline constexpr int kPriceScale = 10000;

// Quantity scale factor: 10^6 micro-shares
// "30" shares = 30'000'000, "219.217767" = 219'217'767
inline constexpr int64_t kQtyScale = 1'000'000;
inline constexpr int kQtyScaleDigits = 6;

// Convert human-readable whole shares to scaled Qty_t
inline constexpr Qty_t qty_from_int(int64_t shares) { return shares * kQtyScale; }

}  // namespace lt
