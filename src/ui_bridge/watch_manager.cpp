#include "ui_bridge/watch_manager.h"

#include <cstring>

namespace lt {

bool WatchManager::subscribe(BtcTimeframe tf) {
    auto& e = entries_[idx(tf)];
    e.watchers_count++;
    if (e.watchers_count == 1) {
        e.fsm.on_subscribe();
        e.last_book_update_ns = 0;
        return true;
    }
    return false;  // already subscribed, just incremented ref count
}

bool WatchManager::unsubscribe(BtcTimeframe tf) {
    auto& e = entries_[idx(tf)];
    if (e.watchers_count <= 0) return false;
    e.watchers_count--;
    if (e.watchers_count == 0) {
        e.fsm.on_unsubscribe();
        e.last_book_update_ns = 0;
        return true;
    }
    return false;  // still has other subscribers
}

bool WatchManager::is_subscribed(BtcTimeframe tf) const {
    return entries_[idx(tf)].watchers_count > 0;
}

int WatchManager::watcher_count(BtcTimeframe tf) const {
    return entries_[idx(tf)].watchers_count;
}

void WatchManager::tick(Timestamp_ns now, Timestamp_ns stale_threshold_ns) {
    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        auto& e = entries_[i];
        if (e.watchers_count <= 0) continue;

        auto state = e.fsm.state();
        if (state == WatcherState::CONNECTED && e.last_book_update_ns > 0) {
            auto elapsed = now - e.last_book_update_ns;
            if (elapsed > stale_threshold_ns) {
                e.fsm.on_ws_stale();
            }
        } else if (state == WatcherState::STALE && e.last_book_update_ns > 0) {
            auto elapsed = now - e.last_book_update_ns;
            if (elapsed <= stale_threshold_ns) {
                e.fsm.on_ws_recovered();
            }
        }
    }
}

void WatchManager::on_book_data_received(BtcTimeframe tf, Timestamp_ns now) {
    entries_[idx(tf)].last_book_update_ns = now;
}

Timestamp_ns WatchManager::last_book_update(BtcTimeframe tf) const {
    return entries_[idx(tf)].last_book_update_ns;
}

std::vector<std::string> WatchManager::active_asset_ids(
    const BtcSeriesRegistry& registry) const {
    std::vector<std::string> ids;
    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        if (entries_[i].watchers_count <= 0) continue;
        auto tf = static_cast<BtcTimeframe>(i);
        const auto* info = registry.current(tf);
        if (!info || info->empty()) continue;
        if (info->token_id_up[0] != '\0') {
            ids.emplace_back(info->token_id_up);
        }
        if (info->token_id_down[0] != '\0') {
            ids.emplace_back(info->token_id_down);
        }
    }
    return ids;
}

std::vector<BtcTimeframe> WatchManager::subscribed_timeframes() const {
    std::vector<BtcTimeframe> result;
    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        if (entries_[i].watchers_count > 0) {
            result.push_back(static_cast<BtcTimeframe>(i));
        }
    }
    return result;
}

int WatchManager::active_count() const {
    int count = 0;
    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        if (entries_[i].watchers_count > 0) ++count;
    }
    return count;
}

}  // namespace lt
