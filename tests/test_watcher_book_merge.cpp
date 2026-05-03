#include "doctest/doctest.h"

#include "ui_bridge/watcher_book_store.h"

using namespace lt;

TEST_SUITE("WatcherBookStore") {

TEST_CASE("empty store has no books") {
    WatcherBookStore store;
    CHECK_FALSE(store.has_book("token_up"));
    CHECK_FALSE(store.has_book("token_down"));
}

TEST_CASE("apply_book_snapshot stores levels") {
    WatcherBookStore store;
    WatcherBookLevel bids[] = {{5200, qty_from_int(100)}, {5100, qty_from_int(200)}, {5000, qty_from_int(50)}};
    WatcherBookLevel asks[] = {{5300, qty_from_int(75)}, {5400, qty_from_int(150)}};

    store.apply_book_snapshot("token1", bids, 3, asks, 2);

    CHECK(store.has_book("token1"));
}

TEST_CASE("apply_price_change updates existing level") {
    WatcherBookStore store;
    WatcherBookLevel bids[] = {{5200, qty_from_int(100)}};
    store.apply_book_snapshot("t1", bids, 1, nullptr, 0);

    store.apply_price_change("t1", 5200, qty_from_int(250), true);

    // Build a simple ladder to verify
    auto ladder = store.build_merged_ladder("t1", "nonexistent", 100);
    REQUIRE(ladder.buy_levels.size() == 1);
    CHECK(ladder.buy_levels[0].price == 5200);
    CHECK(ladder.buy_levels[0].size == qty_from_int(250));
}

TEST_CASE("apply_price_change inserts new level") {
    WatcherBookStore store;
    WatcherBookLevel bids[] = {{5200, qty_from_int(100)}};
    store.apply_book_snapshot("t1", bids, 1, nullptr, 0);

    store.apply_price_change("t1", 5100, qty_from_int(50), true);

    auto ladder = store.build_merged_ladder("t1", "nonexistent", 100);
    REQUIRE(ladder.buy_levels.size() == 2);
    CHECK(ladder.buy_levels[0].price == 5200);  // highest first
    CHECK(ladder.buy_levels[1].price == 5100);
}

TEST_CASE("apply_price_change removes level when size is 0") {
    WatcherBookStore store;
    WatcherBookLevel bids[] = {{5200, qty_from_int(100)}, {5100, qty_from_int(50)}};
    store.apply_book_snapshot("t1", bids, 2, nullptr, 0);

    store.apply_price_change("t1", 5200, 0, true);

    auto ladder = store.build_merged_ladder("t1", "nonexistent", 100);
    REQUIRE(ladder.buy_levels.size() == 1);
    CHECK(ladder.buy_levels[0].price == 5100);
}

TEST_CASE("clear_book removes data") {
    WatcherBookStore store;
    WatcherBookLevel bids[] = {{5200, qty_from_int(100)}};
    store.apply_book_snapshot("t1", bids, 1, nullptr, 0);
    CHECK(store.has_book("t1"));

    store.clear_book("t1");
    CHECK_FALSE(store.has_book("t1"));
}

TEST_CASE("merged ladder: Up-only") {
    WatcherBookStore store;
    WatcherBookLevel up_bids[] = {{5200, qty_from_int(100)}, {5100, qty_from_int(200)}};
    WatcherBookLevel up_asks[] = {{5300, qty_from_int(75)}, {5400, qty_from_int(150)}};
    store.apply_book_snapshot("up", up_bids, 2, up_asks, 2);

    auto ladder = store.build_merged_ladder("up", "nonexistent", 100);

    REQUIRE(ladder.buy_levels.size() == 2);
    CHECK(ladder.buy_levels[0].price == 5200);
    CHECK(ladder.buy_levels[0].size == qty_from_int(100));
    CHECK(ladder.buy_levels[1].price == 5100);
    CHECK(ladder.buy_levels[1].size == qty_from_int(200));

    REQUIRE(ladder.sell_levels.size() == 2);
    CHECK(ladder.sell_levels[0].price == 5300);
    CHECK(ladder.sell_levels[0].size == qty_from_int(75));
    CHECK(ladder.sell_levels[1].price == 5400);
    CHECK(ladder.sell_levels[1].size == qty_from_int(150));
}

TEST_CASE("merged ladder: complement transform") {
    // Down ask at price P -> buy liquidity at (10000 - P) in Up space
    // Down bid at price P -> sell liquidity at (10000 - P) in Up space
    WatcherBookStore store;

    // Down book: bid at 4800 (= sell at 5200 in Up), ask at 4700 (= buy at 5300 in Up)
    WatcherBookLevel down_bids[] = {{4800, qty_from_int(50)}};
    WatcherBookLevel down_asks[] = {{4700, qty_from_int(80)}};
    store.apply_book_snapshot("down", down_bids, 1, down_asks, 1);

    auto ladder = store.build_merged_ladder("nonexistent", "down", 100);

    // Buy levels: complement of Down asks: 10000 - 4700 = 5300
    REQUIRE(ladder.buy_levels.size() == 1);
    CHECK(ladder.buy_levels[0].price == 5300);
    CHECK(ladder.buy_levels[0].size == qty_from_int(80));

    // Sell levels: complement of Down bids: 10000 - 4800 = 5200
    REQUIRE(ladder.sell_levels.size() == 1);
    CHECK(ladder.sell_levels[0].price == 5200);
    CHECK(ladder.sell_levels[0].size == qty_from_int(50));
}

TEST_CASE("merged ladder: Up + Down merge at same price") {
    WatcherBookStore store;

    // Up: bid at 5200 = 100
    WatcherBookLevel up_bids[] = {{5200, qty_from_int(100)}};
    store.apply_book_snapshot("up", up_bids, 1, nullptr, 0);

    // Down: ask at 4800, complement = 5200. Same price as Up bid.
    WatcherBookLevel down_asks[] = {{4800, qty_from_int(50)}};
    store.apply_book_snapshot("down", nullptr, 0, down_asks, 1);

    auto ladder = store.build_merged_ladder("up", "down", 100);

    // Buy should merge: 5200 = 100 (Up bid) + 50 (Down ask complement)
    REQUIRE(ladder.buy_levels.size() == 1);
    CHECK(ladder.buy_levels[0].price == 5200);
    CHECK(ladder.buy_levels[0].size == qty_from_int(150));
}

TEST_CASE("merged ladder: Up + Down at different prices") {
    WatcherBookStore store;

    // Up: bid at 5200 = 100, ask at 5300 = 75
    WatcherBookLevel up_bids[] = {{5200, qty_from_int(100)}};
    WatcherBookLevel up_asks[] = {{5300, qty_from_int(75)}};
    store.apply_book_snapshot("up", up_bids, 1, up_asks, 1);

    // Down: bid at 4600 (comp=5400, sell), ask at 4900 (comp=5100, buy)
    WatcherBookLevel down_bids[] = {{4600, qty_from_int(30)}};
    WatcherBookLevel down_asks[] = {{4900, qty_from_int(40)}};
    store.apply_book_snapshot("down", down_bids, 1, down_asks, 1);

    auto ladder = store.build_merged_ladder("up", "down", 100);

    // Buy levels: 5200 (Up bid) + 5100 (Down ask comp), sorted desc
    REQUIRE(ladder.buy_levels.size() == 2);
    CHECK(ladder.buy_levels[0].price == 5200);
    CHECK(ladder.buy_levels[0].size == qty_from_int(100));
    CHECK(ladder.buy_levels[1].price == 5100);
    CHECK(ladder.buy_levels[1].size == qty_from_int(40));

    // Sell levels: 5300 (Up ask) + 5400 (Down bid comp), sorted asc
    REQUIRE(ladder.sell_levels.size() == 2);
    CHECK(ladder.sell_levels[0].price == 5300);
    CHECK(ladder.sell_levels[0].size == qty_from_int(75));
    CHECK(ladder.sell_levels[1].price == 5400);
    CHECK(ladder.sell_levels[1].size == qty_from_int(30));
}

TEST_CASE("merged ladder: max_depth truncation") {
    WatcherBookStore store;
    WatcherBookLevel bids[5];
    WatcherBookLevel asks[5];
    for (int i = 0; i < 5; ++i) {
        bids[i] = {static_cast<Price_t>(5000 - i * 100), qty_from_int(10)};
        asks[i] = {static_cast<Price_t>(5100 + i * 100), qty_from_int(10)};
    }
    store.apply_book_snapshot("up", bids, 5, asks, 5);

    auto ladder = store.build_merged_ladder("up", "nonexistent", 3);
    CHECK(ladder.buy_levels.size() == 3);
    CHECK(ladder.sell_levels.size() == 3);
}

TEST_CASE("merged ladder: both empty") {
    WatcherBookStore store;
    auto ladder = store.build_merged_ladder("up", "down", 100);
    CHECK(ladder.buy_levels.empty());
    CHECK(ladder.sell_levels.empty());
}

TEST_CASE("apply_book_snapshot sorts unsorted input") {
    WatcherBookStore store;
    // Bids out of order (should be sorted descending)
    WatcherBookLevel bids[] = {{5000, qty_from_int(10)}, {5200, qty_from_int(20)}, {5100, qty_from_int(15)}};
    store.apply_book_snapshot("t1", bids, 3, nullptr, 0);

    auto ladder = store.build_merged_ladder("t1", "nonexistent", 100);
    REQUIRE(ladder.buy_levels.size() == 3);
    CHECK(ladder.buy_levels[0].price == 5200);
    CHECK(ladder.buy_levels[1].price == 5100);
    CHECK(ladder.buy_levels[2].price == 5000);
}

}  // TEST_SUITE
