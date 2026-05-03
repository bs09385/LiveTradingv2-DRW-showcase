#include "doctest/doctest.h"

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

TEST_SUITE("BtcSeriesRegistry") {

TEST_CASE("initially empty") {
    BtcSeriesRegistry reg;
    CHECK_FALSE(reg.has_current(BtcTimeframe::BTC_5M));
    CHECK(reg.current(BtcTimeframe::BTC_5M) == nullptr);
    CHECK(reg.next(BtcTimeframe::BTC_5M) == nullptr);
    CHECK_FALSE(reg.has_next(BtcTimeframe::BTC_5M));
}

TEST_CASE("update_from_discovery sets current") {
    BtcSeriesRegistry reg;
    auto info = make_info("cond1", "up1", "down1");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info, nullptr);

    CHECK(reg.has_current(BtcTimeframe::BTC_5M));
    auto* cur = reg.current(BtcTimeframe::BTC_5M);
    REQUIRE(cur != nullptr);
    CHECK(cur->condition_id_view() == "cond1");
    CHECK(cur->token_id_up_view() == "up1");
    CHECK(cur->token_id_down_view() == "down1");
    CHECK_FALSE(cur->is_closed);
    CHECK_FALSE(reg.has_next(BtcTimeframe::BTC_5M));
}

TEST_CASE("update_from_discovery sets current and next") {
    BtcSeriesRegistry reg;
    auto current = make_info("cond1", "up1", "down1");
    auto next = make_info("cond2", "up2", "down2");
    reg.update_from_discovery(BtcTimeframe::BTC_15M, current, &next);

    CHECK(reg.has_current(BtcTimeframe::BTC_15M));
    CHECK(reg.has_next(BtcTimeframe::BTC_15M));
    auto* n = reg.next(BtcTimeframe::BTC_15M);
    REQUIRE(n != nullptr);
    CHECK(n->condition_id_view() == "cond2");
}

TEST_CASE("timeframes are independent") {
    BtcSeriesRegistry reg;
    auto info5m = make_info("cond_5m", "up5", "down5");
    auto info15m = make_info("cond_15m", "up15", "down15");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info5m, nullptr);
    reg.update_from_discovery(BtcTimeframe::BTC_15M, info15m, nullptr);

    CHECK(reg.current(BtcTimeframe::BTC_5M)->condition_id_view() == "cond_5m");
    CHECK(reg.current(BtcTimeframe::BTC_15M)->condition_id_view() == "cond_15m");
}

TEST_CASE("promote_next moves next to current") {
    BtcSeriesRegistry reg;
    auto current = make_info("old", "old_up", "old_down");
    auto next = make_info("new", "new_up", "new_down");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, current, &next);

    reg.promote_next(BtcTimeframe::BTC_5M);

    CHECK(reg.has_current(BtcTimeframe::BTC_5M));
    CHECK_FALSE(reg.has_next(BtcTimeframe::BTC_5M));
    CHECK(reg.current(BtcTimeframe::BTC_5M)->condition_id_view() == "new");
    CHECK(reg.current(BtcTimeframe::BTC_5M)->token_id_up_view() == "new_up");
}

TEST_CASE("promote_next with no next is no-op") {
    BtcSeriesRegistry reg;
    auto info = make_info("cond", "up", "down");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info, nullptr);

    reg.promote_next(BtcTimeframe::BTC_5M);

    // Current unchanged
    CHECK(reg.current(BtcTimeframe::BTC_5M)->condition_id_view() == "cond");
}

TEST_CASE("mark_closed sets is_closed on current") {
    BtcSeriesRegistry reg;
    auto info = make_info("cond", "up", "down");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info, nullptr);

    CHECK_FALSE(reg.current(BtcTimeframe::BTC_5M)->is_closed);
    reg.mark_closed(BtcTimeframe::BTC_5M);
    CHECK(reg.current(BtcTimeframe::BTC_5M)->is_closed);
}

TEST_CASE("clear removes all state for timeframe") {
    BtcSeriesRegistry reg;
    auto current = make_info("cond", "up", "down");
    auto next = make_info("next", "nup", "ndown");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, current, &next);

    reg.clear(BtcTimeframe::BTC_5M);

    CHECK_FALSE(reg.has_current(BtcTimeframe::BTC_5M));
    CHECK_FALSE(reg.has_next(BtcTimeframe::BTC_5M));
    CHECK(reg.current(BtcTimeframe::BTC_5M) == nullptr);
    CHECK(reg.next(BtcTimeframe::BTC_5M) == nullptr);
}

TEST_CASE("update replaces previous data") {
    BtcSeriesRegistry reg;
    auto info1 = make_info("cond1", "up1", "down1");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info1, nullptr);

    auto info2 = make_info("cond2", "up2", "down2");
    reg.update_from_discovery(BtcTimeframe::BTC_5M, info2, nullptr);

    CHECK(reg.current(BtcTimeframe::BTC_5M)->condition_id_view() == "cond2");
}

}  // TEST_SUITE
