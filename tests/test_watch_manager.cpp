#include "doctest/doctest.h"

#include "ui_bridge/watch_manager.h"
#include "ui_bridge/btc_series_registry.h"

using namespace lt;

namespace {

SeriesMarketInfo make_info(const char* cond, const char* up, const char* down) {
    SeriesMarketInfo info;
    info.set_condition_id(cond);
    info.set_token_id_up(up);
    info.set_token_id_down(down);
    return info;
}

}  // namespace

TEST_SUITE("WatchManager") {

TEST_CASE("initially no subscriptions") {
    WatchManager mgr;
    CHECK_FALSE(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.watcher_count(BtcTimeframe::BTC_5M) == 0);
    CHECK(mgr.active_count() == 0);
}

TEST_CASE("subscribe creates new subscription") {
    WatchManager mgr;
    CHECK(mgr.subscribe(BtcTimeframe::BTC_5M));
    CHECK(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.watcher_count(BtcTimeframe::BTC_5M) == 1);
    CHECK(mgr.active_count() == 1);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::CONNECTING);
}

TEST_CASE("second subscribe increments ref count") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    CHECK_FALSE(mgr.subscribe(BtcTimeframe::BTC_5M));  // already subscribed
    CHECK(mgr.watcher_count(BtcTimeframe::BTC_5M) == 2);
    CHECK(mgr.active_count() == 1);  // still 1 series
}

TEST_CASE("unsubscribe decrements ref count") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    mgr.subscribe(BtcTimeframe::BTC_5M);

    CHECK_FALSE(mgr.unsubscribe(BtcTimeframe::BTC_5M));  // still 1 left
    CHECK(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.watcher_count(BtcTimeframe::BTC_5M) == 1);
}

TEST_CASE("last unsubscribe triggers FSM disconnect") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    CHECK(mgr.unsubscribe(BtcTimeframe::BTC_5M));
    CHECK_FALSE(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.watcher_count(BtcTimeframe::BTC_5M) == 0);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::DISCONNECTED);
}

TEST_CASE("unsubscribe from zero count returns false") {
    WatchManager mgr;
    CHECK_FALSE(mgr.unsubscribe(BtcTimeframe::BTC_5M));
}

TEST_CASE("multiple timeframes are independent") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    mgr.subscribe(BtcTimeframe::BTC_15M);

    CHECK(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.is_subscribed(BtcTimeframe::BTC_15M));
    CHECK(mgr.active_count() == 2);

    mgr.unsubscribe(BtcTimeframe::BTC_5M);
    CHECK_FALSE(mgr.is_subscribed(BtcTimeframe::BTC_5M));
    CHECK(mgr.is_subscribed(BtcTimeframe::BTC_15M));
    CHECK(mgr.active_count() == 1);
}

TEST_CASE("subscribed_timeframes returns active list") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    mgr.subscribe(BtcTimeframe::BTC_15M);

    auto list = mgr.subscribed_timeframes();
    CHECK(list.size() == 2);
    CHECK(list[0] == BtcTimeframe::BTC_5M);
    CHECK(list[1] == BtcTimeframe::BTC_15M);
}

TEST_CASE("tick detects staleness") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    mgr.fsm(BtcTimeframe::BTC_5M).on_ws_connected();

    Timestamp_ns now = 1000000000LL;  // 1 second in ns
    Timestamp_ns stale_threshold = 500000000LL;  // 0.5s

    mgr.on_book_data_received(BtcTimeframe::BTC_5M, now);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::CONNECTED);

    // Tick at 1.3s — not stale yet
    mgr.tick(now + 300000000LL, stale_threshold);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::CONNECTED);

    // Tick at 1.6s — stale
    mgr.tick(now + 600000000LL, stale_threshold);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::STALE);
}

TEST_CASE("tick recovers from stale when new data arrives") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);
    mgr.fsm(BtcTimeframe::BTC_5M).on_ws_connected();

    Timestamp_ns stale_threshold = 500000000LL;

    // Go stale
    mgr.on_book_data_received(BtcTimeframe::BTC_5M, 1000000000LL);
    mgr.tick(2000000000LL, stale_threshold);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::STALE);

    // New data arrives
    mgr.on_book_data_received(BtcTimeframe::BTC_5M, 2100000000LL);
    mgr.tick(2200000000LL, stale_threshold);
    CHECK(mgr.fsm(BtcTimeframe::BTC_5M).state() == WatcherState::CONNECTED);
}

TEST_CASE("active_asset_ids collects token IDs from registry") {
    WatchManager mgr;
    BtcSeriesRegistry reg;

    auto info = make_info("cond1", "token_up_1", "token_down_1");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info, nullptr);

    mgr.subscribe(BtcTimeframe::BTC_5M);

    auto ids = mgr.active_asset_ids(reg);
    CHECK(ids.size() == 2);
    CHECK(ids[0] == "token_up_1");
    CHECK(ids[1] == "token_down_1");
}

TEST_CASE("active_asset_ids skips unsubscribed timeframes") {
    WatchManager mgr;
    BtcSeriesRegistry reg;

    auto info5 = make_info("c5", "u5", "d5");
    auto info15 = make_info("c15", "u15", "d15");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info5, nullptr);
    reg.update_from_discovery(BtcTimeframe::BTC_15M, info15, nullptr);

    mgr.subscribe(BtcTimeframe::BTC_5M);
    // BTC_15M not subscribed

    auto ids = mgr.active_asset_ids(reg);
    CHECK(ids.size() == 2);
    CHECK(ids[0] == "u5");
    CHECK(ids[1] == "d5");
}

TEST_CASE("active_asset_ids with no registry data returns empty") {
    WatchManager mgr;
    BtcSeriesRegistry reg;

    mgr.subscribe(BtcTimeframe::BTC_5M);

    auto ids = mgr.active_asset_ids(reg);
    CHECK(ids.empty());
}

TEST_CASE("on_book_data_received updates last_book_update") {
    WatchManager mgr;
    mgr.subscribe(BtcTimeframe::BTC_5M);

    CHECK(mgr.last_book_update(BtcTimeframe::BTC_5M) == 0);
    mgr.on_book_data_received(BtcTimeframe::BTC_5M, 12345LL);
    CHECK(mgr.last_book_update(BtcTimeframe::BTC_5M) == 12345LL);
}

}  // TEST_SUITE
