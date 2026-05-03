#include <doctest/doctest.h>

#include "events/event_variant.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "state/market_state_store.h"

using namespace lt;

TEST_SUITE("MarketStateStore") {
    TEST_CASE("book snapshot creates state and emits notification") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        BookSnapshot snap;
        snap.asset_id = AssetId("asset1");
        snap.bids[0] = {5200, qty_from_int(100)};
        snap.bid_count = 1;
        snap.asks[0] = {5300, qty_from_int(120)};
        snap.ask_count = 1;

        MarketEvent event;
        event.payload = snap;
        event.recv_ts = 100000;
        event.seq = 1;

        store.apply(event);

        // Check state was created
        CHECK(store.asset_count() == 1);
        auto* state = store.get_state(AssetId("asset1"));
        REQUIRE(state != nullptr);
        CHECK(state->book.bbo().best_bid == 5200);
        CHECK(state->book.bbo().best_ask == 5300);
        CHECK(state->snapshot_count == 1);

        // Check notification was emitted
        auto notif = queue.try_pop();
        REQUIRE(notif.has_value());
        CHECK(notif->kind == NotificationKind::BOOK_SNAPSHOT);
        CHECK(notif->bbo.best_bid == 5200);
        CHECK(notif->bbo.best_ask == 5300);
        CHECK(notif->recv_ts == 100000);
        CHECK(notif->seq == 1);
    }

    TEST_CASE("price change updates book") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        // First apply a snapshot
        BookSnapshot snap;
        snap.asset_id = AssetId("asset1");
        snap.bids[0] = {5200, qty_from_int(100)};
        snap.bid_count = 1;
        snap.asks[0] = {5300, qty_from_int(120)};
        snap.ask_count = 1;

        MarketEvent snap_event;
        snap_event.payload = snap;
        snap_event.recv_ts = 100000;
        snap_event.seq = 1;
        store.apply(snap_event);
        queue.try_pop();  // consume snapshot notification

        // Now apply price change
        PriceChangeEvent pce;
        pce.asset_count = 1;
        pce.asset_changes[0].asset_id = AssetId("asset1");
        pce.asset_changes[0].changes[0] = {5200, Side::BID, qty_from_int(200)};
        pce.asset_changes[0].change_count = 1;

        MarketEvent pce_event;
        pce_event.payload = pce;
        pce_event.recv_ts = 100001;
        pce_event.seq = 2;
        store.apply(pce_event);

        auto* state = store.get_state(AssetId("asset1"));
        REQUIRE(state != nullptr);
        CHECK(state->book.bid_qty_at(5200) == qty_from_int(200));
        CHECK(state->update_count == 1);

        auto notif = queue.try_pop();
        REQUIRE(notif.has_value());
        CHECK(notif->kind == NotificationKind::PRICE_CHANGE);
    }

    TEST_CASE("multi-asset state") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        BookSnapshot snap1;
        snap1.asset_id = AssetId("asset_a");
        snap1.bids[0] = {5200, qty_from_int(100)};
        snap1.bid_count = 1;

        BookSnapshot snap2;
        snap2.asset_id = AssetId("asset_b");
        snap2.bids[0] = {3000, qty_from_int(50)};
        snap2.bid_count = 1;

        MarketEvent event1, event2;
        event1.payload = snap1;
        event2.payload = snap2;

        store.apply(event1);
        store.apply(event2);

        CHECK(store.asset_count() == 2);
        CHECK(store.get_state(AssetId("asset_a")) != nullptr);
        CHECK(store.get_state(AssetId("asset_b")) != nullptr);
    }

    TEST_CASE("queue overflow increments metric") {
        SpscQueue<MarketNotification> queue(2);  // very small queue
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        // Fill the queue beyond capacity
        for (int i = 0; i < 10; ++i) {
            BookSnapshot snap;
            snap.asset_id = AssetId("asset1");
            snap.bids[0] = {5200, qty_from_int(i)};
            snap.bid_count = 1;

            MarketEvent event;
            event.payload = snap;
            store.apply(event);
        }

        CHECK(metrics.get(MetricId::QUEUE_OVERFLOWS) > 0);
    }

    TEST_CASE("tick size change updates state") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        TickSizeChangeEvent tsc;
        tsc.asset_id = AssetId("asset1");
        tsc.old_tick_size = 100;
        tsc.new_tick_size = 10;

        MarketEvent event;
        event.payload = tsc;
        store.apply(event);

        auto* state = store.get_state(AssetId("asset1"));
        REQUIRE(state != nullptr);
        CHECK(state->tick_size == 10);
    }

    TEST_CASE("invalid tick size change is rejected") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        TickSizeChangeEvent tsc;
        tsc.asset_id = AssetId("asset1");
        tsc.new_tick_size = 0;

        MarketEvent event;
        event.payload = tsc;
        store.apply(event);

        CHECK(store.get_state(AssetId("asset1")) == nullptr);
        CHECK(metrics.get(MetricId::BOOK_ERRORS) == 1);
    }

    TEST_CASE("last trade price updates state") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        LastTradePriceEvent ltp;
        ltp.asset_id = AssetId("asset1");
        ltp.price = 5200;
        ltp.size = qty_from_int(50);
        ltp.side = Side::BID;

        MarketEvent event;
        event.payload = ltp;
        store.apply(event);

        auto* state = store.get_state(AssetId("asset1"));
        REQUIRE(state != nullptr);
        CHECK(state->last_trade_price == 5200);
        CHECK(state->last_trade_size == qty_from_int(50));
    }

    TEST_CASE("best_bid_ask is validation-only (no state update or notification)") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        MarketStateStore store(queue, metrics);

        BestBidAskEvent bba;
        bba.asset_id = AssetId("asset1");
        bba.best_bid = 5200;
        bba.best_ask = 5300;

        MarketEvent event;
        event.payload = bba;
        event.recv_ts = 123;
        event.seq = 77;
        store.apply(event);

        // best_bid_ask no longer updates cached_bbo or emits notifications
        // It only compares server BBO with book BBO for divergence detection
        auto notif = queue.try_pop();
        CHECK_FALSE(notif.has_value());

        // BBO_DIVERGENCE metric should be incremented (book BBO is 0/0, server says 5200/5300)
        CHECK(metrics.get(MetricId::BBO_DIVERGENCE) > 0);
    }
}
