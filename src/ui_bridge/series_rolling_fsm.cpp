#include "ui_bridge/series_rolling_fsm.h"

namespace lt {

bool SeriesRollingFsm::on_subscribe() {
    if (state_ != WatcherState::DISCONNECTED) return false;
    state_ = WatcherState::CONNECTING;
    return true;
}

bool SeriesRollingFsm::on_unsubscribe() {
    if (state_ == WatcherState::DISCONNECTED) return false;
    state_ = WatcherState::DISCONNECTED;
    return true;
}

bool SeriesRollingFsm::on_ws_connected() {
    if (state_ != WatcherState::CONNECTING) return false;
    state_ = WatcherState::CONNECTED;
    return true;
}

bool SeriesRollingFsm::on_ws_stale() {
    if (state_ != WatcherState::CONNECTED) return false;
    state_ = WatcherState::STALE;
    return true;
}

bool SeriesRollingFsm::on_ws_recovered() {
    if (state_ != WatcherState::STALE) return false;
    state_ = WatcherState::CONNECTED;
    return true;
}

bool SeriesRollingFsm::on_market_closed(bool next_exists) {
    if (state_ != WatcherState::CONNECTED && state_ != WatcherState::STALE)
        return false;
    state_ = next_exists ? WatcherState::ROLLING : WatcherState::ROLL_PENDING;
    return true;
}

bool SeriesRollingFsm::on_next_discovered() {
    if (state_ != WatcherState::ROLL_PENDING) return false;
    state_ = WatcherState::ROLLING;
    return true;
}

bool SeriesRollingFsm::on_switch_complete() {
    if (state_ != WatcherState::ROLLING) return false;
    state_ = WatcherState::CONNECTING;
    return true;
}

}  // namespace lt
