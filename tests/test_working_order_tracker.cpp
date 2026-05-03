#include "doctest/doctest.h"

#include "scheduler/working_order_tracker.h"
#include "exec/exec_feedback.h"
#include "events/user_events.h"

using namespace lt;

namespace {

ExecutionIntent make_bid_intent(const char* client_id, const char* asset,
                                 const char* market, Price_t price, Qty_t qty) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.client_order_id = OrderId(client_id);
    intent.asset_id = AssetId(asset);
    intent.market_id = AssetId(market);
    intent.price = price;
    intent.qty = qty;
    intent.created_ts = 1000;
    return intent;
}

ExecutionIntent make_ask_intent(const char* client_id, const char* asset,
                                 const char* market, Price_t price, Qty_t qty) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.client_order_id = OrderId(client_id);
    intent.asset_id = AssetId(asset);
    intent.market_id = AssetId(market);
    intent.price = price;
    intent.qty = qty;
    intent.created_ts = 1000;
    return intent;
}

SchedulerEvent make_order_accepted(const char* client_id, const char* exchange_id = "") {
    SchedulerEvent ev;
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::ORDER_ACCEPTED);
    ev.client_order_id = OrderId(client_id);
    if (exchange_id[0]) ev.order_id = OrderId(exchange_id);
    return ev;
}

SchedulerEvent make_order_rejected(const char* client_id) {
    SchedulerEvent ev;
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_REJECT;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::ORDER_REJECTED);
    ev.client_order_id = OrderId(client_id);
    return ev;
}

SchedulerEvent make_cancel_confirmed(const char* exchange_id, const char* client_id = "") {
    SchedulerEvent ev;
    ev.source = EventSource::EXEC_INTERNAL;
    ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_feedback_kind = static_cast<uint8_t>(ExecFeedbackKind::CANCEL_CONFIRMED);
    ev.order_id = OrderId(exchange_id);
    if (client_id[0]) ev.client_order_id = OrderId(client_id);
    return ev;
}

SchedulerEvent make_user_order_filled(const char* exchange_id, const char* client_id = "") {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::FILLED);
    ev.order_id = OrderId(exchange_id);
    if (client_id[0]) ev.client_order_id = OrderId(client_id);
    return ev;
}

SchedulerEvent make_user_order_canceled(const char* exchange_id) {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::CANCELED);
    ev.order_id = OrderId(exchange_id);
    return ev;
}

SchedulerEvent make_user_order_partial(const char* exchange_id, Qty_t filled) {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::PARTIAL);
    ev.order_id = OrderId(exchange_id);
    ev.user_cumulative_filled = filled;
    return ev;
}

SchedulerEvent make_user_order_live(const char* exchange_id, const char* client_id = "") {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::LIVE);
    ev.order_id = OrderId(exchange_id);
    if (client_id[0]) ev.client_order_id = OrderId(client_id);
    return ev;
}

}  // namespace

TEST_SUITE("WorkingOrderTracker") {

TEST_CASE("Empty tracker") {
    WorkingOrderTracker tracker;
    CHECK(tracker.working_count() == 0);
    CHECK(tracker.total_working_notional() == 0);
    CHECK(tracker.working_bid_price(AssetId("mkt1")) == kInvalidPrice);
    CHECK(tracker.working_ask_price(AssetId("mkt1")) == kInvalidPrice);
}

TEST_CASE("Track single bid placement") {
    WorkingOrderTracker tracker;
    auto intent = make_bid_intent("c1", "token_up", "mkt1", 5000, qty_from_int(10));
    tracker.on_intent_sent(intent);

    CHECK(tracker.working_count() == 1);
    CHECK(tracker.working_count_for_market(AssetId("mkt1")) == 1);
    CHECK(tracker.working_bid_price(AssetId("mkt1")) == 5000);
    CHECK(tracker.working_ask_price(AssetId("mkt1")) == kInvalidPrice);

    auto* wo = tracker.find_by_client_id(OrderId("c1"));
    REQUIRE(wo != nullptr);
    CHECK(wo->is_pending == true);
    CHECK(wo->is_live == false);
    CHECK(wo->price == 5000);
    CHECK(wo->original_size == qty_from_int(10));
}

TEST_CASE("Track bid and ask") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok_up", "mkt1", 4900, qty_from_int(5)));
    tracker.on_intent_sent(make_ask_intent("c2", "tok_up", "mkt1", 5100, qty_from_int(5)));

    CHECK(tracker.working_count() == 2);
    CHECK(tracker.working_bid_price(AssetId("mkt1")) == 4900);
    CHECK(tracker.working_ask_price(AssetId("mkt1")) == 5100);
}

TEST_CASE("ORDER_ACCEPTED sets is_live and exchange_order_id") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));

    auto ack = make_order_accepted("c1", "exch1");
    tracker.on_exec_feedback(ack);

    auto* wo = tracker.find_by_client_id(OrderId("c1"));
    REQUIRE(wo != nullptr);
    CHECK(wo->is_live == true);
    CHECK(wo->is_pending == false);
    CHECK(wo->exchange_order_id == OrderId("exch1"));

    // Also findable by exchange id
    auto* wo2 = tracker.find_by_exchange_id(OrderId("exch1"));
    REQUIRE(wo2 != nullptr);
    CHECK(wo2->client_order_id == OrderId("c1"));
}

TEST_CASE("ORDER_REJECTED removes from tracker") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    CHECK(tracker.working_count() == 1);

    tracker.on_exec_feedback(make_order_rejected("c1"));
    CHECK(tracker.working_count() == 0);
    CHECK(tracker.find_by_client_id(OrderId("c1")) == nullptr);
}

TEST_CASE("CANCEL_CONFIRMED removes by exchange_id") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));
    CHECK(tracker.working_count() == 1);

    tracker.on_exec_feedback(make_cancel_confirmed("exch1"));
    CHECK(tracker.working_count() == 0);
}

TEST_CASE("CANCEL_CONFIRMED falls back to client_id") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    // No exchange_order_id set
    CHECK(tracker.working_count() == 1);

    tracker.on_exec_feedback(make_cancel_confirmed("", "c1"));
    CHECK(tracker.working_count() == 0);
}

TEST_CASE("User WS FILLED removes order") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));

    tracker.on_user_update(make_user_order_filled("exch1"));
    CHECK(tracker.working_count() == 0);
}

TEST_CASE("User WS CANCELED removes order") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));

    tracker.on_user_update(make_user_order_canceled("exch1"));
    CHECK(tracker.working_count() == 0);
}

TEST_CASE("User WS FAILED removes order") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));

    SchedulerEvent failed{};
    failed.source = EventSource::USER_WS;
    failed.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    failed.order_status_raw = static_cast<uint8_t>(OrderStatus::FAILED);
    failed.order_id = OrderId("exch1");
    tracker.on_user_update(failed);

    CHECK(tracker.working_count() == 0);
}

TEST_CASE("User WS PARTIAL updates filled_size") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));

    tracker.on_user_update(make_user_order_partial("exch1", qty_from_int(3)));
    auto* wo = tracker.find_by_exchange_id(OrderId("exch1"));
    REQUIRE(wo != nullptr);
    CHECK(wo->filled_size == qty_from_int(3));
    CHECK(wo->is_live == true);
    CHECK(tracker.working_count() == 1);
}

TEST_CASE("User WS LIVE sets is_live and exchange_order_id") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));

    tracker.on_user_update(make_user_order_live("exch1", "c1"));
    auto* wo = tracker.find_by_client_id(OrderId("c1"));
    REQUIRE(wo != nullptr);
    CHECK(wo->is_live == true);
    CHECK(wo->is_pending == false);
    CHECK(wo->exchange_order_id == OrderId("exch1"));
}

TEST_CASE("Fallback matching handles late order-id mapping") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));

    SchedulerEvent live{};
    live.source = EventSource::USER_WS;
    live.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    live.order_status_raw = static_cast<uint8_t>(OrderStatus::LIVE);
    live.asset_id = AssetId("tok");
    live.user_side = Side::BID;
    live.user_price = 5000;
    live.order_id = OrderId("exch1");
    tracker.on_user_update(live);

    auto* wo = tracker.find_by_client_id(OrderId("c1"));
    REQUIRE(wo != nullptr);
    CHECK(wo->is_live == true);
    CHECK(wo->is_pending == false);
    CHECK(wo->exchange_order_id == OrderId("exch1"));
}

TEST_CASE("total_working_notional calculates correctly") {
    WorkingOrderTracker tracker;
    // Bid at 5000 for 10 lots, ask at 5200 for 5 lots
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_intent_sent(make_ask_intent("c2", "tok", "mkt1", 5200, qty_from_int(5)));

    // notional = 5000*qty_from_int(10) + 5200*qty_from_int(5)
    CHECK(tracker.total_working_notional() == 5000LL * qty_from_int(10) + 5200LL * qty_from_int(5));
}

TEST_CASE("total_working_notional accounts for partial fills") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));
    tracker.on_user_update(make_user_order_partial("exch1", qty_from_int(3)));

    // remaining = qty_from_int(10) - qty_from_int(3) = qty_from_int(7), notional = 5000 * qty_from_int(7)
    CHECK(tracker.total_working_notional() == 5000LL * qty_from_int(7));
}

TEST_CASE("cancel_all_intents produces cancels for all working") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 4900, qty_from_int(5)));
    tracker.on_intent_sent(make_ask_intent("c2", "tok", "mkt1", 5100, qty_from_int(5)));
    tracker.on_exec_feedback(make_order_accepted("c1", "exch1"));
    tracker.on_exec_feedback(make_order_accepted("c2", "exch2"));

    IntentBatch batch = tracker.cancel_all_intents();
    CHECK(batch.count == 2);

    // Verify cancel intents have correct action types
    bool has_cancel_bid = false, has_cancel_ask = false;
    for (int i = 0; i < batch.count; ++i) {
        if (batch.intents[i].action == IntentAction::WOULD_CANCEL_BID) has_cancel_bid = true;
        if (batch.intents[i].action == IntentAction::WOULD_CANCEL_ASK) has_cancel_ask = true;
    }
    CHECK(has_cancel_bid);
    CHECK(has_cancel_ask);
}

TEST_CASE("Cancel intent ignores non-placement actions") {
    WorkingOrderTracker tracker;
    ExecutionIntent cancel_intent;
    cancel_intent.action = IntentAction::WOULD_CANCEL_BID;
    cancel_intent.client_order_id = OrderId("c1");
    cancel_intent.asset_id = AssetId("tok");
    cancel_intent.market_id = AssetId("mkt1");
    cancel_intent.price = 5000;
    cancel_intent.qty = qty_from_int(10);

    tracker.on_intent_sent(cancel_intent);
    CHECK(tracker.working_count() == 0);  // cancels not tracked
}

TEST_CASE("Multiple markets tracked independently") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok1", "mkt1", 4900, qty_from_int(5)));
    tracker.on_intent_sent(make_bid_intent("c2", "tok2", "mkt2", 3000, qty_from_int(10)));

    CHECK(tracker.working_count() == 2);
    CHECK(tracker.working_count_for_market(AssetId("mkt1")) == 1);
    CHECK(tracker.working_count_for_market(AssetId("mkt2")) == 1);
    CHECK(tracker.working_bid_price(AssetId("mkt1")) == 4900);
    CHECK(tracker.working_bid_price(AssetId("mkt2")) == 3000);
}

TEST_CASE("Capacity limit: kMaxWorking orders") {
    WorkingOrderTracker tracker;
    constexpr int cap = WorkingOrderTracker::kMaxWorking;
    for (int i = 0; i < cap; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "c%d", i);
        tracker.on_intent_sent(make_bid_intent(id, "tok", "mkt1", 5000, qty_from_int(1)));
    }
    CHECK(tracker.working_count() == cap);

    // One past capacity should be rejected because tracker is full
    char overflow_id[16];
    std::snprintf(overflow_id, sizeof(overflow_id), "c%d", cap);
    CHECK_FALSE(tracker.on_intent_sent(make_bid_intent(overflow_id, "tok", "mkt1", 5000, qty_from_int(1))));
    CHECK(tracker.working_count() == cap);
    CHECK(tracker.find_by_client_id(OrderId(overflow_id)) == nullptr);
}

TEST_CASE("Slot reuse after removal") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    tracker.on_exec_feedback(make_order_rejected("c1"));
    CHECK(tracker.working_count() == 0);

    tracker.on_intent_sent(make_bid_intent("c2", "tok", "mkt1", 5100, qty_from_int(5)));
    CHECK(tracker.working_count() == 1);
    CHECK(tracker.find_by_client_id(OrderId("c2")) != nullptr);
}

TEST_CASE("Ignores events with non-matching source") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));

    // Market event should be ignored
    SchedulerEvent market_ev;
    market_ev.source = EventSource::MARKET_WS;
    market_ev.kind = SchedulerEventKind::MARKET_BBO_UPDATE;
    tracker.on_exec_feedback(market_ev);
    tracker.on_user_update(market_ev);

    CHECK(tracker.working_count() == 1);  // unchanged
}

TEST_CASE("User FILLED by client_order_id fallback") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    // No exchange_order_id — find by client_order_id

    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::FILLED);
    ev.client_order_id = OrderId("c1");
    tracker.on_user_update(ev);

    CHECK(tracker.working_count() == 0);
}

TEST_CASE("find_by_client_id returns nullptr for unknown") {
    WorkingOrderTracker tracker;
    CHECK(tracker.find_by_client_id(OrderId("nonexistent")) == nullptr);
}

TEST_CASE("find_by_exchange_id returns nullptr for unknown") {
    WorkingOrderTracker tracker;
    CHECK(tracker.find_by_exchange_id(OrderId("nonexistent")) == nullptr);
}

TEST_CASE("find with empty OrderId returns nullptr") {
    WorkingOrderTracker tracker;
    tracker.on_intent_sent(make_bid_intent("c1", "tok", "mkt1", 5000, qty_from_int(10)));
    CHECK(tracker.find_by_client_id(OrderId()) == nullptr);
    CHECK(tracker.find_by_exchange_id(OrderId()) == nullptr);
}

TEST_CASE("cancel_all_intents on empty tracker") {
    WorkingOrderTracker tracker;
    IntentBatch batch = tracker.cancel_all_intents();
    CHECK(batch.count == 0);
}

}  // TEST_SUITE
