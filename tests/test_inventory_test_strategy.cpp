#include "doctest/doctest.h"

#include "common/market_pair.h"
#include "common/types.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "inventory/inventory_sink.h"
#include "scheduler/inventory_test_strategy.h"
#include "scheduler/strategy_book_store.h"
#include "scheduler/strategy_context.h"
#include "scheduler/strategy_state_stub.h"

using namespace lt;

namespace {

StrategyBookStore g_inv_books;

// Stub InventoryOpSink that records requests for verification.
struct StubInventoryOpSink : public InventoryOpSink {
    std::vector<InventoryOpRequest> requests;
    bool fail_next = false;

    bool try_request(const InventoryOpRequest& request) override {
        if (fail_next) {
            fail_next = false;
            return false;
        }
        requests.push_back(request);
        return true;
    }
};

// Helper: build context and evaluate in one call (avoids StrategyContext reassignment).
IntentBatch eval(InventoryTestStrategy& strat,
                 const SchedulerEvent& event,
                 const StrategyStateStub& state,
                 InventoryOpSink* ops = nullptr) {
    StrategyContext ctx{event, g_inv_books, nullptr, ops, nullptr, nullptr, state, 0};
    return strat.evaluate(ctx);
}

MarketNotification make_bbo(const char* asset, Price_t bid, Price_t ask,
                            Timestamp_ns ts = 1'000'000) {
    MarketNotification n{};
    n.asset_id = AssetId(asset);
    n.bbo.best_bid = bid;
    n.bbo.best_ask = ask;
    n.kind = NotificationKind::BBO_UPDATE;
    n.recv_ts = ts;
    return n;
}

SchedulerEvent make_resolved(const char* condition_id, const char* winning_asset_id,
                              Timestamp_ns ts = 50'000'000'000LL) {
    SchedulerEvent ev{};
    ev.source = EventSource::MARKET_WS;
    ev.kind = SchedulerEventKind::MARKET_RESOLVED;
    ev.resolved_condition_id = AssetId(condition_id);
    ev.resolved_winning_asset_id = AssetId(winning_asset_id);
    ev.recv_ts = ts;
    return ev;
}

// Setup a registry with one market pair.
MarketPairRegistry setup_registry(const char* condition = "cond_abc",
                                   const char* token_up = "token_up_1",
                                   const char* token_down = "token_down_1") {
    MarketPairRegistry reg;
    MarketPair pair;
    pair.condition_id = AssetId(condition);
    pair.token_id_up = AssetId(token_up);
    pair.token_id_down = AssetId(token_down);
    reg.add_pair(pair);
    return reg;
}

// Drive strategy through SPLIT + hold + MERGE, arriving at WAIT_RESOLVED.
void drive_to_wait_resolved(InventoryTestStrategy& strat,
                             StubInventoryOpSink& sink,
                             const StrategyStateStub& state,
                             Timestamp_ns ts0 = 1'000'000) {
    auto n1 = make_bbo("token_up_1", 5000, 5100, ts0);
    auto e1 = SchedulerEvent::from_market(n1);
    eval(strat, e1, state, &sink);

    Timestamp_ns ts_10s = ts0 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    eval(strat, e2, state, &sink);
}

}  // namespace

TEST_SUITE("InventoryTestStrategy") {

TEST_CASE("Disabled strategy returns empty batch") {
    auto reg = setup_registry();
    InventoryTestStrategy strat(&reg, "");
    CHECK(!strat.enabled());

    StrategyStateStub state;
    auto notif = make_bbo("token_up_1", 5000, 5100);
    auto ev = SchedulerEvent::from_market(notif);
    auto batch = eval(strat, ev, state);
    CHECK(batch.count == 0);
    CHECK(strat.phase() == InvTestPhase::WAIT_FOR_BBO);
}

TEST_CASE("Non-market events ignored in WAIT_FOR_BBO") {
    auto reg = setup_registry();
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;
    SchedulerEvent ev{};
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.recv_ts = 1'000'000;

    auto batch = eval(strat, ev, state);
    CHECK(batch.count == 0);
    CHECK(strat.phase() == InvTestPhase::WAIT_FOR_BBO);
}

TEST_CASE("Invalid BBO ignored (missing bid or ask)") {
    auto reg = setup_registry();
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;

    // Invalid bid
    {
        auto notif = make_bbo("token_up_1", kInvalidPrice, 5100);
        auto ev = SchedulerEvent::from_market(notif);
        eval(strat, ev, state);
        CHECK(strat.phase() == InvTestPhase::WAIT_FOR_BBO);
    }
    // Invalid ask
    {
        auto notif = make_bbo("token_up_1", 5000, kInvalidPrice);
        auto ev = SchedulerEvent::from_market(notif);
        eval(strat, ev, state);
        CHECK(strat.phase() == InvTestPhase::WAIT_FOR_BBO);
    }
}

TEST_CASE("Unregistered asset ignored") {
    auto reg = setup_registry();
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;
    auto notif = make_bbo("unknown_asset", 5000, 5100);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state);
    CHECK(strat.phase() == InvTestPhase::WAIT_FOR_BBO);
}

TEST_CASE("Valid BBO captures condition_id and fires SPLIT") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;
    auto notif = make_bbo("token_up_1", 5000, 5100);
    auto ev = SchedulerEvent::from_market(notif);
    auto batch = eval(strat, ev, state, &sink);

    CHECK(batch.count == 0);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);
    CHECK(strat.captured_condition_id() == AssetId("cond_abc"));
    REQUIRE(sink.requests.size() == 1);
    CHECK(sink.requests[0].type == InventoryOpType::SPLIT);
    CHECK(sink.requests[0].condition_id == AssetId("cond_abc"));
    CHECK(sink.requests[0].quantity == 2 * kQtyScale);
    CHECK(sink.requests[0].request_id == 0);
}

TEST_CASE("Null inventory_ops aborts to DONE on SPLIT") {
    auto reg = setup_registry();
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;
    auto notif = make_bbo("token_up_1", 5000, 5100);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, nullptr);

    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
}

TEST_CASE("try_request failure aborts to DONE on SPLIT") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    sink.fail_next = true;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;
    auto notif = make_bbo("token_up_1", 5000, 5100);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);

    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
    CHECK(sink.requests.empty());
}

TEST_CASE("Hold timer does not advance before 10s") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;

    // BBO -> SPLIT
    auto notif = make_bbo("token_up_1", 5000, 5100, 1'000'000);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);

    // Tick at 5s — should NOT advance
    Timestamp_ns ts_5s = 1'000'000 + 5'000'000'000LL;
    auto notif2 = make_bbo("token_up_1", 5000, 5100, ts_5s);
    auto ev2 = SchedulerEvent::from_market(notif2);
    eval(strat, ev2, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);
    CHECK(sink.requests.size() == 1);
}

TEST_CASE("Hold timer triggers MERGE after 10s") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);

    StrategyStateStub state;

    Timestamp_ns ts0 = 1'000'000;
    auto notif = make_bbo("token_up_1", 5000, 5100, ts0);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);

    Timestamp_ns ts_10s = ts0 + 10'000'000'000LL;
    auto notif2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto ev2 = SchedulerEvent::from_market(notif2);
    eval(strat, ev2, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_RESOLVED);
    REQUIRE(sink.requests.size() == 2);
    CHECK(sink.requests[1].type == InventoryOpType::MERGE);
    CHECK(sink.requests[1].condition_id == AssetId("cond_abc"));
    CHECK(sink.requests[1].quantity == 1 * kQtyScale);
    CHECK(sink.requests[1].request_id == 1);
}

TEST_CASE("WAIT_RESOLVED ignores non-resolution events") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    drive_to_wait_resolved(strat, sink, state);
    REQUIRE(strat.phase() == InvTestPhase::WAIT_RESOLVED);

    // Non-resolution market event
    Timestamp_ns ts_later = 1'000'000 + 11'000'000'000LL;
    auto notif = make_bbo("token_up_1", 5000, 5200, ts_later);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_RESOLVED);
    CHECK(sink.requests.size() == 2);
}

TEST_CASE("WAIT_RESOLVED ignores wrong condition_id") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    drive_to_wait_resolved(strat, sink, state);
    REQUIRE(strat.phase() == InvTestPhase::WAIT_RESOLVED);

    auto wrong = make_resolved("wrong_condition", "some_winner");
    eval(strat, wrong, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_RESOLVED);
    CHECK(sink.requests.size() == 2);
}

TEST_CASE("Full 3-phase happy path: SPLIT -> MERGE -> REDEEM") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    // Phase 1: BBO -> SPLIT
    Timestamp_ns ts0 = 1'000'000;
    auto n1 = make_bbo("token_up_1", 5000, 5100, ts0);
    auto e1 = SchedulerEvent::from_market(n1);
    eval(strat, e1, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);
    REQUIRE(sink.requests.size() == 1);

    // Phase 2: 10s hold -> MERGE
    Timestamp_ns ts_10s = ts0 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    eval(strat, e2, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_RESOLVED);
    REQUIRE(sink.requests.size() == 2);

    // Phase 3: MARKET_RESOLVED -> REDEEM
    auto resolve = make_resolved("cond_abc", "winning_token_xyz");
    eval(strat, resolve, state, &sink);
    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
    REQUIRE(sink.requests.size() == 3);

    CHECK(sink.requests[0].type == InventoryOpType::SPLIT);
    CHECK(sink.requests[0].quantity == 2 * kQtyScale);
    CHECK(sink.requests[1].type == InventoryOpType::MERGE);
    CHECK(sink.requests[1].quantity == 1 * kQtyScale);
    CHECK(sink.requests[2].type == InventoryOpType::REDEEM);
    CHECK(sink.requests[2].quantity == 0);
}

TEST_CASE("REDEEM token_id matches resolved_winning_asset_id") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    drive_to_wait_resolved(strat, sink, state);
    REQUIRE(strat.phase() == InvTestPhase::WAIT_RESOLVED);

    auto resolve = make_resolved("cond_abc", "the_winner_token_42");
    eval(strat, resolve, state, &sink);
    REQUIRE(sink.requests.size() == 3);
    CHECK(sink.requests[2].token_id == AssetId("the_winner_token_42"));
}

TEST_CASE("Request IDs increment correctly") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    CHECK(strat.next_request_id() == 0);

    // SPLIT (request_id = 0)
    auto n1 = make_bbo("token_up_1", 5000, 5100, 1'000'000);
    auto e1 = SchedulerEvent::from_market(n1);
    eval(strat, e1, state, &sink);
    CHECK(strat.next_request_id() == 1);

    // MERGE (request_id = 1)
    Timestamp_ns ts_10s = 1'000'000 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    eval(strat, e2, state, &sink);
    CHECK(strat.next_request_id() == 2);

    // REDEEM (request_id = 2)
    auto resolve = make_resolved("cond_abc", "winner");
    eval(strat, resolve, state, &sink);
    CHECK(strat.next_request_id() == 3);

    REQUIRE(sink.requests.size() == 3);
    CHECK(sink.requests[0].request_id == 0);
    CHECK(sink.requests[1].request_id == 1);
    CHECK(sink.requests[2].request_id == 2);
}

TEST_CASE("No order intents emitted in any phase") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    Timestamp_ns ts0 = 1'000'000;
    auto n1 = make_bbo("token_up_1", 5000, 5100, ts0);
    auto e1 = SchedulerEvent::from_market(n1);
    auto b1 = eval(strat, e1, state, &sink);
    CHECK(b1.count == 0);

    Timestamp_ns ts_10s = ts0 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    auto b2 = eval(strat, e2, state, &sink);
    CHECK(b2.count == 0);

    auto resolve = make_resolved("cond_abc", "winner");
    auto b3 = eval(strat, resolve, state, &sink);
    CHECK(b3.count == 0);
}

TEST_CASE("Null inventory_ops aborts to DONE on MERGE") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    // BBO -> SPLIT (with sink)
    Timestamp_ns ts0 = 1'000'000;
    auto notif = make_bbo("token_up_1", 5000, 5100, ts0);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);
    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);

    // 10s hold -> MERGE attempt without sink
    Timestamp_ns ts_10s = ts0 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    eval(strat, e2, state, nullptr);
    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
}

TEST_CASE("try_request failure aborts to DONE on MERGE") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    // BBO -> SPLIT
    auto notif = make_bbo("token_up_1", 5000, 5100, 1'000'000);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);

    // Fail the MERGE request
    sink.fail_next = true;
    Timestamp_ns ts_10s = 1'000'000 + 10'000'000'000LL;
    auto n2 = make_bbo("token_up_1", 5000, 5100, ts_10s);
    auto e2 = SchedulerEvent::from_market(n2);
    eval(strat, e2, state, &sink);
    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
    CHECK(sink.requests.size() == 1);
}

TEST_CASE("Null inventory_ops aborts to DONE on REDEEM") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    drive_to_wait_resolved(strat, sink, state);
    REQUIRE(strat.phase() == InvTestPhase::WAIT_RESOLVED);

    // REDEEM attempt without sink
    auto resolve = make_resolved("cond_abc", "winner");
    eval(strat, resolve, state, nullptr);
    CHECK(strat.phase() == InvTestPhase::DONE);
    CHECK(!strat.enabled());
}

TEST_CASE("BBO from down token also captures condition_id") {
    auto reg = setup_registry();
    StubInventoryOpSink sink;
    InventoryTestStrategy strat(&reg, "");
    strat.set_enabled(true);
    StrategyStateStub state;

    auto notif = make_bbo("token_down_1", 4800, 4900, 1'000'000);
    auto ev = SchedulerEvent::from_market(notif);
    eval(strat, ev, state, &sink);

    CHECK(strat.phase() == InvTestPhase::WAIT_SPLIT_HOLD);
    CHECK(strat.captured_condition_id() == AssetId("cond_abc"));
    REQUIRE(sink.requests.size() == 1);
    CHECK(sink.requests[0].condition_id == AssetId("cond_abc"));
}

}  // TEST_SUITE
