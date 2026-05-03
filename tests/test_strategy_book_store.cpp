#include "doctest/doctest.h"

#include "book/order_book.h"
#include "events/book_delta.h"
#include "scheduler/strategy_book_store.h"

using namespace lt;

namespace {

BookDelta make_incremental(const char* asset, Price_t price, Side side, Qty_t size) {
    BookDelta delta;
    delta.asset_id = AssetId(asset);
    delta.kind = BookDeltaKind::INCREMENTAL;
    delta.change_count = 1;
    delta.changes[0] = {price, side, size};
    return delta;
}

BookDelta make_snapshot_begin(const char* asset) {
    BookDelta delta;
    delta.asset_id = AssetId(asset);
    delta.kind = BookDeltaKind::SNAPSHOT_BEGIN;
    delta.change_count = 0;
    return delta;
}

BookDelta make_snapshot_chunk(const char* asset) {
    BookDelta delta;
    delta.asset_id = AssetId(asset);
    delta.kind = BookDeltaKind::SNAPSHOT_CHUNK;
    delta.change_count = 0;
    return delta;
}

BookDelta make_snapshot_end(const char* asset) {
    BookDelta delta;
    delta.asset_id = AssetId(asset);
    delta.kind = BookDeltaKind::SNAPSHOT_END;
    delta.change_count = 0;
    return delta;
}

void add_level(BookDelta& delta, Price_t price, Side side, Qty_t size) {
    delta.changes[delta.change_count++] = {price, side, size};
}

}  // namespace

TEST_SUITE("StrategyBookStore") {

// --- Basic operations ---

TEST_CASE("unseeded asset returns nullptr") {
    StrategyBookStore store;
    CHECK(store.book(AssetId("unknown")) == nullptr);
    CHECK(store.bbo(AssetId("unknown")) == nullptr);
    CHECK(!store.is_book_valid(AssetId("unknown")));
}

TEST_CASE("seed creates empty book") {
    StrategyBookStore store;
    store.seed(AssetId("tok_up"));
    CHECK(store.size() == 1);
    CHECK(store.book(AssetId("tok_up")) != nullptr);
    CHECK(store.bbo(AssetId("tok_up")) != nullptr);
    CHECK(store.is_book_valid(AssetId("tok_up")));
}

TEST_CASE("reserve does not create entries") {
    StrategyBookStore store;
    store.reserve(16);
    CHECK(store.size() == 0);
}

// --- Incremental delta application ---

TEST_CASE("incremental delta sets bid level") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    auto delta = make_incremental("tok", 5000, Side::BID, 10);
    store.apply_delta(delta);

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 5000);
    CHECK(b->bbo().bid_size == 10);
}

TEST_CASE("incremental delta sets ask level") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    auto delta = make_incremental("tok", 5100, Side::ASK, 20);
    store.apply_delta(delta);

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_ask == 5100);
    CHECK(b->bbo().ask_size == 20);
}

TEST_CASE("multiple incremental deltas build book") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    store.apply_delta(make_incremental("tok", 4900, Side::BID, 5));
    store.apply_delta(make_incremental("tok", 5100, Side::ASK, 8));
    store.apply_delta(make_incremental("tok", 4800, Side::BID, 3));

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 4900);  // 4900 > 4800
    CHECK(b->bbo().bid_size == 5);
    CHECK(b->bbo().best_ask == 5100);
    CHECK(b->bbo().ask_size == 8);
}

TEST_CASE("incremental delta on unseeded asset is ignored") {
    StrategyBookStore store;
    auto delta = make_incremental("unknown", 5000, Side::BID, 10);
    store.apply_delta(delta);  // should not crash
    CHECK(store.size() == 0);
}

TEST_CASE("incremental delta with zero size removes level") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    store.apply_delta(make_incremental("tok", 5000, Side::BID, 10));
    CHECK(store.book(AssetId("tok"))->bbo().best_bid == 5000);

    // Remove the level
    store.apply_delta(make_incremental("tok", 5000, Side::BID, 0));
    CHECK(store.book(AssetId("tok"))->bbo().best_bid == kInvalidPrice);
}

TEST_CASE("incremental delta with tick_size updates book tick size") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    BookDelta delta;
    delta.asset_id = AssetId("tok");
    delta.kind = BookDeltaKind::INCREMENTAL;
    delta.change_count = 0;
    delta.tick_size = 50;
    store.apply_delta(delta);

    // Tick size is set — verify by checking the book's tick_size
    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->tick_size() == 50);
}

// --- Snapshot chunking ---

TEST_CASE("snapshot BEGIN/END produces correct book") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    // Populate with some initial data
    store.apply_delta(make_incremental("tok", 3000, Side::BID, 99));
    CHECK(store.book(AssetId("tok"))->bbo().best_bid == 3000);

    // Snapshot replaces everything
    auto begin = make_snapshot_begin("tok");
    add_level(begin, 5000, Side::BID, 10);
    add_level(begin, 4900, Side::BID, 20);
    store.apply_delta(begin);
    CHECK(!store.is_book_valid(AssetId("tok")));

    auto end = make_snapshot_end("tok");
    add_level(end, 5100, Side::ASK, 15);
    add_level(end, 5200, Side::ASK, 25);
    store.apply_delta(end);
    CHECK(store.is_book_valid(AssetId("tok")));

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 5000);
    CHECK(b->bbo().bid_size == 10);
    CHECK(b->bbo().best_ask == 5100);
    CHECK(b->bbo().ask_size == 15);

    // Old level at 3000 should be gone (book was cleared)
    Qty_t old_qty = 0;
    b->for_each_bid_n(100, [&](Price_t price, Qty_t qty) {
        if (price == 3000) old_qty = qty;
    });
    CHECK(old_qty == 0);
}

TEST_CASE("snapshot with CHUNK in between") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    auto begin = make_snapshot_begin("tok");
    add_level(begin, 5000, Side::BID, 10);
    store.apply_delta(begin);
    CHECK(!store.is_book_valid(AssetId("tok")));

    auto chunk = make_snapshot_chunk("tok");
    add_level(chunk, 4900, Side::BID, 5);
    add_level(chunk, 5100, Side::ASK, 8);
    store.apply_delta(chunk);
    CHECK(!store.is_book_valid(AssetId("tok")));

    auto end = make_snapshot_end("tok");
    add_level(end, 5200, Side::ASK, 12);
    store.apply_delta(end);
    CHECK(store.is_book_valid(AssetId("tok")));

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 5000);
    CHECK(b->bbo().best_ask == 5100);
}

TEST_CASE("partial snapshot (BEGIN without END) marks book invalid") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    // Initially valid
    CHECK(store.is_book_valid(AssetId("tok")));

    auto begin = make_snapshot_begin("tok");
    add_level(begin, 5000, Side::BID, 10);
    store.apply_delta(begin);

    // Still invalid — no END received
    CHECK(!store.is_book_valid(AssetId("tok")));

    // Book data is there but marked invalid
    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
}

TEST_CASE("second snapshot replaces first") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    // First snapshot
    auto begin1 = make_snapshot_begin("tok");
    add_level(begin1, 5000, Side::BID, 10);
    store.apply_delta(begin1);
    auto end1 = make_snapshot_end("tok");
    add_level(end1, 5100, Side::ASK, 15);
    store.apply_delta(end1);
    CHECK(store.book(AssetId("tok"))->bbo().best_bid == 5000);

    // Second snapshot clears the first
    auto begin2 = make_snapshot_begin("tok");
    add_level(begin2, 6000, Side::BID, 30);
    store.apply_delta(begin2);
    auto end2 = make_snapshot_end("tok");
    add_level(end2, 6100, Side::ASK, 40);
    store.apply_delta(end2);

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 6000);
    CHECK(b->bbo().bid_size == 30);
    CHECK(b->bbo().best_ask == 6100);
    CHECK(b->bbo().ask_size == 40);
}

// --- Multiple assets ---

TEST_CASE("multiple assets are independent") {
    StrategyBookStore store;
    store.seed(AssetId("tok_a"));
    store.seed(AssetId("tok_b"));

    store.apply_delta(make_incremental("tok_a", 5000, Side::BID, 10));
    store.apply_delta(make_incremental("tok_b", 6000, Side::BID, 20));

    CHECK(store.book(AssetId("tok_a"))->bbo().best_bid == 5000);
    CHECK(store.book(AssetId("tok_b"))->bbo().best_bid == 6000);
    CHECK(store.size() == 2);
}

TEST_CASE("snapshot on one asset does not affect another") {
    StrategyBookStore store;
    store.seed(AssetId("tok_a"));
    store.seed(AssetId("tok_b"));

    // Set up both
    store.apply_delta(make_incremental("tok_a", 5000, Side::BID, 10));
    store.apply_delta(make_incremental("tok_b", 6000, Side::BID, 20));

    // Snapshot on tok_a only
    auto begin = make_snapshot_begin("tok_a");
    store.apply_delta(begin);

    CHECK(!store.is_book_valid(AssetId("tok_a")));
    CHECK(store.is_book_valid(AssetId("tok_b")));
    CHECK(store.book(AssetId("tok_b"))->bbo().best_bid == 6000);
}

// --- for_each_book ---

TEST_CASE("for_each_book visits all books") {
    StrategyBookStore store;
    store.seed(AssetId("tok_a"));
    store.seed(AssetId("tok_b"));
    store.seed(AssetId("tok_c"));

    int count = 0;
    store.for_each_book([&](const AssetId&, const OrderBook&) {
        ++count;
    });
    CHECK(count == 3);
}

// --- Multi-change deltas ---

TEST_CASE("delta with multiple changes applies all") {
    StrategyBookStore store;
    store.seed(AssetId("tok"));

    BookDelta delta;
    delta.asset_id = AssetId("tok");
    delta.kind = BookDeltaKind::INCREMENTAL;
    delta.change_count = 3;
    delta.changes[0] = {4900, Side::BID, 10};
    delta.changes[1] = {5000, Side::BID, 20};
    delta.changes[2] = {5100, Side::ASK, 15};
    store.apply_delta(delta);

    const auto* b = store.book(AssetId("tok"));
    REQUIRE(b != nullptr);
    CHECK(b->bbo().best_bid == 5000);  // highest bid
    CHECK(b->bbo().bid_size == 20);
    CHECK(b->bbo().best_ask == 5100);
    CHECK(b->bbo().ask_size == 15);
}

}  // TEST_SUITE
