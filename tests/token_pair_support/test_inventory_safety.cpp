#include <doctest/doctest.h>

#include "common/token_inventory.h"
#include "exec/inventory_safety.h"

using namespace lt;

TEST_SUITE("TokenPair.InventorySafety") {

TEST_CASE("SELL requires sufficient token inventory") {
    TokenInventory inventory;
    inventory.set_position(AssetId("up"), 20);

    ExecIntent intent;
    intent.side = Side::ASK;
    intent.asset_id = AssetId("up");
    intent.size = 30;

    auto result = check_inventory_for_intent(intent, &inventory);
    CHECK_FALSE(result.allowed);
    CHECK(result.available == 20);

    intent.size = 15;
    result = check_inventory_for_intent(intent, &inventory);
    CHECK(result.allowed);
    CHECK(result.available == 20);
}

TEST_CASE("BUY does not require pre-owned inventory") {
    TokenInventory inventory;

    ExecIntent intent;
    intent.side = Side::BID;
    intent.asset_id = AssetId("down");
    intent.size = 100;

    auto result = check_inventory_for_intent(intent, &inventory);
    CHECK(result.allowed);
    CHECK(result.available == 0);
}

}  // TEST_SUITE

