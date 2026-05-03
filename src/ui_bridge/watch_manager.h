#pragma once

#include <string>
#include <vector>

#include "common/types.h"
#include "ui_bridge/btc_series_registry.h"
#include "ui_bridge/series_rolling_fsm.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

// Coordinates watcher subscriptions, ref counting, FSMs, and registry.
// T6-owned, not thread-safe.
class WatchManager {
public:
    WatchManager() = default;

    // Subscribe to a BTC timeframe. Returns true if newly subscribed.
    bool subscribe(BtcTimeframe tf);

    // Unsubscribe from a BTC timeframe. Returns true if was subscribed and now removed.
    bool unsubscribe(BtcTimeframe tf);

    // Query subscription state.
    bool is_subscribed(BtcTimeframe tf) const;
    int watcher_count(BtcTimeframe tf) const;

    // Get the FSM for a timeframe.
    SeriesRollingFsm& fsm(BtcTimeframe tf) { return entries_[idx(tf)].fsm; }
    const SeriesRollingFsm& fsm(BtcTimeframe tf) const { return entries_[idx(tf)].fsm; }

    // Tick: check staleness, trigger FSM transitions as needed.
    // stale_threshold_ns: nanoseconds after which WS data is considered stale.
    void tick(Timestamp_ns now, Timestamp_ns stale_threshold_ns);

    // Record that we received book data for a timeframe.
    void on_book_data_received(BtcTimeframe tf, Timestamp_ns now);

    // Get the last book update time for a timeframe.
    Timestamp_ns last_book_update(BtcTimeframe tf) const;

    // Collect all token IDs that need WS subscription across active series.
    // Uses registry to map timeframes to token IDs.
    std::vector<std::string> active_asset_ids(const BtcSeriesRegistry& registry) const;

    // Get list of subscribed timeframes.
    std::vector<BtcTimeframe> subscribed_timeframes() const;

    // How many total series are subscribed.
    int active_count() const;

private:
    struct WatchEntry {
        SeriesRollingFsm fsm;
        int watchers_count = 0;
        Timestamp_ns last_book_update_ns = 0;
    };

    WatchEntry entries_[kBtcTimeframeCount]{};

    static int idx(BtcTimeframe tf) { return static_cast<int>(tf); }
};

}  // namespace lt
