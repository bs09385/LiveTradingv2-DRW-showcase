#include "doctest/doctest.h"

#include "ui_bridge/series_rolling_fsm.h"

using namespace lt;

TEST_SUITE("SeriesRollingFsm") {

TEST_CASE("initial state is DISCONNECTED") {
    SeriesRollingFsm fsm;
    CHECK(fsm.state() == WatcherState::DISCONNECTED);
    CHECK_FALSE(fsm.is_active());
}

TEST_CASE("subscribe transitions to CONNECTING") {
    SeriesRollingFsm fsm;
    CHECK(fsm.on_subscribe());
    CHECK(fsm.state() == WatcherState::CONNECTING);
    CHECK(fsm.is_active());
    CHECK(fsm.needs_ws_connection());
}

TEST_CASE("subscribe from non-DISCONNECTED fails") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    CHECK_FALSE(fsm.on_subscribe());  // already in CONNECTING
}

TEST_CASE("ws_connected transitions CONNECTING to CONNECTED") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    CHECK(fsm.on_ws_connected());
    CHECK(fsm.state() == WatcherState::CONNECTED);
}

TEST_CASE("ws_connected fails from wrong state") {
    SeriesRollingFsm fsm;
    CHECK_FALSE(fsm.on_ws_connected());  // DISCONNECTED
}

TEST_CASE("ws_stale transitions CONNECTED to STALE") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    CHECK(fsm.on_ws_stale());
    CHECK(fsm.state() == WatcherState::STALE);
}

TEST_CASE("ws_recovered transitions STALE to CONNECTED") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    fsm.on_ws_stale();
    CHECK(fsm.on_ws_recovered());
    CHECK(fsm.state() == WatcherState::CONNECTED);
}

TEST_CASE("ws_recovered fails from non-STALE") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    CHECK_FALSE(fsm.on_ws_recovered());  // CONNECTED, not STALE
}

TEST_CASE("market_closed with next -> ROLLING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    CHECK(fsm.on_market_closed(true));
    CHECK(fsm.state() == WatcherState::ROLLING);
    CHECK(fsm.is_rolling());
}

TEST_CASE("market_closed without next -> ROLL_PENDING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    CHECK(fsm.on_market_closed(false));
    CHECK(fsm.state() == WatcherState::ROLL_PENDING);
    CHECK(fsm.is_waiting_for_next());
}

TEST_CASE("market_closed from STALE works") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    fsm.on_ws_stale();
    CHECK(fsm.on_market_closed(true));
    CHECK(fsm.state() == WatcherState::ROLLING);
}

TEST_CASE("market_closed from DISCONNECTED fails") {
    SeriesRollingFsm fsm;
    CHECK_FALSE(fsm.on_market_closed(true));
}

TEST_CASE("next_discovered transitions ROLL_PENDING to ROLLING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    fsm.on_market_closed(false);
    CHECK(fsm.on_next_discovered());
    CHECK(fsm.state() == WatcherState::ROLLING);
}

TEST_CASE("next_discovered fails from non-ROLL_PENDING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    CHECK_FALSE(fsm.on_next_discovered());  // CONNECTED
}

TEST_CASE("switch_complete transitions ROLLING to CONNECTING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    fsm.on_ws_connected();
    fsm.on_market_closed(true);
    CHECK(fsm.on_switch_complete());
    CHECK(fsm.state() == WatcherState::CONNECTING);
    CHECK(fsm.needs_ws_connection());
}

TEST_CASE("switch_complete fails from non-ROLLING") {
    SeriesRollingFsm fsm;
    fsm.on_subscribe();
    CHECK_FALSE(fsm.on_switch_complete());  // CONNECTING
}

TEST_CASE("unsubscribe from any active state -> DISCONNECTED") {
    SUBCASE("from CONNECTING") {
        SeriesRollingFsm fsm;
        fsm.on_subscribe();
        CHECK(fsm.on_unsubscribe());
        CHECK(fsm.state() == WatcherState::DISCONNECTED);
    }
    SUBCASE("from CONNECTED") {
        SeriesRollingFsm fsm;
        fsm.on_subscribe();
        fsm.on_ws_connected();
        CHECK(fsm.on_unsubscribe());
        CHECK(fsm.state() == WatcherState::DISCONNECTED);
    }
    SUBCASE("from STALE") {
        SeriesRollingFsm fsm;
        fsm.on_subscribe();
        fsm.on_ws_connected();
        fsm.on_ws_stale();
        CHECK(fsm.on_unsubscribe());
        CHECK(fsm.state() == WatcherState::DISCONNECTED);
    }
    SUBCASE("from ROLLING") {
        SeriesRollingFsm fsm;
        fsm.on_subscribe();
        fsm.on_ws_connected();
        fsm.on_market_closed(true);
        CHECK(fsm.on_unsubscribe());
        CHECK(fsm.state() == WatcherState::DISCONNECTED);
    }
    SUBCASE("from ROLL_PENDING") {
        SeriesRollingFsm fsm;
        fsm.on_subscribe();
        fsm.on_ws_connected();
        fsm.on_market_closed(false);
        CHECK(fsm.on_unsubscribe());
        CHECK(fsm.state() == WatcherState::DISCONNECTED);
    }
}

TEST_CASE("unsubscribe from DISCONNECTED fails") {
    SeriesRollingFsm fsm;
    CHECK_FALSE(fsm.on_unsubscribe());
}

TEST_CASE("full rolling cycle") {
    SeriesRollingFsm fsm;

    // Initial subscribe
    fsm.on_subscribe();
    CHECK(fsm.state() == WatcherState::CONNECTING);

    // Connect
    fsm.on_ws_connected();
    CHECK(fsm.state() == WatcherState::CONNECTED);

    // Market closes, no next yet
    fsm.on_market_closed(false);
    CHECK(fsm.state() == WatcherState::ROLL_PENDING);

    // Next discovered
    fsm.on_next_discovered();
    CHECK(fsm.state() == WatcherState::ROLLING);

    // Switch to new market
    fsm.on_switch_complete();
    CHECK(fsm.state() == WatcherState::CONNECTING);

    // New WS connects
    fsm.on_ws_connected();
    CHECK(fsm.state() == WatcherState::CONNECTED);
}

TEST_CASE("watcher_state_name covers all states") {
    CHECK(std::string(watcher_state_name(WatcherState::DISCONNECTED)) == "DISCONNECTED");
    CHECK(std::string(watcher_state_name(WatcherState::CONNECTING)) == "CONNECTING");
    CHECK(std::string(watcher_state_name(WatcherState::CONNECTED)) == "CONNECTED");
    CHECK(std::string(watcher_state_name(WatcherState::STALE)) == "STALE");
    CHECK(std::string(watcher_state_name(WatcherState::ROLL_PENDING)) == "ROLL_PENDING");
    CHECK(std::string(watcher_state_name(WatcherState::ROLLING)) == "ROLLING");
}

}  // TEST_SUITE
