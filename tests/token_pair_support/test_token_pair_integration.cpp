#include <doctest/doctest.h>

#include "common/clock.h"
#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "events/event_variant.h"
#include "events/user_events.h"
#include "exec/exec_queue_sink.h"
#include "exec/inventory_safety.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/quote_planner.h"
#include "state/exec_state_store.h"
#include "state/market_state_store.h"

using namespace lt;

namespace {

UserTradeUpdate make_trade(const char* tid, const char* oid, const char* aid,
                           Side side, Qty_t size) {
    UserTradeUpdate t;
    t.trade_id = TradeId(tid);
    t.taker_order_id = OrderId(oid);
    t.asset_id = AssetId(aid);
    t.market_id = AssetId("cond-1");
    t.status = TradeStatus::MATCHED;
    t.side = side;
    t.fill_price = 5000;
    t.fill_size = size;
    t.maker_entries[0].order_id = OrderId(oid);
    t.maker_entries[0].matched_amount = size;
    t.maker_entry_count = 1;
    return t;
}

UserOrderUpdate make_order(const char* oid, const char* aid, Side side) {
    UserOrderUpdate upd;
    upd.order_id = OrderId(oid);
    upd.asset_id = AssetId(aid);
    upd.market_id = AssetId("cond-1");
    upd.event_type = OrderEventType::PLACEMENT;
    upd.side = side;
    upd.price = 5000;
    upd.original_size = qty_from_int(1000);
    upd.size_matched = 0;
    return upd;
}

}  // namespace

TEST_SUITE("TokenPair.Integration") {

TEST_CASE("token-specific order books remain independent") {
    SpscQueue<MarketNotification> queue(1024);
    Metrics metrics;
    MarketStateStore store(queue, metrics);

    BookSnapshot up_snap;
    up_snap.asset_id = AssetId("up");
    up_snap.bids[0] = {6200, qty_from_int(100)};
    up_snap.bid_count = 1;
    up_snap.asks[0] = {6300, qty_from_int(80)};
    up_snap.ask_count = 1;

    BookSnapshot down_snap;
    down_snap.asset_id = AssetId("down");
    down_snap.bids[0] = {3700, qty_from_int(90)};
    down_snap.bid_count = 1;
    down_snap.asks[0] = {3800, qty_from_int(70)};
    down_snap.ask_count = 1;

    MarketEvent ev1;
    ev1.payload = up_snap;
    ev1.recv_ts = SteadyClock::now();
    ev1.seq = 1;
    store.apply(ev1);

    MarketEvent ev2;
    ev2.payload = down_snap;
    ev2.recv_ts = SteadyClock::now();
    ev2.seq = 2;
    store.apply(ev2);

    auto* up = store.get_state(AssetId("up"));
    auto* down = store.get_state(AssetId("down"));
    REQUIRE(up != nullptr);
    REQUIRE(down != nullptr);

    CHECK(up->book.bbo().best_bid == 6200);
    CHECK(up->book.bbo().best_ask == 6300);
    CHECK(down->book.bbo().best_bid == 3700);
    CHECK(down->book.bbo().best_ask == 3800);

    PriceChangeEvent delta;
    delta.asset_count = 2;
    delta.asset_changes[0].asset_id = AssetId("up");
    delta.asset_changes[0].changes[0] = {6200, Side::BID, qty_from_int(150)};
    delta.asset_changes[0].change_count = 1;
    delta.asset_changes[1].asset_id = AssetId("down");
    delta.asset_changes[1].changes[0] = {3800, Side::ASK, qty_from_int(10)};
    delta.asset_changes[1].change_count = 1;

    MarketEvent ev3;
    ev3.payload = delta;
    ev3.recv_ts = SteadyClock::now();
    ev3.seq = 3;
    store.apply(ev3);

    CHECK(up->book.bid_qty_at(6200) == qty_from_int(150));
    CHECK(down->book.ask_qty_at(3800) == qty_from_int(10));
}

TEST_CASE("fills update token positions and shared inventory") {
    SpscQueue<SchedulerEvent> user_queue(1024);
    Metrics metrics;
    TokenInventory inventory;
    inventory.register_token(AssetId("up"));
    inventory.register_token(AssetId("down"));
    ExecStateStore store(user_queue, metrics, nullptr, nullptr, &inventory);

    // Register orders so maker attribution works
    store.apply_order_update(make_order("o1", "up", Side::ASK), 500, 0);
    user_queue.try_pop();
    store.apply_order_update(make_order("o2", "down", Side::BID), 501, 0);
    user_queue.try_pop();

    auto t1 = make_trade("t1", "o1", "up", Side::ASK, qty_from_int(40));
    auto t2 = make_trade("t2", "o2", "down", Side::BID, qty_from_int(15));

    CHECK(store.apply_trade_update(t1, 1000, 1) == ApplyResult::APPLIED);
    CHECK(store.apply_trade_update(t2, 1001, 2) == ApplyResult::APPLIED);

    auto* up_pos = store.get_position(AssetId("up"));
    auto* down_pos = store.get_position(AssetId("down"));
    REQUIRE(up_pos != nullptr);
    REQUIRE(down_pos != nullptr);

    // t1: Side::ASK on "up" → we SOLD up (-40)
    // t2: Side::BID on "down" → we BOUGHT down (+15)
    CHECK(up_pos->net_position == -qty_from_int(40));
    CHECK(down_pos->net_position == qty_from_int(15));
    CHECK(inventory.position_for(AssetId("up")) == -qty_from_int(40));
    CHECK(inventory.position_for(AssetId("down")) == qty_from_int(15));
}

TEST_CASE("planner pass-through preserves SELL and inventory safety blocks unsafe SELL") {
    MarketPairRegistry registry;
    CHECK(registry.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

    TokenInventory inventory;
    inventory.set_position(AssetId("up"), qty_from_int(5));

    QuotePlanner planner(&registry, &inventory);

    ExecutionIntent desired_sell;
    desired_sell.action = IntentAction::WOULD_PLACE_ASK;
    desired_sell.asset_id = AssetId("up");
    desired_sell.market_id = AssetId("cond-1");
    desired_sell.price = 6400;
    desired_sell.qty = qty_from_int(25);
    desired_sell.intent_id = 99;

    auto planned = planner.plan(desired_sell);
    REQUIRE(planned.count == 1);
    CHECK(planned.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(planned.intents[0].asset_id == AssetId("up"));
    CHECK(planned.intents[0].price == 6400);

    SpscQueue<ExecIntent> exec_q(32);
    ExecQueueSink sink(exec_q);
    CHECK(sink.accept(planned.intents[0]) == SinkResult::ACCEPTED);

    auto* exec = exec_q.front();
    REQUIRE(exec != nullptr);
    CHECK(exec->side == Side::ASK);
    auto safety = check_inventory_for_intent(*exec, &inventory);
    CHECK_FALSE(safety.allowed);
    CHECK(safety.available == qty_from_int(5));
}

}  // TEST_SUITE
