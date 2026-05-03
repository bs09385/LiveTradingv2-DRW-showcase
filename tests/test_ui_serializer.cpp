#include "doctest/doctest.h"

#include "ui_bridge/ui_serializer.h"

#include <cstring>
#include <string>

TEST_SUITE("UiSerializer") {

TEST_CASE("format_price basic") {
    char buf[32];
    lt::format_price(buf, sizeof(buf), 5200);
    CHECK(std::string(buf) == "0.5200");
}

TEST_CASE("format_price zero") {
    char buf[32];
    lt::format_price(buf, sizeof(buf), 0);
    CHECK(std::string(buf) == "0.0000");
}

TEST_CASE("format_price max") {
    char buf[32];
    lt::format_price(buf, sizeof(buf), 10000);
    CHECK(std::string(buf) == "1.0000");
}

TEST_CASE("format_price invalid") {
    char buf[32];
    lt::format_price(buf, sizeof(buf), -1);
    CHECK(std::string(buf) == "-1");
}

TEST_CASE("format_price fractional") {
    char buf[32];
    lt::format_price(buf, sizeof(buf), 100);
    CHECK(std::string(buf) == "0.0100");
}

TEST_CASE("serialize empty snapshot") {
    lt::EngineSnapshot snap{};
    snap.timestamp_ns = 12345;

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"timestamp_ns\":12345") != std::string::npos);
    CHECK(json.find("\"markets\":[]") != std::string::npos);
    CHECK(json.find("\"working_orders\":[]") != std::string::npos);
    CHECK(json.find("\"closed_orders\":[]") != std::string::npos);
    CHECK(json.find("\"trades\":[]") != std::string::npos);
    CHECK(json.find("\"positions\":[]") != std::string::npos);
    CHECK(json.find("\"execution_mode\":\"DRY_RUN\"") != std::string::npos);
    CHECK(json.find("\"degraded\":false") != std::string::npos);
}

TEST_CASE("serialize snapshot with state") {
    lt::UiStateSnapshot state{};
    state.strategy_enabled = true;
    state.spread_ticks = 2;
    state.quote_size = 10;
    state.execution_mode = 1;  // LIVE
    state.risk_checks = 100;
    state.risk_allows = 95;
    state.risk_denies = 5;
    state.positions[0].token_id = lt::AssetId("tok_up");
    state.positions[0].position = 12;
    state.position_count = 1;

    lt::EngineSnapshot snap{};
    snap.timestamp_ns = 999;
    snap.state = &state;

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"strategy_enabled\":true") != std::string::npos);
    CHECK(json.find("\"spread_ticks\":2") != std::string::npos);
    CHECK(json.find("\"quote_size\":10") != std::string::npos);
    CHECK(json.find("\"execution_mode\":\"LIVE\"") != std::string::npos);
    CHECK(json.find("\"risk_checks\":100") != std::string::npos);
    CHECK(json.find("\"risk_allows\":95") != std::string::npos);
    CHECK(json.find("\"risk_denies\":5") != std::string::npos);
    CHECK(json.find("\"positions\":[{\"token_id\":\"tok_up\",\"position\":12}]") != std::string::npos);
}

TEST_CASE("serialize snapshot with working orders") {
    lt::UiStateSnapshot state{};
    state.working_orders[0].client_order_id = lt::OrderId("lt-1");
    state.working_orders[0].side = lt::Side::BID;
    state.working_orders[0].price = 5200;
    state.working_orders[0].original_size = 5;
    state.working_orders[0].is_live = true;
    state.working_orders[0].lifecycle_state =
        static_cast<uint8_t>(lt::UiOrderLifecycleState::WORKING);
    state.working_orders[0].last_update_ts = 123;
    state.working_order_count = 1;

    lt::EngineSnapshot snap{};
    snap.state = &state;

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"client_order_id\":\"lt-1\"") != std::string::npos);
    CHECK(json.find("\"side\":\"BID\"") != std::string::npos);
    CHECK(json.find("\"price\":5200") != std::string::npos);
    CHECK(json.find("\"is_live\":true") != std::string::npos);
    CHECK(json.find("\"lifecycle_state\":\"WORKING\"") != std::string::npos);
    CHECK(json.find("\"last_update_ts\":123") != std::string::npos);
}

TEST_CASE("serialize snapshot with closed orders and trades") {
    lt::UiStateSnapshot state{};
    state.closed_orders[0].client_order_id = lt::OrderId("c-1");
    state.closed_orders[0].exchange_order_id = lt::OrderId("e-1");
    state.closed_orders[0].asset_id = lt::AssetId("tok-a");
    state.closed_orders[0].market_id = lt::AssetId("cond_1");
    state.closed_orders[0].side = lt::Side::ASK;
    state.closed_orders[0].price = 5300;
    state.closed_orders[0].original_size = 10;
    state.closed_orders[0].filled_size = 4;
    state.closed_orders[0].lifecycle_state =
        static_cast<uint8_t>(lt::UiOrderLifecycleState::CANCELED_WITH_FILL);
    state.closed_orders[0].last_update_ts = 456;
    state.closed_order_count = 1;

    state.trades[0].trade_id = lt::TradeId("t-1");
    state.trades[0].order_id = lt::OrderId("e-1");
    state.trades[0].asset_id = lt::AssetId("tok-a");
    state.trades[0].market_id = lt::AssetId("cond_1");
    state.trades[0].side = lt::Side::ASK;
    state.trades[0].price = 5300;
    state.trades[0].size = 4;
    state.trades[0].trade_status = 1;  // MATCHED
    state.trades[0].last_update_ts = 789;
    state.trade_count = 1;

    lt::EngineSnapshot snap{};
    snap.state = &state;
    lt::UiMarketSnapshot mkt{};
    mkt.condition_id = lt::AssetId("cond_1");
    mkt.token_id_up = lt::AssetId("tok_up");
    mkt.token_id_down = lt::AssetId("tok-a");
    mkt.series_label = "BTC 5M";
    snap.markets.push_back(mkt);
    std::string json = lt::serialize_engine_snapshot(snap);

    CHECK(json.find("\"closed_orders\":[") != std::string::npos);
    CHECK(json.find("\"lifecycle_state\":\"CANCELED_WITH_FILL\"") != std::string::npos);
    CHECK(json.find("\"market_label\":\"BTC 5M DOWN\"") != std::string::npos);
    CHECK(json.find("\"trades\":[") != std::string::npos);
    CHECK(json.find("\"status\":\"MATCHED\"") != std::string::npos);
}

TEST_CASE("serialize snapshot with market data") {
    lt::UiBookUpdate book_up{};
    book_up.bbo.best_bid = 5200;
    book_up.bbo.best_ask = 5400;
    book_up.bbo.bid_size = 100;
    book_up.bbo.ask_size = 50;
    book_up.bids[0] = {5200, 100};
    book_up.bid_count = 1;
    book_up.asks[0] = {5400, 50};
    book_up.ask_count = 1;

    lt::UiMarketSnapshot mkt;
    mkt.condition_id = lt::AssetId("cond_1");
    mkt.token_id_up = lt::AssetId("tok_up");
    mkt.token_id_down = lt::AssetId("tok_down");
    mkt.book_up = &book_up;
    mkt.book_down = nullptr;
    mkt.position_up = 10;
    mkt.position_down = 0;

    lt::EngineSnapshot snap{};
    snap.markets.push_back(std::move(mkt));

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"condition_id\":\"cond_1\"") != std::string::npos);
    CHECK(json.find("\"token_id_up\":\"tok_up\"") != std::string::npos);
    CHECK(json.find("\"token_id_down\":\"tok_down\"") != std::string::npos);
    CHECK(json.find("\"series_label\":\"\"") != std::string::npos);
    CHECK(json.find("\"best_bid\":5200") != std::string::npos);
    CHECK(json.find("\"best_ask\":5400") != std::string::npos);
    CHECK(json.find("\"position_up\":10") != std::string::npos);
    CHECK(json.find("\"position_down\":0") != std::string::npos);
}

TEST_CASE("serialize snapshot with metrics") {
    lt::EngineSnapshot snap{};
    snap.metrics.ws_frames = 1000;
    snap.metrics.parse_ok = 999;
    snap.metrics.sched_cycles = 50000;
    snap.metrics.rest_requests = 100;
    snap.metrics.rest_errors = 2;
    snap.metrics.ui_snapshots_dropped = 3;
    snap.metrics.ui_book_drops = 4;
    snap.metrics.ui_state_drops = 5;

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"ws_frames\":1000") != std::string::npos);
    CHECK(json.find("\"parse_ok\":999") != std::string::npos);
    CHECK(json.find("\"sched_cycles\":50000") != std::string::npos);
    CHECK(json.find("\"rest_requests\":100") != std::string::npos);
    CHECK(json.find("\"rest_errors\":2") != std::string::npos);
    CHECK(json.find("\"ui_snapshots_dropped\":3") != std::string::npos);
    CHECK(json.find("\"ui_book_drops\":4") != std::string::npos);
    CHECK(json.find("\"ui_state_drops\":5") != std::string::npos);
}

TEST_CASE("serialize snapshot with gateway health") {
    lt::EngineSnapshot snap{};
    snap.gateway.degraded = true;
    snap.gateway.heartbeat_ok = 500;
    snap.gateway.heartbeat_fail = 3;

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("\"degraded\":true") != std::string::npos);
    CHECK(json.find("\"heartbeat_ok\":500") != std::string::npos);
    CHECK(json.find("\"heartbeat_fail\":3") != std::string::npos);
}

TEST_CASE("serialize null book produces empty arrays") {
    lt::UiMarketSnapshot mkt;
    mkt.condition_id = lt::AssetId("cond_x");
    mkt.token_id_up = lt::AssetId("up_x");
    mkt.token_id_down = lt::AssetId("down_x");
    mkt.book_up = nullptr;
    mkt.book_down = nullptr;

    lt::EngineSnapshot snap{};
    snap.markets.push_back(std::move(mkt));

    std::string json = lt::serialize_engine_snapshot(snap);
    // null books produce empty bids/asks arrays
    CHECK(json.find("\"bids\":[]") != std::string::npos);
    CHECK(json.find("\"asks\":[]") != std::string::npos);
}

TEST_CASE("serialize escapes special characters in strings") {
    lt::EngineSnapshot snap{};
    snap.account_name = "acct\"name";
    snap.account_address = "line1\\line2";

    lt::UiMarketSnapshot mkt;
    mkt.condition_id = lt::AssetId("cond\nx");
    mkt.token_id_up = lt::AssetId("up\tid");
    mkt.token_id_down = lt::AssetId("down");
    snap.markets.push_back(std::move(mkt));

    std::string json = lt::serialize_engine_snapshot(snap);
    CHECK(json.find("acct\\\"name") != std::string::npos);
    CHECK(json.find("line1\\\\line2") != std::string::npos);
    CHECK(json.find("cond\\nx") != std::string::npos);
}

}  // TEST_SUITE
