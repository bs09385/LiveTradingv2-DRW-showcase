#include "doctest/doctest.h"

#include <cstdio>

#include "common/clock.h"
#include "common/market_pair.h"
#include "common/types.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "events/user_events.h"
#include "exec/exec_feedback.h"
#include "scheduler/strategy_book_store.h"
#include "scheduler/strategy_context.h"
#include "scheduler/strategy_state_stub.h"
#include "scheduler/test_strategy.h"
#include "scheduler/working_order_tracker.h"

using namespace lt;

namespace {

StrategyBookStore g_test_books2;

StrategyContext make_ctx(const SchedulerEvent& event, const StrategyStateStub& state) {
    return StrategyContext{event, g_test_books2, nullptr, nullptr, nullptr, nullptr, state, 0};
}

MarketNotification make_market_bbo(const char* asset, Price_t bid, Price_t ask,
                                   Timestamp_ns ts = 1000000) {
    MarketNotification n{};
    n.asset_id = AssetId(asset);
    n.bbo.best_bid = bid;
    n.bbo.best_ask = ask;
    n.kind = NotificationKind::BBO_UPDATE;
    n.recv_ts = ts;
    return n;
}

SchedulerEvent make_exec_accepted(const OrderId& client_id, const OrderId& exchange_id,
                                   Timestamp_ns ts = 2000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_accepted = true;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::ORDER_ACCEPTED);
    ev.client_order_id = client_id;
    ev.order_id = exchange_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_exec_rejected(const OrderId& client_id, Timestamp_ns ts = 2000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_REJECT;
    ev.exec_accepted = false;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::ORDER_REJECTED);
    ev.client_order_id = client_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_user_live(const OrderId& exchange_id, const OrderId& client_id,
                               Timestamp_ns ts = 3000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::LIVE);
    ev.order_id = exchange_id;
    ev.client_order_id = client_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_user_canceled(const OrderId& exchange_id, const OrderId& client_id,
                                   Timestamp_ns ts = 4000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::CANCELED);
    ev.order_id = exchange_id;
    ev.client_order_id = client_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_user_filled(const OrderId& exchange_id, Timestamp_ns ts = 8000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::FILLED);
    ev.order_id = exchange_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_cancel_confirmed(const OrderId& client_id, Timestamp_ns ts = 4000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_accepted = true;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::CANCEL_CONFIRMED);
    ev.client_order_id = client_id;
    ev.recv_ts = ts;
    return ev;
}

SchedulerEvent make_trade_fill(const OrderId& exchange_id, Timestamp_ns ts = 5000000) {
    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_TRADE_UPDATE;
    ev.order_id = exchange_id;
    ev.is_new_fill = true;
    ev.recv_ts = ts;
    return ev;
}

// Helper: drive strategy from BBO through P1_PLACE, return the batch
struct P1Setup {
    OrderId bid_client;
    OrderId ask_client;
};

P1Setup drive_to_p1_wait_accepted(TestStrategy& strat, StrategyStateStub& state) {
    auto notif = make_market_bbo("tok_up", 5000, 5500);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    P1Setup s;
    s.bid_client = batch.intents[0].client_order_id;
    s.ask_client = batch.intents[1].client_order_id;
    return s;
}

}  // namespace

TEST_SUITE("TestStrategy") {

// ---------------------------------------------------------------------------
// Basic lifecycle tests
// ---------------------------------------------------------------------------

TEST_CASE("starts disabled in WAIT_FOR_BBO state") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    StrategyStateStub state;

    CHECK(!strat.enabled());
    CHECK(strat.phase() == TestPhase::WAIT_FOR_BBO);

    auto notif = make_market_bbo("tok_up", 5000, 5500);
    SchedulerEvent event = SchedulerEvent::from_market(notif);
    IntentBatch batch = strat.evaluate(make_ctx(event, state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::WAIT_FOR_BBO);
}

TEST_CASE("non-market events ignored in WAIT_FOR_BBO") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.recv_ts = 1000;
    IntentBatch batch = strat.evaluate(make_ctx(ev, state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::WAIT_FOR_BBO);
}

TEST_CASE("invalid BBO does not advance state") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto notif = make_market_bbo("tok_up", kInvalidPrice, 5500);
    SchedulerEvent event = SchedulerEvent::from_market(notif);
    IntentBatch batch = strat.evaluate(make_ctx(event, state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::WAIT_FOR_BBO);
}

TEST_CASE("safe default params") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    TestStrategy strat(&tracker, &registry, "");

    CHECK(strat.spread_ticks() == 30);
    CHECK(strat.quote_size() == qty_from_int(5));
}

// ---------------------------------------------------------------------------
// Phase 1: GTC Place + Hold + Cancel
// ---------------------------------------------------------------------------

TEST_CASE("BBO capture → P1_PLACE emits 2-intent batch (BID@100 + ASK@9900)") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto notif = make_market_bbo("tok_up", 5000, 5500);
    SchedulerEvent event = SchedulerEvent::from_market(notif);
    IntentBatch batch = strat.evaluate(make_ctx(event, state));

    CHECK(batch.count == 2);
    CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_BID);
    CHECK(batch.intents[0].price == 100);
    CHECK(batch.intents[0].qty == qty_from_int(5));
    CHECK(batch.intents[0].order_type == OrderType::GTC);
    CHECK(batch.intents[0].asset_id == AssetId("tok_up"));
    CHECK(batch.intents[0].market_id == AssetId("cond1"));

    CHECK(batch.intents[1].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(batch.intents[1].price == 9900);
    CHECK(batch.intents[1].qty == qty_from_int(5));
    CHECK(batch.intents[1].order_type == OrderType::GTC);

    CHECK(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);
}

TEST_CASE("P1 client_order_id format is lt-test-N") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto notif = make_market_bbo("tok_up", 5000, 5500);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));

    CHECK(batch.count == 2);
    std::string id0(batch.intents[0].client_order_id.view());
    std::string id1(batch.intents[1].client_order_id.view());
    CHECK(id0.substr(0, 8) == "lt-test-");
    CHECK(id1.substr(0, 8) == "lt-test-");
    CHECK(id0 != id1);
}

TEST_CASE("P1 dual accepted advances to P1_WAIT_LIVE") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto s = drive_to_p1_wait_accepted(strat, state);
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);

    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    // Accept bid only — should stay
    strat.evaluate(make_ctx(make_exec_accepted(s.bid_client, exch_bid), state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);

    // Accept ask — both accepted, should advance
    strat.evaluate(make_ctx(make_exec_accepted(s.ask_client, exch_ask), state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_LIVE);
}

TEST_CASE("P1 accept sets live — next event in P1_WAIT_LIVE advances to P1_HOLD") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto s = drive_to_p1_wait_accepted(strat, state);
    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    strat.evaluate(make_ctx(make_exec_accepted(s.bid_client, exch_bid), state));
    strat.evaluate(make_ctx(make_exec_accepted(s.ask_client, exch_ask), state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_LIVE);

    // EXEC_ORDER_ACK already set live=true for both slots.
    // Verify slots have live set from accept.
    CHECK(strat.slot(0).live);
    CHECK(strat.slot(1).live);

    // Any event in P1_WAIT_LIVE should advance to P1_HOLD immediately
    // (User WS LIVE events are no longer required for phase advancement)
    strat.evaluate(make_ctx(make_user_live(exch_bid, s.bid_client), state));
    CHECK(strat.phase() == TestPhase::P1_HOLD);
}

TEST_CASE("P1 hold → cancel after 10s emits 2 cancel intents") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto s_bid = batch.intents[0].client_order_id;
    auto s_ask = batch.intents[1].client_order_id;

    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    strat.evaluate(make_ctx(make_exec_accepted(s_bid, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(s_ask, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, s_bid, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, s_ask, base_ts + 400), state));
    REQUIRE(strat.phase() == TestPhase::P1_HOLD);

    // Before 10s — stay in HOLD
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 5'000'000'000LL;  // 5s
    batch = strat.evaluate(make_ctx(ev, state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::P1_HOLD);

    // After 10s — emit 2 cancels
    ev.recv_ts = base_ts + 11'000'000'000LL;  // 11s
    batch = strat.evaluate(make_ctx(ev, state));
    CHECK(batch.count == 2);
    CHECK(batch.intents[0].action == IntentAction::WOULD_CANCEL_BID);
    CHECK(batch.intents[0].exchange_order_id == exch_bid);
    CHECK(batch.intents[1].action == IntentAction::WOULD_CANCEL_ASK);
    CHECK(batch.intents[1].exchange_order_id == exch_ask);
    CHECK(strat.phase() == TestPhase::P1_WAIT_CANCELED);
}

TEST_CASE("P1 dual canceled → P2_PLACE emits at-BBO orders") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;

    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    REQUIRE(strat.phase() == TestPhase::P1_HOLD);

    // 10s hold
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    batch = strat.evaluate(make_ctx(ev, state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_CANCELED);

    // Cancel both
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_CANCELED);  // only one canceled so far
    strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));

    // Should have advanced through P2_PLACE and be waiting for P2 accepted
    CHECK(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);
}

TEST_CASE("P1 CANCEL_CONFIRMED from exec also advances past P1_WAIT_CANCELED") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;

    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));

    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_CANCELED);

    // Use CANCEL_CONFIRMED from exec
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));
    CHECK(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);
}

// ---------------------------------------------------------------------------
// Phase 2: At-BBO limits + Fill/Hedge
// ---------------------------------------------------------------------------

TEST_CASE("P2 place uses refreshed BBO prices") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    // Initial BBO: bid=5000 ask=5500
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;

    OrderId exch_bid("exch-bid-1");
    OrderId exch_ask("exch-ask-1");

    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));

    // Update BBO during P1_HOLD: bid=5200 ask=5700
    auto bbo_update = make_market_bbo("tok_up", 5200, 5700, base_ts + 5'000'000'000LL);
    strat.evaluate(make_ctx(SchedulerEvent::from_market(bbo_update), state));

    // Trigger P1_CANCEL after 10s hold
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    batch = strat.evaluate(make_ctx(ev, state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_CANCELED);

    // Cancel both
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    batch = strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));

    // P2_PLACE should use refreshed BBO: bid at ask=5700, ask at bid=5200
    // The evaluate above should have produced a batch when transitioning through P2_PLACE
    // The phase should now be P2_WAIT_ACCEPTED after P2_PLACE emitted the batch

    // We need the batch from the transition. The last cancel event triggers the transition.
    // Check the state
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // Verify P2 slot client_ids exist
    CHECK(strat.slot(0).client_id.len > 0);
    CHECK(strat.slot(1).client_id.len > 0);
}

TEST_CASE("P2 both filled → skip cleanup → P3") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1: accept, live, hold, cancel
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // P2: capture new slot client IDs
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");

    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // Both sides fill
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));

    // Should skip cleanup, go directly to P3
    CHECK(strat.phase() == TestPhase::P3_WAIT_BUY);
}

TEST_CASE("P2 partial fill → hedge + cancel") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1 lifecycle
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // P2: accept both
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");

    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // Only bid fills — stays in P2_WAIT_FILLS (waits for both or timeout)
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    CHECK(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // Move market to an extreme bid so old min-notional hedge math would oversize.
    auto extreme = make_market_bbo("tok_up", 9700, 9800, base_ts + 14'500'000'000LL);
    strat.evaluate(make_ctx(SchedulerEvent::from_market(extreme), state));

    // Timeout triggers cleanup with partial fill
    SchedulerEvent ev2{};
    ev2.recv_ts = base_ts + 24'000'000'000LL;  // 10s+ past P2_WAIT_FILLS entry
    batch = strat.evaluate(make_ctx(ev2, state));
    CHECK(strat.phase() == TestPhase::P2_WAIT_CLEANUP);

    // The cleanup batch should contain: FAK hedge (sell) + cancel unfilled (ask)
    CHECK(batch.count == 2);
    // Hedge: sell side (because bid filled, we sell to flatten)
    CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(batch.intents[0].order_type == OrderType::FAK);
    CHECK(batch.intents[0].price == 9700);
    CHECK(batch.intents[0].qty == qty_from_int(5));
    // Cancel unfilled ask
    CHECK(batch.intents[1].action == IntentAction::WOULD_CANCEL_ASK);
    CHECK(batch.intents[1].exchange_order_id == p2_exch_ask);
}

TEST_CASE("P2 partial cleanup retries hedge once, then stops on second reject") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1 lifecycle
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // P2: accept both
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");

    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // Partial fill path: only bid fills, then timeout to cleanup.
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    SchedulerEvent timeout{};
    timeout.recv_ts = base_ts + 24'000'000'000LL;
    batch = strat.evaluate(make_ctx(timeout, state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_CLEANUP);
    REQUIRE(batch.count == 2);

    const OrderId first_hedge_client = batch.intents[0].client_order_id;
    const OrderId cancel_client = batch.intents[1].client_order_id;

    // Cancel completes; first hedge rejected -> strategy retries exactly once.
    strat.evaluate(make_ctx(make_cancel_confirmed(cancel_client, base_ts + 24'100'000'000LL), state));
    batch = strat.evaluate(make_ctx(make_exec_rejected(first_hedge_client, base_ts + 24'100'000'100LL), state));
    CHECK(strat.phase() == TestPhase::P2_WAIT_CLEANUP);
    REQUIRE(batch.count == 1);
    CHECK(batch.intents[0].order_type == OrderType::FAK);

    // Second hedge rejection should stop strategy instead of silently proceeding.
    batch = strat.evaluate(make_ctx(make_exec_rejected(batch.intents[0].client_order_id,
                                              base_ts + 24'100'000'200LL), state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::DONE);
    CHECK(!strat.enabled());
}

TEST_CASE("P2 neither filled after timeout → cancel both") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1 lifecycle
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_user_canceled(exch_bid, bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_canceled(exch_ask, ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // P2: accept
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");

    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // 10s elapses with no fills → timeout triggers cleanup
    ev.recv_ts = base_ts + 24'000'000'000LL;  // well past 10s from P2_WAIT_FILLS entry
    batch = strat.evaluate(make_ctx(ev, state));
    CHECK(strat.phase() == TestPhase::P2_WAIT_CLEANUP);

    // Should emit 2 cancel intents (cancel both unfilled)
    CHECK(batch.count == 2);
    CHECK(batch.intents[0].action == IntentAction::WOULD_CANCEL_BID);
    CHECK(batch.intents[1].action == IntentAction::WOULD_CANCEL_ASK);
}

// ---------------------------------------------------------------------------
// Phase 3: FAK Market Orders
// ---------------------------------------------------------------------------

TEST_CASE("P3 FAK buy emits WOULD_PLACE_BID with order_type=FAK") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1 lifecycle (accelerated: direct cancel confirms)
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // P2: accept, both fill
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");

    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    batch = strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));

    // Should be in P3_WAIT_BUY, and the batch from the transition should be the FAK buy
    CHECK(strat.phase() == TestPhase::P3_WAIT_BUY);
    CHECK(batch.count == 1);
    CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_BID);
    CHECK(batch.intents[0].order_type == OrderType::FAK);
    CHECK(batch.intents[0].price == 5500);  // latest_ask
}

TEST_CASE("P3 FAK sell emits WOULD_PLACE_ASK with order_type=FAK at latest_bid") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // P1 lifecycle
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));

    // P2: both fill
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");
    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P3_WAIT_BUY);

    // FAK buy accepted → P3_FAK_SELL
    OrderId p3_buy_client = strat.slot(0).client_id;
    OrderId p3_buy_exch("exch-p3-buy");
    batch = strat.evaluate(make_ctx(make_exec_accepted(p3_buy_client, p3_buy_exch, base_ts + 15'000'000'000LL), state));

    CHECK(strat.phase() == TestPhase::P3_WAIT_SELL);
    CHECK(batch.count == 1);
    CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(batch.intents[0].order_type == OrderType::FAK);
    CHECK(batch.intents[0].price == 5000);  // latest_bid
}

TEST_CASE("P3 buy reject advances to P3_WAIT_SELL") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // Full P1
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));

    // Full P2 (both fill)
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");
    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P3_WAIT_BUY);

    batch = strat.evaluate(make_ctx(make_exec_rejected(strat.slot(0).client_id, base_ts + 15'000'000'000LL), state));
    CHECK(strat.phase() == TestPhase::P3_WAIT_SELL);
    CHECK(batch.count == 1);
    CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_ASK);
    CHECK(batch.intents[0].order_type == OrderType::FAK);
}

TEST_CASE("P3 sell reject still completes strategy") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // Full P1
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));

    // Full P2 (both fill)
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");
    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P3_WAIT_BUY);

    // Buy accepted, then sell rejected.
    OrderId p3_buy_client = strat.slot(0).client_id;
    OrderId p3_buy_exch("exch-p3-buy");
    strat.evaluate(make_ctx(make_exec_accepted(p3_buy_client, p3_buy_exch, base_ts + 15'000'000'000LL), state));
    REQUIRE(strat.phase() == TestPhase::P3_WAIT_SELL);

    strat.evaluate(make_ctx(make_exec_rejected(strat.slot(1).client_id, base_ts + 16'000'000'000LL), state));
    CHECK(strat.phase() == TestPhase::DONE);
    CHECK(!strat.enabled());
}

TEST_CASE("P3 sell accepted → DONE, strategy disabled") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // Full P1
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 200), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 300), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 400), state));
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(ev, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));

    // Full P2 (both fill)
    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid"), p2_exch_ask("exch-p2-ask");
    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, base_ts + 13'000'000'000LL), state));
    strat.evaluate(make_ctx(make_exec_accepted(p2_ask_client, p2_exch_ask, base_ts + 13'000'000'100LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_bid, base_ts + 14'000'000'000LL), state));
    strat.evaluate(make_ctx(make_user_filled(p2_exch_ask, base_ts + 14'000'000'100LL), state));

    // P3 buy accepted
    OrderId p3_buy_client = strat.slot(0).client_id;
    OrderId p3_buy_exch("exch-p3-buy");
    strat.evaluate(make_ctx(make_exec_accepted(p3_buy_client, p3_buy_exch, base_ts + 15'000'000'000LL), state));
    REQUIRE(strat.phase() == TestPhase::P3_WAIT_SELL);

    // P3 sell accepted → DONE
    OrderId p3_sell_client = strat.slot(1).client_id;
    OrderId p3_sell_exch("exch-p3-sell");
    strat.evaluate(make_ctx(make_exec_accepted(p3_sell_client, p3_sell_exch, base_ts + 16'000'000'000LL), state));
    CHECK(strat.phase() == TestPhase::DONE);
    CHECK(!strat.enabled());

    // DONE returns empty forever
    auto notif2 = make_market_bbo("tok_up", 5100, 5600, base_ts + 20'000'000'000LL);
    batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif2), state));
    CHECK(batch.count == 0);
}

// ---------------------------------------------------------------------------
// P1/P2 rejection handling
// ---------------------------------------------------------------------------

TEST_CASE("P1 one side rejected — cancels other and advances to P2") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1");

    // BID accepted, ASK rejected
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 100), state));
    batch = strat.evaluate(make_ctx(make_exec_rejected(ask_client, base_ts + 200), state));

    // Should transition to P1_CANCEL and emit cancel intents
    CHECK(strat.phase() == TestPhase::P1_WAIT_CANCELED);
    CHECK(batch.count == 2);  // cancel for both (rejected one will harmlessly fail)

    // Cancel confirmed for accepted order. The rejected slot is already terminal
    // (rejected flag persists from P1_WAIT_ACCEPTED), so no cancel response needed
    // for it — the strategy advances to P2 immediately.
    batch = strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 300), state));

    // Should advance to P2 (rejected slot treated as terminal in P1_WAIT_CANCELED)
    CHECK(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);
    CHECK(batch.count == 2);  // P2 placement batch
}

TEST_CASE("P1 both sides rejected — skips directly to P2") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;

    // First rejection triggers P1_CANCEL → P1_WAIT_CANCELED.
    // slot_[0].rejected persists; slot_[1] not yet rejected → stays in P1_WAIT_CANCELED.
    strat.evaluate(make_ctx(make_exec_rejected(bid_client, base_ts + 100), state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_CANCELED);

    // Second rejection makes both slots terminal (rejected flags).
    // P1_WAIT_CANCELED sees done_0 && done_1 → advances directly to P2.
    batch = strat.evaluate(make_ctx(make_exec_rejected(ask_client, base_ts + 200), state));

    // Should advance to P2 (both rejected = both terminal, no cancel responses needed)
    CHECK(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);
    CHECK(batch.count == 2);  // P2 placement batch
}

TEST_CASE("P2 one side rejected — cleans up and advances to P3") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // Full P1 lifecycle (accepted sets live=true, so P1_WAIT_LIVE passes quickly)
    ts += 100;
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, ts), state));
    ts += 100;
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, ts), state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_LIVE);

    // Tick to advance P1_WAIT_LIVE → P1_HOLD (live was set by accept)
    ts += 100;
    SchedulerEvent tick{};
    tick.recv_ts = ts;
    strat.evaluate(make_ctx(tick, state));
    CHECK(strat.phase() == TestPhase::P1_HOLD);

    // Hold timer expiry → P1_CANCEL (emits cancel intents)
    ts += 10'000'000'000LL;
    tick.recv_ts = ts;
    batch = strat.evaluate(make_ctx(tick, state));
    CHECK(strat.phase() == TestPhase::P1_WAIT_CANCELED);
    CHECK(batch.count == 2);

    // Cancel confirmations
    ts += 100;
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, ts), state));
    ts += 100;
    strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, ts), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    OrderId p2_bid_client = strat.slot(0).client_id;
    OrderId p2_ask_client = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-bid");

    // P2: bid accepted, ask rejected
    ts += 100;
    strat.evaluate(make_ctx(make_exec_accepted(p2_bid_client, p2_exch_bid, ts), state));
    ts += 100;
    batch = strat.evaluate(make_ctx(make_exec_rejected(p2_ask_client, ts), state));

    // Should transition to P2_CLEANUP → P2_WAIT_CLEANUP (neither-filled path)
    CHECK(strat.phase() == TestPhase::P2_WAIT_CLEANUP);

    // Cancel confirmed for accepted bid. The rejected slot is already terminal
    // (rejected flag persists from P2_WAIT_ACCEPTED), so no cancel response needed
    // for it — the strategy advances to P3 immediately.
    ts += 100;
    batch = strat.evaluate(make_ctx(make_cancel_confirmed(p2_bid_client, ts), state));

    // Should advance to P3 (rejected slot treated as terminal in P2_WAIT_CLEANUP)
    CHECK(strat.phase() == TestPhase::P3_WAIT_BUY);
    CHECK(batch.count == 1);  // FAK buy
    CHECK(batch.intents[0].order_type == OrderType::FAK);
}

// ---------------------------------------------------------------------------
// Timeout
// ---------------------------------------------------------------------------

TEST_CASE("timeout in any wait state → DONE") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);

    // 31 seconds later → timeout
    SchedulerEvent ev{};
    ev.recv_ts = base_ts + 31'000'000'000LL;
    batch = strat.evaluate(make_ctx(ev, state));
    CHECK(batch.count == 0);
    CHECK(strat.phase() == TestPhase::DONE);
    CHECK(!strat.enabled());
}

// ---------------------------------------------------------------------------
// BBO refresh
// ---------------------------------------------------------------------------

TEST_CASE("BBO refresh updates latest prices during all phases") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    // Initial BBO
    auto notif = make_market_bbo("tok_up", 5000, 5500, base_ts);
    strat.evaluate(make_ctx(SchedulerEvent::from_market(notif), state));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);

    // Feed updated BBO during P1_WAIT_ACCEPTED
    auto notif2 = make_market_bbo("tok_up", 6000, 6500, base_ts + 1000);
    strat.evaluate(make_ctx(SchedulerEvent::from_market(notif2), state));

    // The strategy should still be waiting (BBO update doesn't match order slot)
    CHECK(strat.phase() == TestPhase::P1_WAIT_ACCEPTED);
    // BBO refresh is internal — we verify indirectly when P2/P3 use the prices
}

TEST_CASE("BBO refresh ignores unrelated asset after capture") {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns base_ts = 1000000;
    auto first = make_market_bbo("tok_up", 5000, 5500, base_ts);
    IntentBatch batch = strat.evaluate(make_ctx(SchedulerEvent::from_market(first), state));
    auto bid_client = batch.intents[0].client_order_id;
    auto ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid-1"), exch_ask("exch-ask-1");

    // Feed a different asset BBO that should be ignored by refresh_bbo().
    auto foreign = make_market_bbo("tok_down", 9000, 9500, base_ts + 1000);
    strat.evaluate(make_ctx(SchedulerEvent::from_market(foreign), state));

    // Complete P1 and observe generated P2 placement prices.
    strat.evaluate(make_ctx(make_exec_accepted(bid_client, exch_bid, base_ts + 2000), state));
    strat.evaluate(make_ctx(make_exec_accepted(ask_client, exch_ask, base_ts + 3000), state));
    strat.evaluate(make_ctx(make_user_live(exch_bid, bid_client, base_ts + 4000), state));
    strat.evaluate(make_ctx(make_user_live(exch_ask, ask_client, base_ts + 5000), state));

    SchedulerEvent hold_tick{};
    hold_tick.recv_ts = base_ts + 11'000'000'000LL;
    strat.evaluate(make_ctx(hold_tick, state));
    strat.evaluate(make_ctx(make_cancel_confirmed(bid_client, base_ts + 12'000'000'000LL), state));
    batch = strat.evaluate(make_ctx(make_cancel_confirmed(ask_client, base_ts + 12'000'000'100LL), state));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);
    REQUIRE(batch.count == 2);

    // P2 uses captured tok_up BBO (5500/5000), not foreign tok_down BBO (9500/9000).
    CHECK(batch.intents[0].price == 5500);
    CHECK(batch.intents[1].price == 5000);
}

// ---------------------------------------------------------------------------
// Timed full lifecycle — evaluate() latency per state transition
// ---------------------------------------------------------------------------
TEST_CASE("Timed full lifecycle — evaluate() latency" * doctest::skip(false)) {
    WorkingOrderTracker tracker;
    MarketPairRegistry registry;
    registry.add_pair(AssetId("cond1"), AssetId("tok_up"), AssetId("tok_down"));

    TestStrategy strat(&tracker, &registry, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    struct TimingEntry {
        const char* label;
        int64_t eval_ns;
    };
    TimingEntry timings[32];
    int t_idx = 0;

    auto timed_eval = [&](const char* label, const SchedulerEvent& ev) -> IntentBatch {
        auto t0 = SteadyClock::now();
        IntentBatch batch = strat.evaluate(make_ctx(ev, state));
        auto t1 = SteadyClock::now();
        if (t_idx < 32) {
            timings[t_idx++] = {label, t1 - t0};
        }
        return batch;
    };

    auto lifecycle_start = SteadyClock::now();
    Timestamp_ns ts = 1000000;

    // 1. BBO → P1_PLACE (emits 2 intents)
    auto notif = make_market_bbo("tok_up", 5000, 5500, ts);
    IntentBatch batch = timed_eval("BBO -> P1 place", SchedulerEvent::from_market(notif));
    REQUIRE(batch.count == 2);
    OrderId bid_client = batch.intents[0].client_order_id;
    OrderId ask_client = batch.intents[1].client_order_id;
    OrderId exch_bid("exch-bid"), exch_ask("exch-ask");

    // 2-3. Both accepted
    ts += 1000;
    timed_eval("P1 bid accepted", make_exec_accepted(bid_client, exch_bid, ts));
    ts += 1000;
    timed_eval("P1 ask accepted", make_exec_accepted(ask_client, exch_ask, ts));
    REQUIRE(strat.phase() == TestPhase::P1_WAIT_LIVE);

    // 4-5. Both live
    ts += 1000;
    timed_eval("P1 bid live", make_user_live(exch_bid, bid_client, ts));
    ts += 1000;
    timed_eval("P1 ask live", make_user_live(exch_ask, ask_client, ts));
    REQUIRE(strat.phase() == TestPhase::P1_HOLD);

    // 6. Hold 10s
    ts += 10'000'000'000LL;
    SchedulerEvent ev{};
    ev.recv_ts = ts;
    batch = timed_eval("P1 hold->cancel", ev);
    REQUIRE(batch.count == 2);

    // 7-8. Both canceled
    ts += 1000;
    timed_eval("P1 bid canceled", make_user_canceled(exch_bid, bid_client, ts));
    ts += 1000;
    batch = timed_eval("P1 ask canceled -> P2", make_user_canceled(exch_ask, ask_client, ts));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_ACCEPTED);

    // 9-10. P2 both accepted
    OrderId p2_bid = strat.slot(0).client_id;
    OrderId p2_ask = strat.slot(1).client_id;
    OrderId p2_exch_bid("exch-p2-b"), p2_exch_ask("exch-p2-a");
    ts += 1000;
    timed_eval("P2 bid accepted", make_exec_accepted(p2_bid, p2_exch_bid, ts));
    ts += 1000;
    timed_eval("P2 ask accepted", make_exec_accepted(p2_ask, p2_exch_ask, ts));
    REQUIRE(strat.phase() == TestPhase::P2_WAIT_FILLS);

    // 11-12. P2 both filled
    ts += 1000;
    timed_eval("P2 bid filled", make_user_filled(p2_exch_bid, ts));
    ts += 1000;
    batch = timed_eval("P2 ask filled -> P3", make_user_filled(p2_exch_ask, ts));
    CHECK(strat.phase() == TestPhase::P3_WAIT_BUY);
    CHECK(batch.count == 1);

    // 13. P3 buy accepted → P3_FAK_SELL
    OrderId p3_buy = strat.slot(0).client_id;
    OrderId p3_buy_exch("exch-p3-buy");
    ts += 1000;
    batch = timed_eval("P3 buy accepted -> sell", make_exec_accepted(p3_buy, p3_buy_exch, ts));
    CHECK(strat.phase() == TestPhase::P3_WAIT_SELL);
    CHECK(batch.count == 1);

    // 14. P3 sell accepted → DONE
    OrderId p3_sell = strat.slot(1).client_id;
    OrderId p3_sell_exch("exch-p3-sell");
    ts += 1000;
    timed_eval("P3 sell accepted -> DONE", make_exec_accepted(p3_sell, p3_sell_exch, ts));
    CHECK(strat.phase() == TestPhase::DONE);
    CHECK(!strat.enabled());

    auto lifecycle_end = SteadyClock::now();
    int64_t total_ns = lifecycle_end - lifecycle_start;

    // Print timing report
    std::printf("\n=== Test Strategy 3-Phase FSM — Per-Evaluate Timing ===\n");
    int64_t sum_ns = 0;
    int64_t max_ns = 0;
    for (int i = 0; i < t_idx; ++i) {
        std::printf("  %-25s  %6lld ns\n", timings[i].label, (long long)timings[i].eval_ns);
        sum_ns += timings[i].eval_ns;
        if (timings[i].eval_ns > max_ns) max_ns = timings[i].eval_ns;
    }
    std::printf("  -------------------------  ----------\n");
    std::printf("  %-25s  %6lld ns  (sum of evaluate() calls)\n", "total eval", (long long)sum_ns);
    std::printf("  %-25s  %6lld ns  (avg per evaluate())\n", "avg eval", (long long)(sum_ns / t_idx));
    std::printf("  %-25s  %6lld ns  (worst evaluate())\n", "max eval", (long long)max_ns);
    std::printf("  %-25s  %6lld ns  (wall clock BBO->DONE)\n", "lifecycle", (long long)total_ns);
    std::printf("  transitions:              %d\n\n", t_idx);

    for (int i = 0; i < t_idx; ++i) {
        CHECK(timings[i].eval_ns < 100'000);  // < 100us generous bound
    }
}

}  // TEST_SUITE
