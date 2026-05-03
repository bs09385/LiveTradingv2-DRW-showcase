#include <doctest/doctest.h>

#include "common/market_pair.h"

using namespace lt;

TEST_SUITE("TokenPair.ComplementMath") {

TEST_CASE("complement price uses fixed-point scale") {
    auto p1 = get_complement_price(0);
    REQUIRE(p1.ok());
    CHECK(p1.value == 10000);

    auto p2 = get_complement_price(10000);
    REQUIRE(p2.ok());
    CHECK(p2.value == 0);

    auto p3 = get_complement_price(5200);
    REQUIRE(p3.ok());
    CHECK(p3.value == 4800);
}

TEST_CASE("complement price rejects out-of-range values") {
    CHECK_FALSE(get_complement_price(-1).ok());
    CHECK_FALSE(get_complement_price(10001).ok());
}

TEST_CASE("complement token mapping is symmetric") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("token-up"), AssetId("token-down")));

    auto* down = registry.get_complement_token(AssetId("token-up"));
    REQUIRE(down != nullptr);
    CHECK(*down == AssetId("token-down"));

    auto* up = registry.get_complement_token(AssetId("token-down"));
    REQUIRE(up != nullptr);
    CHECK(*up == AssetId("token-up"));

    auto* cond = registry.condition_for_token(AssetId("token-up"));
    REQUIRE(cond != nullptr);
    CHECK(*cond == AssetId("cond-1"));
}

TEST_CASE("registry rejects invalid or duplicate pairs") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("u1"), AssetId("d1")));
    CHECK_FALSE(registry.add_pair(AssetId("cond-1"), AssetId("u2"), AssetId("d2")));
    CHECK_FALSE(registry.add_pair(AssetId("cond-2"), AssetId("u1"), AssetId("d3")));
    CHECK_FALSE(registry.add_pair(AssetId("cond-3"), AssetId("same"), AssetId("same")));
}

}  // TEST_SUITE

