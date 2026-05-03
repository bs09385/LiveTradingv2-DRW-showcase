#include "doctest/doctest.h"

#include "ui_bridge/ui_types.h"

#include <cstring>
#include <type_traits>

TEST_SUITE("UiTypes") {

TEST_CASE("UiBookLevel is trivially copyable") {
    static_assert(std::is_trivially_copyable_v<lt::UiBookLevel>);
    CHECK(sizeof(lt::UiBookLevel) > 0);
}

TEST_CASE("UiBookUpdate is trivially copyable") {
    static_assert(std::is_trivially_copyable_v<lt::UiBookUpdate>);
    CHECK(sizeof(lt::UiBookUpdate) > 0);
}

TEST_CASE("UiWorkingOrder is trivially copyable") {
    static_assert(std::is_trivially_copyable_v<lt::UiWorkingOrder>);
    CHECK(sizeof(lt::UiWorkingOrder) > 0);
}

TEST_CASE("UiStateSnapshot is trivially copyable") {
    static_assert(std::is_trivially_copyable_v<lt::UiStateSnapshot>);
    CHECK(sizeof(lt::UiStateSnapshot) > 0);
}

TEST_CASE("UiBookUpdate default values") {
    lt::UiBookUpdate upd{};
    CHECK(upd.bid_count == 0);
    CHECK(upd.ask_count == 0);
    CHECK(upd.timestamp == 0);
    CHECK(upd.asset_id.len == 0);
}

TEST_CASE("UiStateSnapshot default values") {
    lt::UiStateSnapshot snap{};
    CHECK(snap.working_order_count == 0);
    CHECK(snap.closed_order_count == 0);
    CHECK(snap.trade_count == 0);
    CHECK(snap.strategy_enabled == false);
    CHECK(snap.spread_ticks == 0);
    CHECK(snap.quote_size == 0);
    CHECK(snap.execution_mode == 0);
    CHECK(snap.risk_checks == 0);
    CHECK(snap.risk_allows == 0);
    CHECK(snap.risk_denies == 0);
    CHECK(snap.timestamp == 0);
}

TEST_CASE("UiBookUpdate bid/ask population") {
    lt::UiBookUpdate upd{};
    upd.asset_id = lt::AssetId("token_123");
    upd.bbo.best_bid = 5200;
    upd.bbo.best_ask = 5400;
    upd.bids[0] = lt::UiBookLevel{5200, lt::qty_from_int(100)};
    upd.bids[1] = lt::UiBookLevel{5100, lt::qty_from_int(50)};
    upd.bid_count = 2;
    upd.asks[0] = lt::UiBookLevel{5400, lt::qty_from_int(80)};
    upd.ask_count = 1;

    CHECK(upd.bid_count == 2);
    CHECK(upd.bids[0].price == 5200);
    CHECK(upd.bids[0].size == lt::qty_from_int(100));
    CHECK(upd.bids[1].price == 5100);
    CHECK(upd.asks[0].price == 5400);
    CHECK(upd.ask_count == 1);
}

TEST_CASE("UiWorkingOrder fields") {
    lt::UiWorkingOrder wo{};
    wo.client_order_id = lt::OrderId("lt-1");
    wo.side = lt::Side::ASK;
    wo.price = 4800;
    wo.original_size = lt::qty_from_int(5);
    wo.is_live = true;
    wo.is_pending = false;

    CHECK(wo.client_order_id.view() == "lt-1");
    CHECK(wo.side == lt::Side::ASK);
    CHECK(wo.price == 4800);
    CHECK(wo.is_live == true);
    CHECK(wo.is_pending == false);
    CHECK(wo.lifecycle_state == 0);
}

TEST_CASE("kMaxUiBookDepth and kMaxUiWorkingOrders constants") {
    CHECK(lt::kMaxUiBookDepth == 10);
    CHECK(lt::kMaxUiWorkingOrders == 64);
    CHECK(lt::kMaxUiClosedOrders == 128);
    CHECK(lt::kMaxUiTrades == 128);
}

TEST_CASE("UiBookUpdate memcpy roundtrip") {
    lt::UiBookUpdate src{};
    src.asset_id = lt::AssetId("test_asset");
    src.bbo.best_bid = 1000;
    src.bbo.best_ask = 2000;
    src.bids[0] = lt::UiBookLevel{1000, lt::qty_from_int(50)};
    src.bid_count = 1;

    lt::UiBookUpdate dst{};
    std::memcpy(&dst, &src, sizeof(lt::UiBookUpdate));
    CHECK(dst.asset_id == src.asset_id);
    CHECK(dst.bbo.best_bid == 1000);
    CHECK(dst.bid_count == 1);
    CHECK(dst.bids[0].price == 1000);
}

TEST_CASE("UiTokenPosition is trivially copyable") {
    static_assert(std::is_trivially_copyable_v<lt::UiTokenPosition>);
    CHECK(sizeof(lt::UiTokenPosition) > 0);
}

TEST_CASE("UiStateSnapshot position fields default") {
    lt::UiStateSnapshot snap{};
    CHECK(snap.position_count == 0);
    for (int i = 0; i < lt::kMaxUiPositions; ++i) {
        CHECK(snap.positions[i].position == 0);
        CHECK(snap.positions[i].token_id.len == 0);
    }
}

TEST_CASE("kMaxUiPositions constant") {
    CHECK(lt::kMaxUiPositions == 64);
}

}  // TEST_SUITE
