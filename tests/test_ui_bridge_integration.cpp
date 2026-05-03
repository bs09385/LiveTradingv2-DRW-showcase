#include "doctest/doctest.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "common/types.h"
#include "events/scheduler_events.h"
#include "exec/exec_feedback.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/strategy_scheduler.h"
#include "scheduler/working_order_tracker.h"
#include "ui_bridge/ui_command_parser.h"
#include "ui_bridge/ui_serializer.h"
#include "ui_bridge/ui_types.h"

namespace {

lt::SchedulerConfig ui_sched_cfg() {
    lt::SchedulerConfig cfg;
    cfg.stats_interval_ms = 60000;
    cfg.poll_strategy = 1;
    cfg.sleep_us = 50;
    cfg.strategy_stub_emit_intents = false;
    cfg.exec_feedback_loop_enabled = false;
    return cfg;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

}  // namespace

TEST_SUITE("UiBridgeIntegration") {

TEST_CASE("Queue drain keeps latest per asset") {
    lt::SpscQueue<lt::UiBookUpdate> queue(16);

    // Push 3 updates for same asset — latest should win
    for (int i = 0; i < 3; ++i) {
        lt::UiBookUpdate upd{};
        upd.asset_id = lt::AssetId("token_A");
        upd.bbo.best_bid = 1000 + i * 100;
        upd.timestamp = i + 1;
        CHECK(queue.try_push(upd));
    }

    // Push 1 update for different asset
    {
        lt::UiBookUpdate upd{};
        upd.asset_id = lt::AssetId("token_B");
        upd.bbo.best_bid = 9000;
        upd.timestamp = 10;
        CHECK(queue.try_push(upd));
    }

    // Drain latest per asset (simulating T6 logic)
    std::unordered_map<lt::AssetId, lt::UiBookUpdate, lt::AssetIdHash> latest;
    while (auto* item = queue.front()) {
        latest[item->asset_id] = *item;
        queue.pop();
    }

    CHECK(latest.size() == 2);
    CHECK(latest[lt::AssetId("token_A")].bbo.best_bid == 1200);  // last one
    CHECK(latest[lt::AssetId("token_A")].timestamp == 3);
    CHECK(latest[lt::AssetId("token_B")].bbo.best_bid == 9000);
}

TEST_CASE("State queue drain keeps latest only") {
    lt::SpscQueue<lt::UiStateSnapshot> queue(16);

    // Push 3 state snapshots
    for (int i = 0; i < 3; ++i) {
        lt::UiStateSnapshot snap{};
        snap.spread_ticks = i + 1;
        snap.timestamp = i + 1;
        CHECK(queue.try_push(snap));
    }

    // Drain — keep latest
    lt::UiStateSnapshot latest{};
    bool has = false;
    while (auto* item = queue.front()) {
        latest = *item;
        has = true;
        queue.pop();
    }

    CHECK(has);
    CHECK(latest.spread_ticks == 3);
    CHECK(latest.timestamp == 3);
}

TEST_CASE("Command to control queue roundtrip") {
    lt::SpscQueue<lt::SchedulerEvent> control_queue(16);

    // Parse a UI command
    auto cmd = lt::parse_ui_command(R"({"cmd":"set_mode","mode":"LIVE"})");
    REQUIRE(cmd.valid);

    // Push to control queue (simulating T6 logic)
    CHECK(control_queue.try_push(cmd.event));

    // Pop from control queue (simulating T2 logic)
    auto* ev = control_queue.front();
    REQUIRE(ev != nullptr);
    CHECK(ev->source == lt::EventSource::CONTROL);
    CHECK(ev->kind == lt::SchedulerEventKind::CONTROL_SET_MODE);
    CHECK(ev->control_mode == 1);  // LIVE
    control_queue.pop();
}

TEST_CASE("Multiple commands in sequence") {
    lt::SpscQueue<lt::SchedulerEvent> control_queue(16);

    const char* commands[] = {
        R"({"cmd":"enable_strategy"})",
        R"({"cmd":"cancel_all"})",
    };

    for (const auto* json : commands) {
        auto cmd = lt::parse_ui_command(json);
        REQUIRE(cmd.valid);
        CHECK(control_queue.try_push(cmd.event));
    }

    CHECK(control_queue.size() == 2);

    // Verify order preserved (FIFO)
    auto* e1 = control_queue.front();
    CHECK(e1->kind == lt::SchedulerEventKind::CONTROL_ENABLE_STRATEGY);
    control_queue.pop();

    auto* e2 = control_queue.front();
    CHECK(e2->kind == lt::SchedulerEventKind::CONTROL_CANCEL_ALL);
    control_queue.pop();
}

TEST_CASE("Inventory command roundtrip") {
    lt::SpscQueue<lt::SchedulerEvent> control_queue(16);

    auto cmd = lt::parse_ui_command(
        R"({"cmd":"inventory_split","condition_id":"cond-1","shares":12})");
    REQUIRE(cmd.valid);
    CHECK(control_queue.try_push(cmd.event));

    auto* ev = control_queue.front();
    REQUIRE(ev != nullptr);
    CHECK(ev->kind == lt::SchedulerEventKind::CONTROL_INVENTORY_SPLIT);
    CHECK(ev->control_condition_id == lt::AssetId("cond-1"));
    CHECK(ev->control_qty_param == lt::qty_from_int(12));
    control_queue.pop();
}

TEST_CASE("Snapshot assembly with mock data") {
    // Simulate what T6 does: assemble EngineSnapshot from components
    lt::UiBookUpdate book_up{};
    book_up.asset_id = lt::AssetId("tok_up_1");
    book_up.bbo.best_bid = 5200;
    book_up.bbo.best_ask = 5400;
    book_up.bids[0] = {5200, 100};
    book_up.bid_count = 1;
    book_up.asks[0] = {5400, 50};
    book_up.ask_count = 1;

    lt::UiStateSnapshot state{};
    state.strategy_enabled = true;
    state.spread_ticks = 1;
    state.quote_size = 5;
    state.execution_mode = 0;
    state.working_order_count = 0;
    state.closed_orders[0].client_order_id = lt::OrderId("c1");
    state.closed_orders[0].market_id = lt::AssetId("cond_1");
    state.closed_orders[0].asset_id = lt::AssetId("tok_up_1");
    state.closed_orders[0].lifecycle_state =
        static_cast<uint8_t>(lt::UiOrderLifecycleState::FILLED);
    state.closed_order_count = 1;
    state.trades[0].trade_id = lt::TradeId("t1");
    state.trades[0].market_id = lt::AssetId("cond_1");
    state.trades[0].asset_id = lt::AssetId("tok_up_1");
    state.trades[0].trade_status = 1;  // MATCHED
    state.trade_count = 1;
    state.positions[0].token_id = lt::AssetId("tok_up_1");
    state.positions[0].position = 9;
    state.position_count = 1;

    lt::UiMarketSnapshot mkt;
    mkt.condition_id = lt::AssetId("cond_1");
    mkt.token_id_up = lt::AssetId("tok_up_1");
    mkt.token_id_down = lt::AssetId("tok_down_1");
    mkt.series_label = "BTC 5M";
    mkt.book_up = &book_up;
    mkt.book_down = nullptr;
    mkt.position_up = 5;
    mkt.position_down = 0;

    lt::EngineSnapshot snap{};
    snap.timestamp_ns = 1234567890;
    snap.markets.push_back(std::move(mkt));
    snap.state = &state;
    snap.metrics.ws_frames = 1000;
    snap.gateway.degraded = false;

    std::string json = lt::serialize_engine_snapshot(snap);

    // Verify key fields present
    CHECK(json.find("\"timestamp_ns\":1234567890") != std::string::npos);
    CHECK(json.find("\"condition_id\":\"cond_1\"") != std::string::npos);
    CHECK(json.find("\"best_bid\":5200") != std::string::npos);
    CHECK(json.find("\"position_up\":5") != std::string::npos);
    CHECK(json.find("\"strategy_enabled\":true") != std::string::npos);
    CHECK(json.find("\"closed_orders\":[") != std::string::npos);
    CHECK(json.find("\"market_label\":\"BTC 5M UP\"") != std::string::npos);
    CHECK(json.find("\"trades\":[") != std::string::npos);
    CHECK(json.find("\"positions\":[{\"token_id\":\"tok_up_1\",\"position\":9}]") != std::string::npos);
    CHECK(json.find("\"ws_frames\":1000") != std::string::npos);
    CHECK(json.find("\"degraded\":false") != std::string::npos);
}

TEST_CASE("Working to terminal transition clears stale working rows") {
    lt::SpscQueue<lt::MarketNotification> market_q(128);
    lt::SpscQueue<lt::SchedulerEvent> user_q(128);
    lt::SpscQueue<lt::SchedulerEvent> exec_q(128);
    lt::SpscQueue<lt::SchedulerEvent> control_q(32);
    lt::SpscQueue<lt::UiStateSnapshot> ui_state_q(256);
    lt::Metrics metrics;
    lt::AsyncLogger logger("test_ui_lifecycle_stale.log", 1024, lt::LogLevel::WARN);
    logger.start();

    lt::WorkingOrderTracker tracker;
    lt::StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, ui_sched_cfg(),
                                    nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, &tracker);
    scheduler.set_ui_state_queue(&ui_state_q, 1000);

    lt::ExecutionIntent intent{};
    intent.action = lt::IntentAction::WOULD_PLACE_BID;
    intent.client_order_id = lt::OrderId("cid-1");
    intent.asset_id = lt::AssetId("tok-up");
    intent.market_id = lt::AssetId("cond-1");
    intent.price = 5000;
    intent.qty = 10;
    intent.created_ts = 1;
    REQUIRE(tracker.on_intent_sent(intent));

    std::thread t([&]() { scheduler.run(); });

    lt::SchedulerEvent ack{};
    ack.source = lt::EventSource::EXEC_INTERNAL;
    ack.kind = lt::SchedulerEventKind::EXEC_ORDER_ACK;
    ack.exec_feedback_kind = static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_ACCEPTED);
    ack.client_order_id = lt::OrderId("cid-1");
    ack.recv_ts = 100;
    REQUIRE(exec_q.try_push(ack));

    lt::SchedulerEvent live{};
    live.source = lt::EventSource::USER_WS;
    live.kind = lt::SchedulerEventKind::USER_ORDER_UPDATE;
    live.order_status_raw = static_cast<uint8_t>(lt::OrderStatus::LIVE);
    live.order_id = lt::OrderId("ex-1");
    live.asset_id = lt::AssetId("tok-up");
    live.user_market_id = lt::AssetId("cond-1");
    live.user_side = lt::Side::BID;
    live.user_price = 5000;
    live.user_original_size = 10;
    live.recv_ts = 101;
    REQUIRE(user_q.try_push(live));

    lt::SchedulerEvent canceled = live;
    canceled.order_status_raw = static_cast<uint8_t>(lt::OrderStatus::CANCELED);
    canceled.recv_ts = 102;
    REQUIRE(user_q.try_push(canceled));

    lt::UiStateSnapshot latest{};
    bool found_terminal = wait_until([&]() {
        bool changed = false;
        while (auto* snap = ui_state_q.front()) {
            latest = *snap;
            ui_state_q.pop();
            changed = true;
        }
        return changed && latest.working_order_count == 0 && latest.closed_order_count >= 1;
    });

    scheduler.request_shutdown();
    t.join();
    logger.stop();
    std::remove("test_ui_lifecycle_stale.log");

    REQUIRE(found_terminal);
    CHECK(latest.working_order_count == 0);
    CHECK(latest.closed_order_count == 1);
    CHECK(latest.closed_orders[0].lifecycle_state ==
          static_cast<uint8_t>(lt::UiOrderLifecycleState::CANCELED_NO_FILL));
}

TEST_CASE("Rejected orders are terminal closed entries without duplicates") {
    lt::SpscQueue<lt::MarketNotification> market_q(128);
    lt::SpscQueue<lt::SchedulerEvent> user_q(128);
    lt::SpscQueue<lt::SchedulerEvent> exec_q(128);
    lt::SpscQueue<lt::SchedulerEvent> control_q(32);
    lt::SpscQueue<lt::UiStateSnapshot> ui_state_q(256);
    lt::Metrics metrics;
    lt::AsyncLogger logger("test_ui_lifecycle_rejected.log", 1024, lt::LogLevel::WARN);
    logger.start();

    lt::WorkingOrderTracker tracker;
    lt::StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, ui_sched_cfg(),
                                    nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, &tracker);
    scheduler.set_ui_state_queue(&ui_state_q, 1000);

    lt::ExecutionIntent intent{};
    intent.action = lt::IntentAction::WOULD_PLACE_ASK;
    intent.client_order_id = lt::OrderId("cid-r");
    intent.asset_id = lt::AssetId("tok-down");
    intent.market_id = lt::AssetId("cond-r");
    intent.price = 5100;
    intent.qty = 7;
    intent.created_ts = 1;
    REQUIRE(tracker.on_intent_sent(intent));

    std::thread t([&]() { scheduler.run(); });

    lt::SchedulerEvent reject{};
    reject.source = lt::EventSource::EXEC_INTERNAL;
    reject.kind = lt::SchedulerEventKind::EXEC_ORDER_REJECT;
    reject.exec_feedback_kind = static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_REJECTED);
    reject.client_order_id = lt::OrderId("cid-r");
    reject.recv_ts = 200;
    REQUIRE(exec_q.try_push(reject));

    lt::SchedulerEvent failed{};
    failed.source = lt::EventSource::USER_WS;
    failed.kind = lt::SchedulerEventKind::USER_ORDER_UPDATE;
    failed.order_status_raw = static_cast<uint8_t>(lt::OrderStatus::FAILED);
    failed.order_id = lt::OrderId("ex-r");
    failed.asset_id = lt::AssetId("tok-down");
    failed.user_market_id = lt::AssetId("cond-r");
    failed.user_side = lt::Side::ASK;
    failed.user_price = 5100;
    failed.user_original_size = 7;
    failed.recv_ts = 201;
    REQUIRE(user_q.try_push(failed));

    lt::UiStateSnapshot latest{};
    bool found_rejected = wait_until([&]() {
        bool changed = false;
        while (auto* snap = ui_state_q.front()) {
            latest = *snap;
            ui_state_q.pop();
            changed = true;
        }
        return changed && latest.working_order_count == 0 &&
               latest.closed_order_count >= 1 &&
               latest.closed_orders[0].lifecycle_state ==
                   static_cast<uint8_t>(lt::UiOrderLifecycleState::REJECTED);
    });

    scheduler.request_shutdown();
    t.join();
    logger.stop();
    std::remove("test_ui_lifecycle_rejected.log");

    REQUIRE(found_rejected);
    CHECK(latest.working_order_count == 0);
    CHECK(latest.closed_order_count == 1);
    CHECK(latest.closed_orders[0].lifecycle_state ==
          static_cast<uint8_t>(lt::UiOrderLifecycleState::REJECTED));
}

TEST_CASE("Book queue with SPSC semantics") {
    lt::SpscQueue<lt::UiBookUpdate> queue(4);  // small capacity

    // Fill queue
    for (int i = 0; i < 4; ++i) {
        lt::UiBookUpdate upd{};
        upd.asset_id = lt::AssetId("tok");
        upd.bbo.best_bid = i * 100;
        CHECK(queue.try_push(upd));
    }

    // Next push should fail (queue full)
    lt::UiBookUpdate overflow{};
    CHECK_FALSE(queue.try_push(overflow));

    // Pop one and verify order
    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->bbo.best_bid == 0);
    queue.pop();
}

TEST_CASE("Invalid commands don't produce events") {
    lt::SpscQueue<lt::SchedulerEvent> control_queue(16);

    const char* invalid[] = {
        "",
        "not json",
        R"({"foo":"bar"})",
        R"({"cmd":"unknown"})",
        R"({"cmd":"set_mode"})",  // missing mode
        R"({"cmd":"set_spread"})",  // missing ticks
    };

    for (const auto* json : invalid) {
        auto cmd = lt::parse_ui_command(json);
        CHECK_FALSE(cmd.valid);
    }

    CHECK(control_queue.empty());
}

TEST_CASE("Metrics snapshot fields") {
    lt::Metrics metrics;
    metrics.inc(lt::MetricId::WS_FRAMES_RECEIVED);
    metrics.inc(lt::MetricId::WS_FRAMES_RECEIVED);
    metrics.inc(lt::MetricId::PARSE_OK);

    CHECK(metrics.get(lt::MetricId::WS_FRAMES_RECEIVED) == 2);
    CHECK(metrics.get(lt::MetricId::PARSE_OK) == 1);
}

TEST_CASE("UI metric IDs are valid") {
    lt::Metrics metrics;
    metrics.inc(lt::MetricId::UI_SNAPSHOTS_SENT);
    metrics.inc(lt::MetricId::UI_SNAPSHOTS_DROPPED);
    metrics.inc(lt::MetricId::UI_COMMANDS_RECEIVED);
    metrics.inc(lt::MetricId::UI_COMMANDS_INVALID);
    metrics.inc(lt::MetricId::UI_WS_CONNECTED);
    metrics.inc(lt::MetricId::UI_WS_DISCONNECTED);
    metrics.inc(lt::MetricId::UI_BOOK_PUSHES);

    CHECK(metrics.get(lt::MetricId::UI_SNAPSHOTS_SENT) == 1);
    CHECK(metrics.get(lt::MetricId::UI_SNAPSHOTS_DROPPED) == 1);
    CHECK(metrics.get(lt::MetricId::UI_COMMANDS_RECEIVED) == 1);
    CHECK(metrics.get(lt::MetricId::UI_COMMANDS_INVALID) == 1);
    CHECK(metrics.get(lt::MetricId::UI_WS_CONNECTED) == 1);
    CHECK(metrics.get(lt::MetricId::UI_WS_DISCONNECTED) == 1);
    CHECK(metrics.get(lt::MetricId::UI_BOOK_PUSHES) == 1);

    // Verify metric names
    CHECK(std::string(lt::Metrics::metric_name(lt::MetricId::UI_SNAPSHOTS_SENT)) == "ui.snapshots_sent");
    CHECK(std::string(lt::Metrics::metric_name(lt::MetricId::UI_BOOK_PUSHES)) == "ui.book_pushes");
}

TEST_CASE("M6 audit: new UI metric IDs are valid") {
    lt::Metrics metrics;
    metrics.inc(lt::MetricId::UI_COMMANDS_DROPPED);
    metrics.inc(lt::MetricId::UI_BOOK_DROPS);
    metrics.inc(lt::MetricId::UI_STATE_DROPS);

    CHECK(metrics.get(lt::MetricId::UI_COMMANDS_DROPPED) == 1);
    CHECK(metrics.get(lt::MetricId::UI_BOOK_DROPS) == 1);
    CHECK(metrics.get(lt::MetricId::UI_STATE_DROPS) == 1);

    CHECK(std::string(lt::Metrics::metric_name(lt::MetricId::UI_COMMANDS_DROPPED)) == "ui.commands_dropped");
    CHECK(std::string(lt::Metrics::metric_name(lt::MetricId::UI_BOOK_DROPS)) == "ui.book_drops");
    CHECK(std::string(lt::Metrics::metric_name(lt::MetricId::UI_STATE_DROPS)) == "ui.state_drops");
}

}  // TEST_SUITE
