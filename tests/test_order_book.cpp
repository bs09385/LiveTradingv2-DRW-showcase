#include <doctest/doctest.h>

#include "book/order_book.h"
#include "events/market_events.h"

using namespace lt;

static BookSnapshot make_snapshot() {
    BookSnapshot snap;
    snap.asset_id = AssetId("test_asset");

    // Bids: 0.52@100, 0.51@200, 0.50@500
    snap.bids[0] = {5200, qty_from_int(100)};
    snap.bids[1] = {5100, qty_from_int(200)};
    snap.bids[2] = {5000, qty_from_int(500)};
    snap.bid_count = 3;

    // Asks: 0.53@120, 0.54@300, 0.55@250
    snap.asks[0] = {5300, qty_from_int(120)};
    snap.asks[1] = {5400, qty_from_int(300)};
    snap.asks[2] = {5500, qty_from_int(250)};
    snap.ask_count = 3;

    return snap;
}

TEST_SUITE("OrderBook") {
    TEST_CASE("initial state is EMPTY") {
        OrderBook book;
        CHECK(book.status() == BookStatus::EMPTY);
        CHECK(book.bbo().best_bid == kInvalidPrice);
        CHECK(book.bbo().best_ask == kInvalidPrice);
    }

    TEST_CASE("apply_snapshot populates book") {
        OrderBook book;
        auto snap = make_snapshot();
        auto err = book.apply_snapshot(snap);

        CHECK(err == ErrorCode::OK);
        CHECK(book.status() == BookStatus::LIVE);

        // BBO
        CHECK(book.bbo().best_bid == 5200);
        CHECK(book.bbo().best_ask == 5300);
        CHECK(book.bbo().bid_size == qty_from_int(100));
        CHECK(book.bbo().ask_size == qty_from_int(120));

        // Individual levels
        CHECK(book.bid_qty_at(5200) == qty_from_int(100));
        CHECK(book.bid_qty_at(5100) == qty_from_int(200));
        CHECK(book.bid_qty_at(5000) == qty_from_int(500));
        CHECK(book.ask_qty_at(5300) == qty_from_int(120));
        CHECK(book.ask_qty_at(5400) == qty_from_int(300));
        CHECK(book.ask_qty_at(5500) == qty_from_int(250));
    }

    TEST_CASE("snapshot clears previous data") {
        OrderBook book;
        auto snap = make_snapshot();
        book.apply_snapshot(snap);

        // Apply a different snapshot
        BookSnapshot snap2;
        snap2.asset_id = AssetId("test_asset");
        snap2.bids[0] = {3000, qty_from_int(50)};
        snap2.bid_count = 1;
        snap2.asks[0] = {7000, qty_from_int(60)};
        snap2.ask_count = 1;

        book.apply_snapshot(snap2);
        CHECK(book.bbo().best_bid == 3000);
        CHECK(book.bbo().best_ask == 7000);
        CHECK(book.bid_qty_at(5200) == 0);  // old level cleared
    }

    TEST_CASE("apply_price_change - modify level") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        AssetPriceChange changes;
        changes.asset_id = AssetId("test_asset");
        changes.changes[0] = {5200, Side::BID, qty_from_int(150)};  // update bid at 0.52
        changes.change_count = 1;

        auto err = book.apply_price_change(changes);
        CHECK(err == ErrorCode::OK);
        CHECK(book.bid_qty_at(5200) == qty_from_int(150));
        CHECK(book.bbo().best_bid == 5200);
        CHECK(book.bbo().bid_size == qty_from_int(150));
    }

    TEST_CASE("apply_price_change - remove level") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        AssetPriceChange changes;
        changes.asset_id = AssetId("test_asset");
        changes.changes[0] = {5200, Side::BID, 0};  // remove best bid
        changes.change_count = 1;

        auto err = book.apply_price_change(changes);
        CHECK(err == ErrorCode::OK);
        CHECK(book.bid_qty_at(5200) == 0);
        CHECK(book.bbo().best_bid == 5100);  // next best
        CHECK(book.bbo().bid_size == qty_from_int(200));
    }

    TEST_CASE("apply_price_change - add new level") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        AssetPriceChange changes;
        changes.asset_id = AssetId("test_asset");
        changes.changes[0] = {5250, Side::BID, qty_from_int(75)};  // new level between bid/ask
        changes.change_count = 1;

        auto err = book.apply_price_change(changes);
        CHECK(err == ErrorCode::OK);
        CHECK(book.bid_qty_at(5250) == qty_from_int(75));
        CHECK(book.bbo().best_bid == 5250);  // new best bid
    }

    TEST_CASE("apply_price_change removing final levels sets EMPTY status") {
        OrderBook book;
        BookSnapshot snap;
        snap.asset_id = AssetId("test_asset");
        snap.bids[0] = {5200, qty_from_int(100)};
        snap.bid_count = 1;
        auto err = book.apply_snapshot(snap);
        REQUIRE(err == ErrorCode::OK);
        CHECK(book.status() == BookStatus::LIVE);

        AssetPriceChange changes;
        changes.asset_id = AssetId("test_asset");
        changes.changes[0] = {5200, Side::BID, 0};
        changes.change_count = 1;

        err = book.apply_price_change(changes);
        CHECK(err == ErrorCode::OK);
        CHECK(book.status() == BookStatus::EMPTY);
        CHECK(book.bbo().best_bid == kInvalidPrice);
        CHECK(book.bbo().best_ask == kInvalidPrice);
    }

    TEST_CASE("BBO cache updates correctly") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        CHECK(book.bbo().spread() == 100);  // 0.53 - 0.52 = 0.01
        CHECK_FALSE(book.bbo().crossed());
    }

    TEST_CASE("crossed book detection") {
        OrderBook book;
        BookSnapshot snap;
        snap.bids[0] = {5500, qty_from_int(100)};  // bid above ask
        snap.bid_count = 1;
        snap.asks[0] = {5300, qty_from_int(100)};
        snap.ask_count = 1;

        book.apply_snapshot(snap);
        CHECK(book.status() == BookStatus::CROSSED);
        CHECK(book.bbo().crossed());
    }

    TEST_CASE("tick size change") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        CHECK(book.tick_size() == 100);
        book.set_tick_size(10);
        CHECK(book.tick_size() == 10);
        // Book data unchanged
        CHECK(book.bbo().best_bid == 5200);
    }

    TEST_CASE("rejects negative quantity") {
        OrderBook book;
        BookSnapshot snap;
        snap.bids[0] = {5200, -10};
        snap.bid_count = 1;

        auto err = book.apply_snapshot(snap);
        CHECK(err == ErrorCode::BOOK_NEGATIVE_QTY);
    }

    TEST_CASE("rejects invalid price") {
        OrderBook book;
        BookSnapshot snap;
        snap.bids[0] = {-1, qty_from_int(100)};
        snap.bid_count = 1;

        auto err = book.apply_snapshot(snap);
        CHECK(err == ErrorCode::BOOK_INVALID_PRICE);
    }

    TEST_CASE("invalid snapshot does not leave partial ladder state") {
        OrderBook book;
        BookSnapshot snap;
        snap.bids[0] = {5200, qty_from_int(100)};
        snap.bids[1] = {20000, qty_from_int(50)};  // invalid
        snap.bid_count = 2;

        auto err = book.apply_snapshot(snap);
        CHECK(err == ErrorCode::BOOK_INVALID_PRICE);
        CHECK(book.status() == BookStatus::EMPTY);
        CHECK(book.total_bid_qty() == 0);
    }

    TEST_CASE("for_each iteration") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        int bid_count = 0;
        book.for_each_bid([&](Price_t p, Qty_t q) {
            bid_count++;
            CHECK(q > 0);
        });
        CHECK(bid_count == 3);

        int ask_count = 0;
        book.for_each_ask([&](Price_t p, Qty_t q) {
            ask_count++;
            CHECK(q > 0);
        });
        CHECK(ask_count == 3);
    }

    TEST_CASE("total quantity") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());

        CHECK(book.total_bid_qty() == qty_from_int(800));   // 100 + 200 + 500
        CHECK(book.total_ask_qty() == qty_from_int(670));   // 120 + 300 + 250
    }

    TEST_CASE("clear resets everything") {
        OrderBook book;
        book.apply_snapshot(make_snapshot());
        book.clear();

        CHECK(book.status() == BookStatus::EMPTY);
        CHECK(book.bbo().best_bid == kInvalidPrice);
        CHECK(book.total_bid_qty() == 0);
    }
}
