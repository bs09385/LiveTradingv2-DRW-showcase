#pragma once

#include <string_view>

#include "events/scheduler_events.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

struct UiCommand {
    SchedulerEvent event;
    bool valid = false;
};

// Parse a UI command from JSON. Returns UiCommand with valid=true on success.
// Supported commands:
//   {"cmd":"enable_strategy"}
//   {"cmd":"disable_strategy"}
//   {"cmd":"cancel_all"}
//   {"cmd":"set_mode","mode":"DRY_RUN"|"LIVE"}
//   {"cmd":"start_session","end_time":0}    // 0 = indefinite
//   {"cmd":"stop_session"}
//   {"cmd":"inventory_split","condition_id":"...","shares":25}
//   {"cmd":"inventory_merge","condition_id":"...","shares":25}
//   {"cmd":"inventory_redeem","condition_id":"...","token_id":"...","shares":0}
UiCommand parse_ui_command(std::string_view json);

// --- Watcher commands (BTC ladder watch feature) ---

struct WatchCommand {
    enum class Type : uint8_t { SUBSCRIBE, UNSUBSCRIBE, REQUEST_LIST };
    Type type = Type::REQUEST_LIST;
    BtcTimeframe timeframe = BtcTimeframe::BTC_5M;
    bool valid = false;
};

// Parse a watcher command from JSON. Returns WatchCommand with valid=true on success.
// Supported commands:
//   {"cmd":"watch_subscribe","series_key":"BTC_5m"|"BTC_15m"|"BTC_1h"|"BTC_4h"}
//   {"cmd":"watch_unsubscribe","series_key":"BTC_5m"|"BTC_15m"|"BTC_1h"|"BTC_4h"}
//   {"cmd":"request_series_list"}
WatchCommand parse_watch_command(std::string_view json);

// Returns true if the JSON cmd field is a watcher command (watch_* or request_series_list).
bool is_watch_command(std::string_view json);

}  // namespace lt
