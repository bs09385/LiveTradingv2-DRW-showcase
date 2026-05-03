#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "events/scheduler_events.h"
#include "events/user_events.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "state/exec_state_store.h"

using namespace lt;

namespace {

UserOrderUpdate make_order(const char* oid, const char* aid, OrderEventType type,
                           Side side, Price_t price, Qty_t orig, Qty_t matched) {
    UserOrderUpdate upd;
    upd.order_id = OrderId(oid);
    upd.asset_id = AssetId(aid);
    upd.market_id = AssetId("market1");
    upd.event_type = type;
    upd.side = side;
    upd.price = price;
    upd.original_size = orig;
    upd.size_matched = matched;
    upd.exchange_ts = 1700000000;
    return upd;
}

UserTradeUpdate make_trade(const char* tid, const char* taker_oid, const char* aid,
                           TradeStatus status, Side side, Price_t price, Qty_t size,
                           const char* maker_oid = nullptr) {
    UserTradeUpdate upd;
    upd.trade_id = TradeId(tid);
    upd.taker_order_id = OrderId(taker_oid);
    upd.asset_id = AssetId(aid);
    upd.market_id = AssetId("market1");
    upd.status = status;
    upd.side = side;
    upd.fill_price = price;
    upd.fill_size = size;
    upd.match_ts = 1700000010;
    upd.last_update_ts = 1700000010;
    // Default maker entry: use taker_oid so existing orders_ lookups match
    const char* moid = maker_oid ? maker_oid : taker_oid;
    upd.maker_entries[0].order_id = OrderId(moid);
    upd.maker_entries[0].matched_amount = size;
    upd.maker_entry_count = 1;
    return upd;
}

// Helper: register an order so maker_orders attribution works
void register_order(ExecStateStore& store, SpscQueue<SchedulerEvent>& queue,
                    const char* oid, const char* aid, Side side) {
    auto o = make_order(oid, aid, OrderEventType::PLACEMENT, side, 5000, qty_from_int(1000), 0);
    store.apply_order_update(o, 1000, 0);
    queue.try_pop();  // drain queue
}

}  // namespace

TEST_SUITE("ExecStateStore") {
    TEST_CASE("order PLACEMENT creates tracked order as LIVE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto upd = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(upd, 100000, 1);

        CHECK(result == ApplyResult::APPLIED);
        CHECK(store.order_count() == 1);

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::LIVE);
        CHECK(tracked->original_size == qty_from_int(100));
        CHECK(tracked->size_matched == 0);

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->source == EventSource::USER_WS);
        CHECK(ev->kind == SchedulerEventKind::USER_ORDER_UPDATE);
    }

    TEST_CASE("order UPDATE partial fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // First: PLACEMENT
        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        // Then: UPDATE with partial fill
        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        auto result = store.apply_order_update(u, 100001, 2);

        CHECK(result == ApplyResult::APPLIED);
        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::PARTIAL);
        CHECK(tracked->size_matched == qty_from_int(30));
    }

    TEST_CASE("order UPDATE fully filled") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(100));
        store.apply_order_update(u, 100001, 2);

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::FILLED);
    }

    TEST_CASE("order CANCELLATION") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto c = make_order("o1", "a1", OrderEventType::CANCELLATION, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(c, 100001, 2);

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::CANCELED);
    }

    TEST_CASE("order events propagate derived order_status_raw to scheduler") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        CHECK(store.apply_order_update(p, 100000, 1) == ApplyResult::APPLIED);
        auto ev1 = queue.try_pop();
        REQUIRE(ev1.has_value());
        CHECK(ev1->order_status_raw == static_cast<uint8_t>(OrderStatus::LIVE));

        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        CHECK(store.apply_order_update(u, 100001, 2) == ApplyResult::APPLIED);
        auto ev2 = queue.try_pop();
        REQUIRE(ev2.has_value());
        CHECK(ev2->order_status_raw == static_cast<uint8_t>(OrderStatus::PARTIAL));

        auto c = make_order("o1", "a1", OrderEventType::CANCELLATION, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        CHECK(store.apply_order_update(c, 100002, 3) == ApplyResult::APPLIED);
        auto ev3 = queue.try_pop();
        REQUIRE(ev3.has_value());
        CHECK(ev3->order_status_raw == static_cast<uint8_t>(OrderStatus::CANCELED));
    }

    TEST_CASE("partial then cancellation preserves cumulative fill for UI lifecycle classification") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        CHECK(store.apply_order_update(p, 100000, 1) == ApplyResult::APPLIED);
        queue.try_pop();

        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(25));
        CHECK(store.apply_order_update(u, 100001, 2) == ApplyResult::APPLIED);
        queue.try_pop();

        auto c = make_order("o1", "a1", OrderEventType::CANCELLATION, Side::BID, 5500, qty_from_int(100), qty_from_int(25));
        CHECK(store.apply_order_update(c, 100002, 3) == ApplyResult::APPLIED);

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::CANCELED);
        CHECK(tracked->size_matched == qty_from_int(25));

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->order_status_raw == static_cast<uint8_t>(OrderStatus::CANCELED));
        CHECK(ev->user_cumulative_filled == qty_from_int(25));
    }

    TEST_CASE("order duplicate UPDATE same size_matched is skipped") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto u1 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        store.apply_order_update(u1, 100001, 2);
        queue.try_pop();

        // Duplicate: same size_matched
        auto u2 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        auto result = store.apply_order_update(u2, 100002, 3);
        CHECK(result == ApplyResult::DUPLICATE);
        CHECK(metrics.get(MetricId::USER_WS_DUPLICATES) == 1);

        // No event pushed for duplicate
        CHECK(!queue.try_pop().has_value());
    }

    TEST_CASE("trade MATCHED creates fill and position") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        // Trade side=BID means WE bought → position increases
        auto t = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t, 200000, 1);

        CHECK(result == ApplyResult::APPLIED);
        CHECK(store.trade_count() == 1);

        auto* tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == TradeStatus::MATCHED);
        CHECK(tracked->fill_size == qty_from_int(50));

        // Position updated: we BOUGHT → position increases
        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == qty_from_int(50));
        CHECK(pos->total_bought == qty_from_int(50));
        CHECK(pos->fill_count == 1);

        CHECK(metrics.get(MetricId::USER_WS_FILLS) == 1);
    }

    TEST_CASE("trade MINED -> CONFIRMED does not recount fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MINED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);

        auto t3 = make_trade("t1", "o1", "a1", TradeStatus::CONFIRMED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t3, 200002, 3);

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(50));  // Only counted once (we sold)
        CHECK(pos->fill_count == 1);

        CHECK(metrics.get(MetricId::USER_WS_FILLS) == 1);
    }

    TEST_CASE("trade MATCHED -> RETRYING -> FAILED") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::RETRYING, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);

        auto* tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == TradeStatus::RETRYING);

        auto t3 = make_trade("t1", "o1", "a1", TradeStatus::FAILED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t3, 200002, 3);

        tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == TradeStatus::FAILED);
    }

    TEST_CASE("duplicate MATCHED trade does not double-count fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        // Re-send same trade (different recv_ts but same trade_id)
        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(50));  // Not doubled (we sold)
        CHECK(pos->fill_count == 1);
    }

    TEST_CASE("BUY side increments, SELL side decrements position") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);
        register_order(store, queue, "o2", "a1", Side::BID);

        // Side::ASK → we SOLD 100
        auto sell = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(100));
        store.apply_trade_update(sell, 200000, 1);

        // Side::BID → we BOUGHT 40
        auto buy = make_trade("t2", "o2", "a1", TradeStatus::MATCHED, Side::BID, 5600, qty_from_int(40));
        store.apply_trade_update(buy, 200001, 2);

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(60));  // -100 + 40
        CHECK(pos->total_sold == qty_from_int(100));
        CHECK(pos->total_bought == qty_from_int(40));
        CHECK(pos->fill_count == 2);
    }

    TEST_CASE("position accumulation across multiple fills") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);

        // All Side::ASK → we SOLD 10 shares each time
        for (int i = 0; i < 5; ++i) {
            std::string tid = "t" + std::to_string(i);
            auto t = make_trade(tid.c_str(), "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(10));
            store.apply_trade_update(t, 200000 + i, static_cast<SeqNum_t>(i + 1));
        }

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(50));  // Sold 50 total
        CHECK(pos->total_sold == qty_from_int(50));
        CHECK(pos->fill_count == 5);
    }

    TEST_CASE("trade update before order exists: unattributed, zero fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Trade arrives before order — maker entry won't match orders_ map
        auto t = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t, 200000, 1);
        CHECK(result == ApplyResult::APPLIED);

        // Trade is tracked for lifecycle, but no fill recorded (unattributed)
        CHECK(store.trade_count() == 1);
        CHECK(store.order_count() == 0);

        auto* pos = store.get_position(AssetId("a1"));
        CHECK(pos == nullptr);  // no fill → no position

        CHECK(metrics.get(MetricId::USER_WS_TRADE_UNATTRIBUTED) == 1);
    }

    TEST_CASE("queue emission has correct fields") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto upd = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::ASK, 5500, qty_from_int(100), 0);
        store.apply_order_update(upd, 100000, 42);

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->source == EventSource::USER_WS);
        CHECK(ev->kind == SchedulerEventKind::USER_ORDER_UPDATE);
        CHECK(ev->asset_id == AssetId("a1"));
        CHECK(ev->recv_ts == 100000);
        CHECK(ev->seq == 42);
        CHECK(ev->order_id == OrderId("o1"));
        CHECK(ev->user_side == Side::ASK);
        CHECK(ev->user_market_id == AssetId("market1"));
        CHECK(ev->user_price == 5500);
        CHECK(ev->user_original_size == qty_from_int(100));
    }

    TEST_CASE("trade emission has correct fields") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t, 200000, 99);

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->source == EventSource::USER_WS);
        CHECK(ev->kind == SchedulerEventKind::USER_TRADE_UPDATE);
        CHECK(ev->asset_id == AssetId("a1"));
        CHECK(ev->recv_ts == 200000);
        CHECK(ev->seq == 99);
        CHECK(ev->trade_id == TradeId("t1"));
        CHECK(ev->user_market_id == AssetId("market1"));
        CHECK(ev->is_new_fill == true);
        CHECK(ev->user_fill_size == qty_from_int(50));
        CHECK(ev->user_price == 5500);
    }

    TEST_CASE("trade emission is_new_fill false for status-only updates") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::CONFIRMED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->is_new_fill == false);
    }

    TEST_CASE("queue overflow increments metric and returns QUEUE_OVERFLOW") {
        SpscQueue<SchedulerEvent> queue(2);  // very small
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        ApplyResult last_result = ApplyResult::APPLIED;
        for (int i = 0; i < 10; ++i) {
            std::string oid = "o" + std::to_string(i);
            auto upd = make_order(oid.c_str(), "a1", OrderEventType::PLACEMENT,
                                  Side::BID, 5500, qty_from_int(100), 0);
            last_result = store.apply_order_update(upd, 100000 + i,
                                                   static_cast<SeqNum_t>(i + 1));
        }

        CHECK(metrics.get(MetricId::USER_WS_QUEUE_OVERFLOW) > 0);
    }

    TEST_CASE("order PLACEMENT with nonzero size_matched tracks status but not position") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Edge case: PLACEMENT with pre-matched size
        auto upd = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), qty_from_int(50));
        store.apply_order_update(upd, 100000, 1);

        // Order status is tracked
        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->size_matched == qty_from_int(50));

        // Fills are NOT recorded from order updates (trade MATCHED is sole source)
        auto* pos = store.get_position(AssetId("a1"));
        CHECK(pos == nullptr);
    }

    TEST_CASE("order size_matched increase tracks status but not position") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);

        auto u1 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        store.apply_order_update(u1, 100001, 2);

        auto u2 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(70));
        store.apply_order_update(u2, 100002, 3);

        // Order status tracked correctly
        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->size_matched == qty_from_int(70));
        CHECK(tracked->status == OrderStatus::PARTIAL);

        // Fills are NOT recorded from order updates (trade MATCHED is sole source)
        auto* pos = store.get_position(AssetId("a1"));
        CHECK(pos == nullptr);
    }

    TEST_CASE("multi-asset positions are independent") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);
        register_order(store, queue, "o2", "a2", Side::BID);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(100));
        store.apply_trade_update(t1, 200000, 1);

        auto t2 = make_trade("t2", "o2", "a2", TradeStatus::MATCHED, Side::BID, 4500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);

        CHECK(store.position_count() == 2);

        auto* p1 = store.get_position(AssetId("a1"));
        REQUIRE(p1 != nullptr);
        CHECK(p1->net_position == -qty_from_int(100));  // Side::ASK → we sold

        auto* p2 = store.get_position(AssetId("a2"));
        REQUIRE(p2 != nullptr);
        CHECK(p2->net_position == qty_from_int(50));  // Side::BID → we bought
    }

    // === Audit regression tests (M3 security/correctness audit) ===

    TEST_CASE("[audit] stale size_matched regression rejected as DUPLICATE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        // UPDATE with matched=50
        auto u1 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(50));
        store.apply_order_update(u1, 100001, 2);
        queue.try_pop();

        // Out-of-order UPDATE with lower matched=30 (stale) must be rejected
        auto u2 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(30));
        auto result = store.apply_order_update(u2, 100002, 3);
        CHECK(result == ApplyResult::DUPLICATE);

        // Order state should NOT have regressed
        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->size_matched == qty_from_int(50));  // not regressed to 30
    }

    TEST_CASE("[audit] PLACEMENT after FILLED is rejected as DUPLICATE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // PLACEMENT -> UPDATE (fully filled)
        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(100));
        store.apply_order_update(u, 100001, 2);
        queue.try_pop();

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::FILLED);

        // Late PLACEMENT arrives (terminal state cannot regress to LIVE)
        auto late_p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(late_p, 100002, 3);
        CHECK(result == ApplyResult::DUPLICATE);
        CHECK(tracked->status == OrderStatus::FILLED);
    }

    TEST_CASE("[audit] UPDATE on CANCELED order with same size_matched is DUPLICATE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto c = make_order("o1", "a1", OrderEventType::CANCELLATION, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(c, 100001, 2);
        queue.try_pop();

        // UPDATE on CANCELED order with no new fill
        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(u, 100002, 3);
        CHECK(result == ApplyResult::DUPLICATE);
    }

    TEST_CASE("[audit] CANCELLATION after FILLED is accepted (exchange race)") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(p, 100000, 1);
        queue.try_pop();

        auto u = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(100));
        store.apply_order_update(u, 100001, 2);
        queue.try_pop();

        // CANCELLATION can still arrive after FILLED (exchange race), should be accepted
        auto c = make_order("o1", "a1", OrderEventType::CANCELLATION, Side::BID, 5500, qty_from_int(100), qty_from_int(100));
        auto result = store.apply_order_update(c, 100002, 3);
        CHECK(result == ApplyResult::APPLIED);

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::CANCELED);
    }

    TEST_CASE("[audit] out-of-order MATCHED trade after MINED still counts fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);

        // MINED arrives first (no fill counted yet)
        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MINED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto* pos = store.get_position(AssetId("a1"));
        CHECK(pos == nullptr);  // No fill yet (MINED doesn't count)

        // MATCHED arrives late (out-of-order) — fill must be counted
        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t2, 200001, 2);
        CHECK(result == ApplyResult::APPLIED);

        pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(50));  // Side::ASK → we sold
        CHECK(pos->fill_count == 1);
        CHECK(metrics.get(MetricId::USER_WS_FILLS) == 1);

        // Status should NOT regress (stays MINED, not MATCHED)
        auto* tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == TradeStatus::MINED);
    }

    TEST_CASE("[audit] out-of-order CONFIRMED then MATCHED counts fill once") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::ASK);

        // CONFIRMED arrives first
        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::CONFIRMED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        // MATCHED arrives late
        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);
        queue.try_pop();

        // Second MATCHED (replay) should NOT count again
        auto t3 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::ASK, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t3, 200002, 3);
        CHECK(result == ApplyResult::DUPLICATE);

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(50));  // Side::ASK → we sold, counted once
        CHECK(pos->fill_count == 1);
    }

    TEST_CASE("[audit] repeated trade status update is DUPLICATE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MINED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);
        queue.try_pop();

        // Repeated MINED should be DUPLICATE
        auto t3 = make_trade("t1", "o1", "a1", TradeStatus::MINED, Side::BID, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t3, 200002, 3);
        CHECK(result == ApplyResult::DUPLICATE);
        CHECK(metrics.get(MetricId::USER_WS_DUPLICATES) == 1);
    }

    TEST_CASE("[audit] trade status regression (non-MATCHED) is DUPLICATE") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t1 = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t1, 200000, 1);
        queue.try_pop();

        auto t2 = make_trade("t1", "o1", "a1", TradeStatus::MINED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t2, 200001, 2);
        queue.try_pop();

        auto t3 = make_trade("t1", "o1", "a1", TradeStatus::CONFIRMED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t3, 200002, 3);
        queue.try_pop();

        // RETRYING arriving after CONFIRMED is a regression, not MATCHED, must be DUPLICATE
        auto t4 = make_trade("t1", "o1", "a1", TradeStatus::RETRYING, Side::BID, 5500, qty_from_int(50));
        auto result = store.apply_trade_update(t4, 200003, 4);
        CHECK(result == ApplyResult::DUPLICATE);
    }

    TEST_CASE("[audit] order partial status does not regress from FILLED") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Start with UPDATE that is fully filled (first-seen as FILLED)
        auto u1 = make_order("o1", "a1", OrderEventType::UPDATE, Side::BID, 5500, qty_from_int(100), qty_from_int(100));
        store.apply_order_update(u1, 100000, 1);
        queue.try_pop();

        auto* tracked = store.get_order(OrderId("o1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->status == OrderStatus::FILLED);

        // Late PLACEMENT cannot regress FILLED to LIVE
        auto p = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(p, 100001, 2);
        CHECK(result == ApplyResult::DUPLICATE);
        CHECK(tracked->status == OrderStatus::FILLED);
    }

    TEST_CASE("[audit] queue overflow returns QUEUE_OVERFLOW not APPLIED") {
        SpscQueue<SchedulerEvent> queue(2);  // very small
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Fill the queue
        auto o1 = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(o1, 100000, 1);
        auto o2 = make_order("o2", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(o2, 100001, 2);

        // Queue should now be full — next push must return QUEUE_OVERFLOW
        auto o3 = make_order("o3", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(o3, 100002, 3);
        CHECK(result == ApplyResult::QUEUE_OVERFLOW);
        CHECK(metrics.get(MetricId::USER_WS_QUEUE_OVERFLOW) >= 1);
    }

    TEST_CASE("metrics counters incremented correctly") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        auto o = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(o, 100000, 1);

        auto t = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 5500, qty_from_int(50));
        store.apply_trade_update(t, 200000, 2);

        CHECK(metrics.get(MetricId::USER_WS_ORDER_UPDATES) == 1);
        CHECK(metrics.get(MetricId::USER_WS_TRADE_UPDATES) == 1);
        CHECK(metrics.get(MetricId::USER_WS_FILLS) == 1);
        CHECK(metrics.get(MetricId::USER_WS_POSITION_DELTAS) == 1);
    }

    // === Audit round-2 regression tests ===

    TEST_CASE("[audit2] push_spin retries on full queue (unit test of SpscQueue)") {
        // Verify push_spin returns true when space becomes available
        SpscQueue<SchedulerEvent> queue(4);
        SchedulerEvent ev;
        ev.source = EventSource::USER_WS;

        // Fill queue
        CHECK(queue.try_push(ev) == true);
        CHECK(queue.try_push(ev) == true);
        CHECK(queue.try_push(ev) == true);
        CHECK(queue.try_push(ev) == true);

        // try_push fails (full)
        CHECK(queue.try_push(ev) == false);

        // Drain one slot
        queue.try_pop();

        // push_spin should succeed immediately (slot available)
        CHECK(queue.push_spin(ev, 10) == true);
    }

    TEST_CASE("[audit2] push_spin failure sets fatal flag") {
        SpscQueue<SchedulerEvent> queue(2);  // very small
        Metrics metrics;
        std::atomic<bool> fatal{false};
        ExecStateStore store(queue, metrics, nullptr, &fatal);

        // Fill queue and don't drain — spin will exhaust
        auto o1 = make_order("o1", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(o1, 100000, 1);
        auto o2 = make_order("o2", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        store.apply_order_update(o2, 100001, 2);

        // No consumer — spin will exhaust, fatal flag should be set
        auto o3 = make_order("o3", "a1", OrderEventType::PLACEMENT, Side::BID, 5500, qty_from_int(100), 0);
        auto result = store.apply_order_update(o3, 100002, 3);

        CHECK(result == ApplyResult::QUEUE_OVERFLOW);
        CHECK(fatal.load() == true);
        CHECK(metrics.get(MetricId::USER_WS_QUEUE_OVERFLOW) >= 1);
    }

    TEST_CASE("[audit2] client_order_id stored in TrackedOrder") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        UserOrderUpdate upd;
        upd.order_id = OrderId("exchange_hash_123");
        upd.client_order_id = OrderId("my_client_id_456");
        upd.asset_id = AssetId("a1");
        upd.market_id = AssetId("market1");
        upd.event_type = OrderEventType::PLACEMENT;
        upd.side = Side::BID;
        upd.price = 5500;
        upd.original_size = qty_from_int(100);
        upd.size_matched = 0;

        store.apply_order_update(upd, 100000, 1);

        auto* tracked = store.get_order(OrderId("exchange_hash_123"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->client_order_id == OrderId("my_client_id_456"));
    }

    TEST_CASE("[audit2] client_order_id propagated to SchedulerEvent") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        UserOrderUpdate upd;
        upd.order_id = OrderId("exchange_hash_789");
        upd.client_order_id = OrderId("client_xyz");
        upd.asset_id = AssetId("a1");
        upd.market_id = AssetId("market1");
        upd.event_type = OrderEventType::PLACEMENT;
        upd.side = Side::BID;
        upd.price = 5500;
        upd.original_size = qty_from_int(100);
        upd.size_matched = 0;

        store.apply_order_update(upd, 100000, 1);

        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->client_order_id == OrderId("client_xyz"));
    }

    // === Cross-book fill and fill attribution tests ===

    TEST_CASE("cross-book fill: order on UP, matched on DOWN book, uses order perspective") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Register our SELL order on "up" token
        auto o = make_order("o1", "up", OrderEventType::PLACEMENT, Side::ASK, 6300, qty_from_int(100), 0);
        store.apply_order_update(o, 1000, 0);
        queue.try_pop();

        // Trade arrives as BUY on "down" token at complement price 3700
        // Our order "o1" is found in maker_orders → asset differs → conversion
        auto t = make_trade("t1", "taker1", "down", TradeStatus::MATCHED, Side::BID, 3700, qty_from_int(50), "o1");
        store.apply_trade_update(t, 200000, 1);

        // Position should be on "up" (our order's asset), not "down"
        auto* pos_up = store.get_position(AssetId("up"));
        REQUIRE(pos_up != nullptr);
        CHECK(pos_up->net_position == -qty_from_int(50));  // SELL: position decreased
        CHECK(pos_up->total_sold == qty_from_int(50));

        auto* pos_down = store.get_position(AssetId("down"));
        CHECK(pos_down == nullptr);  // no position on DOWN

        // TrackedTrade stored with converted values
        auto* tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->asset_id == AssetId("up"));
        CHECK(tracked->side == Side::ASK);
        CHECK(tracked->fill_price == 6300);  // complement of 3700

        // SchedulerEvent also uses converted values
        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->asset_id == AssetId("up"));
        CHECK(ev->user_side == Side::ASK);
        CHECK(ev->user_price == 6300);
        CHECK(ev->is_new_fill == true);

        CHECK(metrics.get(MetricId::USER_WS_CROSS_BOOK_FILL) == 1);
    }

    TEST_CASE("cross-book fill: BUY order on UP, matched on DOWN book") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Register BUY order on "up" token
        auto o = make_order("o1", "up", OrderEventType::PLACEMENT, Side::BID, 7000, qty_from_int(100), 0);
        store.apply_order_update(o, 1000, 0);
        queue.try_pop();

        // Trade arrives as SELL on "down" at complement price 3000
        auto t = make_trade("t1", "taker1", "down", TradeStatus::MATCHED, Side::ASK, 3000, qty_from_int(20), "o1");
        store.apply_trade_update(t, 200000, 1);

        // Position on UP (BUY), not DOWN
        auto* pos = store.get_position(AssetId("up"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == qty_from_int(20));  // BUY: position increased
        CHECK(pos->total_bought == qty_from_int(20));

        CHECK(store.get_position(AssetId("down")) == nullptr);
        CHECK(metrics.get(MetricId::USER_WS_CROSS_BOOK_FILL) == 1);
    }

    TEST_CASE("same-book fill: order and trade on same token, no conversion needed") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        register_order(store, queue, "o1", "a1", Side::BID);

        auto t = make_trade("t1", "o1", "a1", TradeStatus::MATCHED, Side::BID, 6300, qty_from_int(50));
        store.apply_trade_update(t, 200000, 1);

        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == qty_from_int(50));

        CHECK(metrics.get(MetricId::USER_WS_CROSS_BOOK_FILL) == 0);
    }

    TEST_CASE("unattributed trade records zero fill") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // No order registered — maker entry "unknown_oid" won't match
        auto t = make_trade("t1", "unknown_taker", "a1", TradeStatus::MATCHED,
                            Side::BID, 5500, qty_from_int(500), "unknown_oid");
        store.apply_trade_update(t, 200000, 1);

        // Trade tracked for lifecycle
        CHECK(store.trade_count() == 1);
        auto* tracked = store.get_trade(TradeId("t1"));
        REQUIRE(tracked != nullptr);
        CHECK(tracked->fill_size == 0);  // zero fill

        // No position created
        CHECK(store.get_position(AssetId("a1")) == nullptr);

        // Event emitted but with no fill
        auto ev = queue.try_pop();
        REQUIRE(ev.has_value());
        CHECK(ev->is_new_fill == false);

        CHECK(metrics.get(MetricId::USER_WS_TRADE_UNATTRIBUTED) == 1);
    }

    TEST_CASE("taker fallback when maker lookup fails") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Register order "taker1" (will match taker_order_id)
        register_order(store, queue, "taker1", "a1", Side::BID);

        // Trade with non-matching maker entries, but taker_order_id matches
        auto t = make_trade("t1", "taker1", "a1", TradeStatus::MATCHED,
                            Side::BID, 5500, qty_from_int(100), "unknown_maker");
        store.apply_trade_update(t, 200000, 1);

        // Fill should use taker's total (fill_size)
        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == qty_from_int(100));

        CHECK(metrics.get(MetricId::USER_WS_TAKER_FALLBACK) == 1);
    }

    TEST_CASE("maker match takes priority over taker match") {
        SpscQueue<SchedulerEvent> queue(1024);
        Metrics metrics;
        ExecStateStore store(queue, metrics);

        // Register both maker and taker orders
        register_order(store, queue, "maker1", "a1", Side::ASK);
        register_order(store, queue, "taker1", "a1", Side::BID);

        // Trade where maker and taker both match. maker_entries matched_amount=20
        UserTradeUpdate t;
        t.trade_id = TradeId("t1");
        t.taker_order_id = OrderId("taker1");
        t.asset_id = AssetId("a1");
        t.market_id = AssetId("market1");
        t.status = TradeStatus::MATCHED;
        t.side = Side::ASK;
        t.fill_price = 5500;
        t.fill_size = qty_from_int(200);  // taker total
        t.maker_entries[0].order_id = OrderId("maker1");
        t.maker_entries[0].matched_amount = qty_from_int(20);  // our maker fill
        t.maker_entry_count = 1;

        store.apply_trade_update(t, 200000, 1);

        // Maker match should win: 20, not taker's 200
        auto* pos = store.get_position(AssetId("a1"));
        REQUIRE(pos != nullptr);
        CHECK(pos->net_position == -qty_from_int(20));  // ASK = sell

        CHECK(metrics.get(MetricId::USER_WS_TAKER_FALLBACK) == 0);
    }
}
