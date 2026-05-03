#include "doctest/doctest.h"

#include <optional>

#include "ui_bridge/btc_series_registry.h"
#include "ui_bridge/series_rolling_fsm.h"
#include "ui_bridge/watch_manager.h"
#include "ui_bridge/watcher_book_store.h"

using namespace lt;

namespace {

SeriesMarketInfo make_info(const char* cond, const char* up, const char* down,
                           bool closed = false) {
    SeriesMarketInfo info;
    info.set_condition_id(cond);
    info.set_token_id_up(up);
    info.set_token_id_down(down);
    info.is_closed = closed;
    return info;
}

// Simulates the on_discovery_result + execute_rollover logic from IpcBridge::Impl
// without requiring the full IpcBridge (which needs Boost.Asio, WS, queues, etc).
struct RolloverHarness {
    WatchManager watch_manager;
    BtcSeriesRegistry series_registry;
    WatcherBookStore watcher_books;
    std::vector<WatcherBookLevel> pending_trades[kBtcTimeframeCount];

    // Track calls
    int rollover_count = 0;
    BtcTimeframe last_rollover_tf{};
    int status_send_count = 0;
    int refresh_sub_count = 0;

    void on_discovery_result(BtcTimeframe tf, SeriesMarketInfo current,
                             std::optional<SeriesMarketInfo> next) {
        const SeriesMarketInfo* next_ptr = next.has_value() ? &next.value() : nullptr;
        auto& fsm = watch_manager.fsm(tf);

        // Detect market closure
        if (current.is_closed && fsm.is_active() && !fsm.is_rolling() && !fsm.is_waiting_for_next()) {
            series_registry.update_from_discovery(tf, current, next_ptr);
            bool has_next = next_ptr && !next_ptr->is_closed && !next_ptr->empty();
            fsm.on_market_closed(has_next);

            if (fsm.is_rolling()) {
                execute_rollover(tf);
            } else {
                send_watcher_status(tf);
            }
            return;
        }

        // Normal update
        series_registry.update_from_discovery(tf, current, next_ptr);

        // Check if waiting for next and it arrived
        if (fsm.is_waiting_for_next() && series_registry.has_next(tf)) {
            fsm.on_next_discovered();
            if (fsm.is_rolling()) {
                execute_rollover(tf);
            } else {
                send_watcher_status(tf);
            }
            return;
        }

        refresh_watcher_subscriptions();
    }

    void execute_rollover(BtcTimeframe tf) {
        int ti = static_cast<int>(tf);

        // Capture old token IDs
        const auto* old_info = series_registry.current(tf);
        std::string old_up, old_down;
        if (old_info) {
            old_up = old_info->token_id_up;
            old_down = old_info->token_id_down;
        }

        // Promote next to current
        series_registry.promote_next(tf);

        // Clear old books
        if (!old_up.empty()) watcher_books.clear_book(old_up);
        if (!old_down.empty()) watcher_books.clear_book(old_down);

        // Clear pending trades
        pending_trades[ti].clear();

        // Reset staleness tracking
        watch_manager.on_book_data_received(tf, 0);

        // Refresh WS subscriptions
        refresh_watcher_subscriptions();

        // Complete the roll: ROLLING -> CONNECTING
        watch_manager.fsm(tf).on_switch_complete();

        // Notify UI
        send_watcher_status(tf);

        ++rollover_count;
        last_rollover_tf = tf;
    }

    void send_watcher_status(BtcTimeframe /*tf*/) {
        ++status_send_count;
    }

    void refresh_watcher_subscriptions() {
        ++refresh_sub_count;
    }

    // Setup helpers
    void setup_subscribed_connected(BtcTimeframe tf, const char* cond,
                                     const char* up, const char* down) {
        watch_manager.subscribe(tf);
        auto info = make_info(cond, up, down);
        series_registry.update_from_discovery(tf, info, nullptr);
        watch_manager.fsm(tf).on_ws_connected();
    }
};

}  // namespace

TEST_SUITE("WatcherRollover") {

TEST_CASE("rollover with next available: CONNECTED -> ROLLING -> CONNECTING") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_5M;

    // Setup: subscribed and connected with current market
    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::CONNECTED);

    // Add some book data and pending trades
    WatcherBookLevel bids[] = {{5000, 100}};
    WatcherBookLevel asks[] = {{5100, 200}};
    h.watcher_books.apply_book_snapshot("up_old", bids, 1, asks, 1);
    h.watcher_books.apply_book_snapshot("down_old", bids, 1, asks, 1);
    h.pending_trades[0].push_back({5050, 50});
    h.watch_manager.on_book_data_received(tf, 1000000);

    // Discovery returns: current is closed, next is available
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    auto next_info = make_info("cond_new", "up_new", "down_new", false);
    h.on_discovery_result(tf, closed_info, next_info);

    // Verify FSM went through ROLLING and arrived at CONNECTING
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::CONNECTING);

    // Verify rollover executed
    CHECK(h.rollover_count == 1);
    CHECK(h.last_rollover_tf == tf);

    // Verify new current is the promoted market
    const auto* cur = h.series_registry.current(tf);
    REQUIRE(cur != nullptr);
    CHECK(cur->condition_id_view() == "cond_new");
    CHECK(cur->token_id_up_view() == "up_new");
    CHECK(cur->token_id_down_view() == "down_new");

    // Verify next is cleared after promotion
    CHECK_FALSE(h.series_registry.has_next(tf));

    // Verify old books were cleared
    CHECK_FALSE(h.watcher_books.has_book("up_old"));
    CHECK_FALSE(h.watcher_books.has_book("down_old"));

    // Verify pending trades cleared
    CHECK(h.pending_trades[0].empty());

    // Verify staleness tracking was reset
    CHECK(h.watch_manager.last_book_update(tf) == 0);

    // Verify subscriptions were refreshed
    CHECK(h.refresh_sub_count > 0);
}

TEST_CASE("rollover without next: CONNECTED -> ROLL_PENDING, then next discovered -> CONNECTING") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_5M;

    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");

    // Discovery returns: current is closed, no next available
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    h.on_discovery_result(tf, closed_info, std::nullopt);

    // Should be in ROLL_PENDING (waiting for next)
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::ROLL_PENDING);
    CHECK(h.rollover_count == 0);  // no rollover yet

    // Status should have been sent
    CHECK(h.status_send_count > 0);
    int status_before = h.status_send_count;

    // Later discovery finds the next market (as a normal non-closed current)
    auto new_current = make_info("cond_old", "up_old", "down_old", true);
    auto next_info = make_info("cond_new", "up_new", "down_new", false);

    // Reset and simulate: now we get an update where next is available
    // We need to call as if discovery found the same closed current + new next
    // But FSM is already in ROLL_PENDING, so the closure path won't fire again
    // Instead the waiting_for_next path fires
    h.on_discovery_result(tf, new_current, next_info);

    // Should have triggered rollover
    CHECK(h.rollover_count == 1);
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::CONNECTING);

    // New current is promoted
    const auto* cur = h.series_registry.current(tf);
    REQUIRE(cur != nullptr);
    CHECK(cur->condition_id_view() == "cond_new");
}

TEST_CASE("rollover from STALE state") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_15M;

    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");

    // Transition to STALE
    h.watch_manager.fsm(tf).on_ws_stale();
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::STALE);

    // Discovery returns closed market with next available
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    auto next_info = make_info("cond_new", "up_new", "down_new", false);
    h.on_discovery_result(tf, closed_info, next_info);

    // FSM: STALE -> on_market_closed(true) -> ROLLING -> execute_rollover -> CONNECTING
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::CONNECTING);
    CHECK(h.rollover_count == 1);
}

TEST_CASE("idempotent closure detection: second is_closed is no-op") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_5M;

    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");

    // First closure with next -> rollover
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    auto next_info = make_info("cond_new", "up_new", "down_new", false);
    h.on_discovery_result(tf, closed_info, next_info);
    CHECK(h.rollover_count == 1);
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::CONNECTING);

    int rollover_before = h.rollover_count;

    // Second discovery with same closed info -> should be a no-op (FSM is CONNECTING)
    // CONNECTING is active but not CONNECTED/STALE, so on_market_closed won't fire
    h.on_discovery_result(tf, closed_info, next_info);

    // No additional rollover
    CHECK(h.rollover_count == rollover_before);
}

TEST_CASE("old books and pending trades are cleared on rollover") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_5M;

    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");

    // Populate books for old tokens
    WatcherBookLevel bids[] = {{5000, 100}, {4900, 200}};
    WatcherBookLevel asks[] = {{5100, 150}};
    h.watcher_books.apply_book_snapshot("up_old", bids, 2, asks, 1);
    h.watcher_books.apply_book_snapshot("down_old", bids, 2, asks, 1);
    CHECK(h.watcher_books.has_book("up_old"));
    CHECK(h.watcher_books.has_book("down_old"));

    // Populate pending trades
    int ti = static_cast<int>(tf);
    h.pending_trades[ti].push_back({5050, 50});
    h.pending_trades[ti].push_back({5060, 30});
    CHECK(h.pending_trades[ti].size() == 2);

    // Rollover
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    auto next_info = make_info("cond_new", "up_new", "down_new", false);
    h.on_discovery_result(tf, closed_info, next_info);

    // Old books gone
    CHECK_FALSE(h.watcher_books.has_book("up_old"));
    CHECK_FALSE(h.watcher_books.has_book("down_old"));

    // Pending trades gone
    CHECK(h.pending_trades[ti].empty());

    // New books not yet populated (need WS data)
    CHECK_FALSE(h.watcher_books.has_book("up_new"));
}

TEST_CASE("edge case: next market is also closed — treated as no next") {
    RolloverHarness h;
    auto tf = BtcTimeframe::BTC_15M;

    h.setup_subscribed_connected(tf, "cond_old", "up_old", "down_old");

    // Next market exists but is also closed
    auto closed_info = make_info("cond_old", "up_old", "down_old", true);
    auto next_closed = make_info("cond_next", "up_next", "down_next", true);
    h.on_discovery_result(tf, closed_info, next_closed);

    // Should be ROLL_PENDING since next is closed (treated as no usable next)
    CHECK(h.watch_manager.fsm(tf).state() == WatcherState::ROLL_PENDING);
    CHECK(h.rollover_count == 0);
}

}  // TEST_SUITE
