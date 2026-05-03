#pragma once

#include "ui_bridge/watcher_types.h"

namespace lt {

// Per-series state machine for BTC ladder watcher.
// Pure logic, no I/O. All transitions are explicit method calls.
//
// State transitions:
//   DISCONNECTED --subscribe-->        CONNECTING
//   CONNECTING   --ws_connected-->     CONNECTED
//   CONNECTED    --ws_stale-->         STALE
//   STALE        --ws_recovered-->     CONNECTED
//   CONNECTED    --market_closed + next_exists-->   ROLLING
//   CONNECTED    --market_closed + !next_exists-->  ROLL_PENDING
//   ROLL_PENDING --next_discovered-->  ROLLING
//   ROLLING      --switch_complete-->  CONNECTING
//   any          --unsubscribe-->      DISCONNECTED
class SeriesRollingFsm {
public:
    SeriesRollingFsm() = default;

    WatcherState state() const { return state_; }

    // --- Input events ---

    // User subscribes to this series
    bool on_subscribe();

    // User unsubscribes from this series
    bool on_unsubscribe();

    // WS connection established for this series' tokens
    bool on_ws_connected();

    // WS data went stale (no updates within threshold)
    bool on_ws_stale();

    // WS data recovered after being stale
    bool on_ws_recovered();

    // Market confirmed closed via REST + staleness.
    // next_exists: whether a next market instance has been discovered.
    // Returns true if state changed.
    bool on_market_closed(bool next_exists);

    // A next market instance has been discovered while in ROLL_PENDING.
    bool on_next_discovered();

    // WS reconnected to the new market's tokens after rolling.
    bool on_switch_complete();

    // --- Queries ---

    bool is_active() const { return state_ != WatcherState::DISCONNECTED; }
    bool needs_ws_connection() const {
        return state_ == WatcherState::CONNECTING;
    }
    bool is_rolling() const { return state_ == WatcherState::ROLLING; }
    bool is_waiting_for_next() const { return state_ == WatcherState::ROLL_PENDING; }

private:
    WatcherState state_ = WatcherState::DISCONNECTED;
};

}  // namespace lt
