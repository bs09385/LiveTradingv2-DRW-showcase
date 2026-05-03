#include <doctest/doctest.h>

#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "logger/metrics.h"
#include "scheduler/quote_planner.h"

using namespace lt;

namespace {

ExecutionIntent make_sell_intent(const char* token, Price_t price, Qty_t qty) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.asset_id = AssetId(token);
    intent.price = price;
    intent.qty = qty;
    intent.intent_id = 7;
    return intent;
}

}  // namespace

TEST_SUITE("TokenPair.QuotePlanner") {

TEST_CASE("sell intent is preserved exactly") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

    TokenInventory inventory;
    inventory.set_position(AssetId("up"), qty_from_int(100));

    QuotePlanner planner(&registry, &inventory);
    auto in = make_sell_intent("up", 6200, qty_from_int(25));
    auto out = planner.plan(in);

    REQUIRE(out.count == 1);
    CHECK(out.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(out.intents[0].asset_id == AssetId("up"));
    CHECK(out.intents[0].price == 6200);
    CHECK(out.intents[0].qty == qty_from_int(25));

}

TEST_CASE("sell intent with zero inventory is still preserved exactly") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

    TokenInventory inventory;
    inventory.set_position(AssetId("up"), 0);

    QuotePlanner planner(&registry, &inventory);
    auto in = make_sell_intent("up", 6200, qty_from_int(25));
    auto out = planner.plan(in);

    REQUIRE(out.count == 1);
    CHECK(out.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(out.intents[0].asset_id == AssetId("up"));
    CHECK(out.intents[0].price == 6200);
    CHECK(out.intents[0].qty == qty_from_int(25));

}

TEST_CASE("buy intent is not rewritten") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

    TokenInventory inventory;
    QuotePlanner planner(&registry, &inventory);

    ExecutionIntent in;
    in.action = IntentAction::WOULD_PLACE_BID;
    in.asset_id = AssetId("up");
    in.price = 5100;
    in.qty = qty_from_int(12);

    auto out = planner.plan(in);
    REQUIRE(out.count == 1);
    CHECK(out.intents[0].action == IntentAction::WOULD_PLACE_BID);
    CHECK(out.intents[0].asset_id == AssetId("up"));
    CHECK(out.intents[0].price == 5100);

}

TEST_CASE("pass-through planner does not increment conversion metric") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

    TokenInventory inventory;
    inventory.set_position(AssetId("up"), 0);

    Metrics metrics;
    QuotePlanner planner(&registry, &inventory, &metrics);

    auto in = make_sell_intent("up", 6200, qty_from_int(25));
    auto out = planner.plan(in);

    REQUIRE(out.count == 1);
    CHECK(out.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(metrics.get(MetricId::SCHED_QUOTE_CONVERSIONS) == 0);
}

}  // TEST_SUITE
