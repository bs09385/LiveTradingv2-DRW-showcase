#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace lt {

// --- BTC Timeframe ---

enum class BtcTimeframe : uint8_t { BTC_5M = 0, BTC_15M };
inline constexpr int kBtcTimeframeCount = 2;

inline const char* timeframe_name(BtcTimeframe tf) {
    switch (tf) {
        case BtcTimeframe::BTC_5M:  return "BTC_5m";
        case BtcTimeframe::BTC_15M: return "BTC_15m";
    }
    return "UNKNOWN";
}

inline BtcTimeframe timeframe_from_index(int idx) {
    return static_cast<BtcTimeframe>(idx);
}

// --- Watcher State (FSM) ---

enum class WatcherState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING,
    CONNECTED,
    STALE,
    ROLL_PENDING,
    ROLLING,
};

inline const char* watcher_state_name(WatcherState s) {
    switch (s) {
        case WatcherState::DISCONNECTED: return "DISCONNECTED";
        case WatcherState::CONNECTING:   return "CONNECTING";
        case WatcherState::CONNECTED:    return "CONNECTED";
        case WatcherState::STALE:        return "STALE";
        case WatcherState::ROLL_PENDING: return "ROLL_PENDING";
        case WatcherState::ROLLING:      return "ROLLING";
    }
    return "UNKNOWN";
}

// --- Series Key ---

struct SeriesKey {
    BtcTimeframe timeframe = BtcTimeframe::BTC_5M;

    bool operator==(const SeriesKey& o) const { return timeframe == o.timeframe; }
    bool operator!=(const SeriesKey& o) const { return !(*this == o); }
};

// --- Series Market Info ---

// Information about a single market instance within a BTC series.
// Uses std::string since this is UI-only / REST-populated (not hot path).
struct SeriesMarketInfo {
    char condition_id[128]{};
    char token_id_up[128]{};
    char token_id_down[128]{};
    bool is_closed = false;

    void set_condition_id(std::string_view sv) {
        auto n = std::min(sv.size(), sizeof(condition_id) - 1);
        std::memcpy(condition_id, sv.data(), n);
        condition_id[n] = '\0';
    }
    void set_token_id_up(std::string_view sv) {
        auto n = std::min(sv.size(), sizeof(token_id_up) - 1);
        std::memcpy(token_id_up, sv.data(), n);
        token_id_up[n] = '\0';
    }
    void set_token_id_down(std::string_view sv) {
        auto n = std::min(sv.size(), sizeof(token_id_down) - 1);
        std::memcpy(token_id_down, sv.data(), n);
        token_id_down[n] = '\0';
    }

    std::string_view condition_id_view() const { return condition_id; }
    std::string_view token_id_up_view() const { return token_id_up; }
    std::string_view token_id_down_view() const { return token_id_down; }

    bool empty() const { return condition_id[0] == '\0'; }
};

// --- Watcher Book Level ---

struct WatcherBookLevel {
    Price_t price = 0;
    Qty_t size = 0;
};

inline constexpr int kMaxWatcherBookDepth = 1024;

}  // namespace lt
