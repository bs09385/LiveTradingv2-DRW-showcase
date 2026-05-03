#include <doctest/doctest.h>

#include "common/clock.h"
#include "events/event_variant.h"
#include "logger/metrics.h"
#include "parser/market_message_parser.h"
#include "queue/spsc_queue.h"
#include "state/market_state_store.h"

using namespace lt;

TEST_SUITE("Integration: T0 Pipeline") {
    TEST_CASE("raw JSON -> parse -> book -> queue end-to-end") {
        // Setup components
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);
        MarketMessageParser parser;
        SeqNum_t seq = 0;

        // Step 1: Book snapshot
        const char* snapshot_json = R"({
            "event_type": "book",
            "asset_id": "test_asset_123",
            "market": "0xabc",
            "timestamp": "1700000000",
            "bids": [
                {"price": "0.52", "size": "100"},
                {"price": "0.51", "size": "200"},
                {"price": "0.50", "size": "500"}
            ],
            "asks": [
                {"price": "0.53", "size": "120"},
                {"price": "0.54", "size": "300"}
            ]
        })";

        auto recv_ts = SteadyClock::now();
        MarketEvent event;
        auto err = parser.parse(snapshot_json, recv_ts, ++seq, event);
        CHECK(err == ErrorCode::OK);
        store.apply(event);

        // Verify book state
        auto* state = store.get_state(AssetId("test_asset_123"));
        REQUIRE(state != nullptr);
        CHECK(state->book.bbo().best_bid == 5200);
        CHECK(state->book.bbo().best_ask == 5300);
        CHECK(state->book.status() == BookStatus::LIVE);

        // Verify notification
        auto notif = queue.try_pop();
        REQUIRE(notif.has_value());
        CHECK(notif->kind == NotificationKind::BOOK_SNAPSHOT);
        CHECK(notif->bbo.best_bid == 5200);
        CHECK(notif->bbo.best_ask == 5300);

        // Step 2: Price change
        const char* delta_json = R"({
            "event_type": "price_change",
            "market": "0xabc",
            "timestamp": "1700000001",
            "price_changes": [
                {
                    "asset_id": "test_asset_123",
                    "best_bid": "0.52",
                    "best_ask": "0.53",
                    "changes": [
                        {"price": "0.52", "side": "BUY", "size": "250"},
                        {"price": "0.53", "side": "SELL", "size": "80"}
                    ]
                }
            ]
        })";

        MarketEvent event2;
        err = parser.parse(delta_json, SteadyClock::now(), ++seq, event2);
        CHECK(err == ErrorCode::OK);
        store.apply(event2);

        CHECK(state->book.bid_qty_at(5200) == qty_from_int(250));
        CHECK(state->book.ask_qty_at(5300) == qty_from_int(80));

        auto notif2 = queue.try_pop();
        REQUIRE(notif2.has_value());
        CHECK(notif2->kind == NotificationKind::PRICE_CHANGE);

        // Step 3: Last trade price
        const char* trade_json = R"({
            "event_type": "last_trade_price",
            "asset_id": "test_asset_123",
            "market": "0xabc",
            "price": "0.52",
            "size": "30",
            "side": "BUY",
            "timestamp": "1700000002"
        })";

        MarketEvent event3;
        err = parser.parse(trade_json, SteadyClock::now(), ++seq, event3);
        CHECK(err == ErrorCode::OK);
        store.apply(event3);

        CHECK(state->last_trade_price == 5200);
        CHECK(state->last_trade_size == qty_from_int(30));

        auto notif3 = queue.try_pop();
        REQUIRE(notif3.has_value());
        CHECK(notif3->kind == NotificationKind::LAST_TRADE);

        // Step 4: PONG handling
        MarketEvent event4;
        err = parser.parse("PONG", SteadyClock::now(), ++seq, event4);
        CHECK(err == ErrorCode::PONG_MESSAGE);

        // Step 5: Malformed message
        MarketEvent event5;
        err = parser.parse("{invalid json", SteadyClock::now(), ++seq, event5);
        CHECK(err != ErrorCode::OK);

        // Verify metrics
        CHECK(metrics.get(MetricId::BOOK_SNAPSHOTS) == 1);
        CHECK(metrics.get(MetricId::BOOK_UPDATES) == 1);
        CHECK(metrics.get(MetricId::QUEUE_PUSHES) >= 3);
    }

    TEST_CASE("multi-asset pipeline") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);
        MarketMessageParser parser;
        SeqNum_t seq = 0;

        // Snapshot for asset A
        const char* snap_a = R"({
            "event_type": "book",
            "asset_id": "asset_a",
            "market": "0x1",
            "timestamp": "1700000000",
            "bids": [{"price": "0.40", "size": "100"}],
            "asks": [{"price": "0.60", "size": "100"}]
        })";

        // Snapshot for asset B
        const char* snap_b = R"({
            "event_type": "book",
            "asset_id": "asset_b",
            "market": "0x2",
            "timestamp": "1700000000",
            "bids": [{"price": "0.70", "size": "50"}],
            "asks": [{"price": "0.80", "size": "50"}]
        })";

        MarketEvent ev1, ev2;
        parser.parse(snap_a, SteadyClock::now(), ++seq, ev1);
        store.apply(ev1);
        parser.parse(snap_b, SteadyClock::now(), ++seq, ev2);
        store.apply(ev2);

        CHECK(store.asset_count() == 2);

        auto* sa = store.get_state(AssetId("asset_a"));
        auto* sb = store.get_state(AssetId("asset_b"));
        REQUIRE(sa != nullptr);
        REQUIRE(sb != nullptr);
        CHECK(sa->book.bbo().best_bid == 4000);
        CHECK(sb->book.bbo().best_bid == 7000);

        // Batch price change affecting both assets
        const char* batch_delta = R"({
            "event_type": "price_change",
            "market": "0x1",
            "timestamp": "1700000001",
            "price_changes": [
                {
                    "asset_id": "asset_a",
                    "best_bid": "0.41",
                    "best_ask": "0.60",
                    "changes": [{"price": "0.41", "side": "BUY", "size": "200"}]
                },
                {
                    "asset_id": "asset_b",
                    "best_bid": "0.70",
                    "best_ask": "0.79",
                    "changes": [{"price": "0.79", "side": "SELL", "size": "75"}]
                }
            ]
        })";

        MarketEvent ev3;
        parser.parse(batch_delta, SteadyClock::now(), ++seq, ev3);
        store.apply(ev3);

        CHECK(sa->book.bbo().best_bid == 4100);
        CHECK(sb->book.ask_qty_at(7900) == qty_from_int(75));
    }
}
