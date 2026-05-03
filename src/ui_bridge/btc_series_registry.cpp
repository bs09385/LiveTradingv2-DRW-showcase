#include "ui_bridge/btc_series_registry.h"

namespace lt {

void BtcSeriesRegistry::update_from_discovery(BtcTimeframe tf,
                                               const SeriesMarketInfo& current,
                                               const SeriesMarketInfo* next) {
    auto& e = entries_[idx(tf)];
    e.current = current;
    e.has_current = true;
    if (next) {
        e.next_market = *next;
        e.has_next_market = true;
    } else {
        e.next_market = {};
        e.has_next_market = false;
    }
}

const SeriesMarketInfo* BtcSeriesRegistry::current(BtcTimeframe tf) const {
    auto& e = entries_[idx(tf)];
    return e.has_current ? &e.current : nullptr;
}

const SeriesMarketInfo* BtcSeriesRegistry::next(BtcTimeframe tf) const {
    auto& e = entries_[idx(tf)];
    return e.has_next_market ? &e.next_market : nullptr;
}

bool BtcSeriesRegistry::has_next(BtcTimeframe tf) const {
    return entries_[idx(tf)].has_next_market;
}

void BtcSeriesRegistry::promote_next(BtcTimeframe tf) {
    auto& e = entries_[idx(tf)];
    if (!e.has_next_market) return;
    e.current = e.next_market;
    e.has_current = true;
    e.next_market = {};
    e.has_next_market = false;
}

void BtcSeriesRegistry::mark_closed(BtcTimeframe tf) {
    auto& e = entries_[idx(tf)];
    if (e.has_current) {
        e.current.is_closed = true;
    }
}

void BtcSeriesRegistry::clear(BtcTimeframe tf) {
    entries_[idx(tf)] = {};
}

bool BtcSeriesRegistry::has_current(BtcTimeframe tf) const {
    return entries_[idx(tf)].has_current;
}

}  // namespace lt
