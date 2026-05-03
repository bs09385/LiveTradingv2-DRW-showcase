#pragma once

#include <cstdint>

#include "common/discovery.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

// Slot naming: 6 slots total (current/next/previous for each of 5M and 15M)
enum class SlotName : uint8_t {
    CURRENT_5M  = 0,
    NEXT_5M     = 1,
    PREVIOUS_5M = 2,
    CURRENT_15M = 3,
    NEXT_15M    = 4,
    PREVIOUS_15M = 5
};
constexpr int kSlotCount = 6;
constexpr int kActiveSlotCount = 4;  // excludes PREVIOUS slots

inline const char* slot_name_str(SlotName s) {
    switch (s) {
        case SlotName::CURRENT_5M:   return "CURRENT_5M";
        case SlotName::NEXT_5M:      return "NEXT_5M";
        case SlotName::PREVIOUS_5M:  return "PREVIOUS_5M";
        case SlotName::CURRENT_15M:  return "CURRENT_15M";
        case SlotName::NEXT_15M:     return "NEXT_15M";
        case SlotName::PREVIOUS_15M: return "PREVIOUS_15M";
    }
    return "UNKNOWN";
}

inline bool is_current_slot(SlotName s) {
    return s == SlotName::CURRENT_5M || s == SlotName::CURRENT_15M;
}

inline bool is_previous_slot(SlotName s) {
    return s == SlotName::PREVIOUS_5M || s == SlotName::PREVIOUS_15M;
}

inline BtcTimeframe slot_timeframe(SlotName s) {
    switch (s) {
        case SlotName::CURRENT_5M:
        case SlotName::NEXT_5M:
        case SlotName::PREVIOUS_5M:
            return BtcTimeframe::BTC_5M;
        case SlotName::CURRENT_15M:
        case SlotName::NEXT_15M:
        case SlotName::PREVIOUS_15M:
            return BtcTimeframe::BTC_15M;
    }
    return BtcTimeframe::BTC_5M;
}

inline SlotName current_slot_for(BtcTimeframe tf) {
    return tf == BtcTimeframe::BTC_5M ? SlotName::CURRENT_5M : SlotName::CURRENT_15M;
}

inline SlotName next_slot_for(BtcTimeframe tf) {
    return tf == BtcTimeframe::BTC_5M ? SlotName::NEXT_5M : SlotName::NEXT_15M;
}

inline SlotName previous_slot_for(BtcTimeframe tf) {
    return tf == BtcTimeframe::BTC_5M ? SlotName::PREVIOUS_5M : SlotName::PREVIOUS_15M;
}

// Slot lifecycle phases
enum class SlotPhase : uint8_t {
    EMPTY       = 0,  // No market assigned
    DISCOVERING = 1,  // Discovery in progress
    REGISTERED  = 2,  // Market registered in stores, not yet subscribed
    SUBSCRIBING = 3,  // WS subscribe sent, waiting for first data
    ACTIVE      = 4,  // Receiving data, eligible for quoting (CURRENT only)
    CLOSING     = 5,  // Window ended, cancels sent, waiting for demotion
    PREVIOUS    = 6,  // Demoted to PREVIOUS — positions retained, no orderbook
};

inline const char* slot_phase_str(SlotPhase p) {
    switch (p) {
        case SlotPhase::EMPTY:       return "EMPTY";
        case SlotPhase::DISCOVERING: return "DISCOVERING";
        case SlotPhase::REGISTERED:  return "REGISTERED";
        case SlotPhase::SUBSCRIBING: return "SUBSCRIBING";
        case SlotPhase::ACTIVE:      return "ACTIVE";
        case SlotPhase::CLOSING:     return "CLOSING";
        case SlotPhase::PREVIOUS:    return "PREVIOUS";
    }
    return "UNKNOWN";
}

// Full slot state
struct MarketSlot {
    SlotName name = SlotName::CURRENT_5M;
    SlotPhase phase = SlotPhase::EMPTY;
    DiscoveredMarket market;
    BtcTimeframe timeframe = BtcTimeframe::BTC_5M;
    int64_t window_start_unix_s = 0;
    int64_t window_end_unix_s = 0;

    void clear() {
        phase = SlotPhase::EMPTY;
        market = {};
        window_start_unix_s = 0;
        window_end_unix_s = 0;
    }
};

// Lightweight state retained for PREVIOUS slots (no orderbook)
struct PreviousMarketState {
    DiscoveredMarket market;
    int64_t window_start_unix_s = 0;
    int64_t window_end_unix_s = 0;
    bool awaiting_resolved = true;   // still waiting for market_resolved
    bool awaiting_redeem = false;    // resolved, waiting for redeem
    int64_t redeem_fire_time_s = 0;  // wall-clock time to fire redeem

    void clear() {
        market = {};
        window_start_unix_s = 0;
        window_end_unix_s = 0;
        awaiting_resolved = false;
        awaiting_redeem = false;
        redeem_fire_time_s = 0;
    }
};

}  // namespace lt
