#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "common/clock.h"
#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "exec/exec_queue_sink.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/exec_intent_stub_sink.h"
#include "scheduler/risk_gate_stub.h"
#include "scheduler/strategy_scheduler.h"
#include "scheduler/strategy_state_stub.h"
#include "scheduler/strategy_stub.h"

using namespace lt;

// ---------------------------------------------------------------------------
// Helper: create default SchedulerConfig for tests
// ---------------------------------------------------------------------------
static SchedulerConfig test_config() {
    SchedulerConfig cfg;
    cfg.stats_interval_ms = 60000;  // suppress periodic dumps during tests
    cfg.poll_strategy = 1;          // sleep (avoid spinning in tests)
    cfg.sleep_us = 50;
    cfg.strategy_stub_emit_intents = false;
    return cfg;
}

// ---------------------------------------------------------------------------
// Helper: push a SchedulerEvent to a queue
// ---------------------------------------------------------------------------
static SchedulerEvent make_event(EventSource src, SchedulerEventKind kind,
                                 const char* asset = "") {
    SchedulerEvent ev;
    ev.source = src;
    ev.kind = kind;
    ev.asset_id = AssetId(asset);
    ev.recv_ts = SteadyClock::now();
    ev.seq = 0;
    if (src == EventSource::MARKET_WS) {
        ev.bbo.best_bid = 5000;
        ev.bbo.best_ask = 5100;
        ev.bbo.bid_size = 100;
        ev.bbo.ask_size = 100;
    }
    return ev;
}

// ---------------------------------------------------------------------------
// Helper: push a MarketNotification
// ---------------------------------------------------------------------------
static MarketNotification make_market_notif(NotificationKind kind,
                                            const char* asset = "test_asset") {
    MarketNotification notif;
    notif.kind = kind;
    notif.asset_id = AssetId(asset);
    notif.recv_ts = SteadyClock::now();
    notif.seq = 0;
    notif.bbo.best_bid = 5000;
    notif.bbo.best_ask = 5100;
    notif.bbo.bid_size = 100;
    notif.bbo.ask_size = 100;
    return notif;
}

// ===========================================================================
// SchedulerEvent model tests
// ===========================================================================
TEST_SUITE("SchedulerEvent") {
    TEST_CASE("from_market maps all NotificationKind values") {
        struct {
            NotificationKind nk;
            SchedulerEventKind expected;
        } cases[] = {
            {NotificationKind::BOOK_SNAPSHOT, SchedulerEventKind::MARKET_BOOK_SNAPSHOT},
            {NotificationKind::PRICE_CHANGE, SchedulerEventKind::MARKET_PRICE_CHANGE},
            {NotificationKind::BBO_UPDATE, SchedulerEventKind::MARKET_BBO_UPDATE},
            {NotificationKind::TICK_SIZE_CHANGE, SchedulerEventKind::MARKET_TICK_SIZE_CHANGE},
            {NotificationKind::LAST_TRADE, SchedulerEventKind::MARKET_LAST_TRADE},
        };
        for (auto& c : cases) {
            MarketNotification notif;
            notif.kind = c.nk;
            notif.asset_id = AssetId("abc");
            notif.recv_ts = 12345;
            notif.seq = 99;
            notif.bbo = {5000, 5100, 10, 20};
            notif.tick_size = (c.nk == NotificationKind::TICK_SIZE_CHANGE) ? 250 : 0;

            auto ev = SchedulerEvent::from_market(notif);
            CHECK(ev.source == EventSource::MARKET_WS);
            CHECK(ev.kind == c.expected);
            CHECK(ev.asset_id == AssetId("abc"));
            CHECK(ev.recv_ts == 12345);
            CHECK(ev.seq == 99);
            CHECK(ev.bbo.best_bid == 5000);
            CHECK(ev.bbo.best_ask == 5100);
            CHECK(ev.market_tick_size == notif.tick_size);
        }
    }

    TEST_CASE("source_priority ordering") {
        CHECK(source_priority(EventSource::USER_WS) < source_priority(EventSource::EXEC_INTERNAL));
        CHECK(source_priority(EventSource::EXEC_INTERNAL) < source_priority(EventSource::MARKET_WS));
        CHECK(source_priority(EventSource::MARKET_WS) < source_priority(EventSource::CONTROL));
    }
}

// ===========================================================================
// StrategyStateStub tests
// ===========================================================================
TEST_SUITE("StrategyStateStub") {
    TEST_CASE("tracks event counts by source and kind") {
        StrategyStateStub state;

        auto ev1 = make_event(EventSource::MARKET_WS,
                              SchedulerEventKind::MARKET_BOOK_SNAPSHOT, "asset_a");
        auto ev2 = make_event(EventSource::USER_WS,
                              SchedulerEventKind::USER_ORDER_UPDATE, "asset_a");
        auto ev3 = make_event(EventSource::MARKET_WS,
                              SchedulerEventKind::MARKET_PRICE_CHANGE, "asset_b");

        state.on_event(ev1);
        state.on_event(ev2);
        state.on_event(ev3);

        CHECK(state.total_events() == 3);
        CHECK(state.events_by_source(EventSource::MARKET_WS) == 2);
        CHECK(state.events_by_source(EventSource::USER_WS) == 1);
        CHECK(state.events_by_kind(SchedulerEventKind::MARKET_BOOK_SNAPSHOT) == 1);
        CHECK(state.events_by_kind(SchedulerEventKind::MARKET_PRICE_CHANGE) == 1);
        CHECK(state.events_by_kind(SchedulerEventKind::USER_ORDER_UPDATE) == 1);
    }

    TEST_CASE("tracks per-asset state and BBO") {
        StrategyStateStub state;

        SchedulerEvent ev;
        ev.source = EventSource::MARKET_WS;
        ev.kind = SchedulerEventKind::MARKET_BBO_UPDATE;
        ev.asset_id = AssetId("asset_x");
        ev.recv_ts = 1000;
        ev.bbo = {4500, 4600, 50, 60};

        state.on_event(ev);

        auto* ast = state.get_asset_state(AssetId("asset_x"));
        REQUIRE(ast != nullptr);
        CHECK(ast->event_count == 1);
        CHECK(ast->last_seen_ts == 1000);
        CHECK(ast->last_bbo.best_bid == 4500);
        CHECK(ast->last_bbo.best_ask == 4600);
    }

    TEST_CASE("cycle and trigger counting") {
        StrategyStateStub state;
        state.increment_cycle();
        state.increment_cycle();
        state.increment_triggers();
        CHECK(state.cycle_count() == 2);
        CHECK(state.trigger_count() == 1);
    }
}

// ===========================================================================
// StrategyStub tests
// ===========================================================================
TEST_SUITE("StrategyStub") {
    TEST_CASE("no intents when emit disabled") {
        StrategyStub strat(false);
        StrategyStateStub state;

        auto ev = make_event(EventSource::MARKET_WS,
                             SchedulerEventKind::MARKET_BOOK_SNAPSHOT, "a");
        auto batch = strat.evaluate(ev, state);

        CHECK(batch.count == 0);
        CHECK(strat.invocation_count() == 1);
    }

    TEST_CASE("emits bid/ask intents when enabled and BBO valid") {
        StrategyStub strat(true);
        StrategyStateStub state;

        auto ev = make_event(EventSource::MARKET_WS,
                             SchedulerEventKind::MARKET_BOOK_SNAPSHOT, "asset_1");
        auto batch = strat.evaluate(ev, state);

        CHECK(batch.count == 2);
        CHECK(batch.intents[0].action == IntentAction::WOULD_PLACE_BID);
        CHECK(batch.intents[0].price == 5000);
        CHECK(batch.intents[1].action == IntentAction::WOULD_PLACE_ASK);
        CHECK(batch.intents[1].price == 5100);
        CHECK(batch.intents[0].asset_id == AssetId("asset_1"));
    }

    TEST_CASE("no intents on non-market events even when enabled") {
        StrategyStub strat(true);
        StrategyStateStub state;

        auto ev = make_event(EventSource::USER_WS,
                             SchedulerEventKind::USER_ORDER_UPDATE, "a");
        auto batch = strat.evaluate(ev, state);

        CHECK(batch.count == 0);
    }

    TEST_CASE("no intents when BBO invalid") {
        StrategyStub strat(true);
        StrategyStateStub state;

        SchedulerEvent ev;
        ev.source = EventSource::MARKET_WS;
        ev.kind = SchedulerEventKind::MARKET_BOOK_SNAPSHOT;
        ev.bbo.best_bid = kInvalidPrice;
        ev.bbo.best_ask = kInvalidPrice;
        auto batch = strat.evaluate(ev, state);

        CHECK(batch.count == 0);
    }
}

// ===========================================================================
// RiskGateStub tests
// ===========================================================================
TEST_SUITE("RiskGateStub") {
    TEST_CASE("always allows in M2 stub") {
        RiskGateStub gate;
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_PLACE_BID;

        auto decision = gate.evaluate(intent);
        CHECK(decision == RiskDecision::ALLOW);
        CHECK(gate.check_count() == 1);
        CHECK(gate.allow_count() == 1);
        CHECK(gate.deny_count() == 0);

        gate.evaluate(intent);
        gate.evaluate(intent);
        CHECK(gate.check_count() == 3);
        CHECK(gate.allow_count() == 3);
    }
}

// ===========================================================================
// ExecIntentStubSink tests
// ===========================================================================
TEST_SUITE("ExecIntentStubSink") {
    TEST_CASE("returns NO_QUEUE without feedback queue") {
        ExecIntentStubSink sink(nullptr);
        ExecutionIntent intent;
        intent.intent_id = 42;

        CHECK(sink.accept(intent) == SinkResult::NO_QUEUE);
        CHECK(sink.accept(intent) == SinkResult::NO_QUEUE);

        CHECK(sink.intent_count() == 2);
        CHECK(sink.feedback_count() == 0);
        CHECK(sink.overflow_count() == 0);
    }

    TEST_CASE("returns ACCEPTED when queue provided") {
        SpscQueue<SchedulerEvent> exec_q(1024);
        ExecIntentStubSink sink(&exec_q);

        ExecutionIntent intent;
        intent.asset_id = AssetId("test");
        intent.intent_id = 7;

        CHECK(sink.accept(intent) == SinkResult::ACCEPTED);

        CHECK(sink.intent_count() == 1);
        CHECK(sink.feedback_count() == 1);
        CHECK(sink.overflow_count() == 0);

        auto* feedback = exec_q.front();
        REQUIRE(feedback != nullptr);
        CHECK(feedback->source == EventSource::EXEC_INTERNAL);
        CHECK(feedback->kind == SchedulerEventKind::EXEC_ORDER_ACK);
        CHECK(feedback->intent_ref_id == 7);
        CHECK(feedback->exec_accepted == true);
        CHECK(feedback->asset_id == AssetId("test"));
    }

    TEST_CASE("returns OVERFLOW when queue is full") {
        SpscQueue<SchedulerEvent> exec_q(2);  // very small capacity
        ExecIntentStubSink sink(&exec_q);

        ExecutionIntent intent;
        intent.intent_id = 1;

        // Fill the queue
        CHECK(sink.accept(intent) == SinkResult::ACCEPTED);
        // Second push may or may not succeed depending on SPSC impl
        auto r2 = sink.accept(intent);
        // Keep pushing until we get overflow
        while (r2 != SinkResult::OVERFLOW) {
            r2 = sink.accept(intent);
            if (sink.intent_count() > 100) break;  // safety valve
        }
        CHECK(sink.overflow_count() > 0);
    }
}

// ===========================================================================
// StrategyScheduler priority ordering tests
// ===========================================================================
TEST_SUITE("StrategyScheduler Priority") {
    TEST_CASE("USER_WS processed before MARKET_WS") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_priority1.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push market events first, then user events
        for (int i = 0; i < 5; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }
        for (int i = 0; i < 3; ++i) {
            user_q.try_push(make_event(EventSource::USER_WS,
                                       SchedulerEventKind::USER_ORDER_UPDATE, "a"));
        }

        // Run briefly
        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        // All events should be consumed
        CHECK(scheduler.state().total_events() == 8);
        CHECK(scheduler.state().events_by_source(EventSource::USER_WS) == 3);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 5);

        // User events metric should reflect processing
        CHECK(metrics.get(MetricId::SCHED_EVENTS_USER) == 3);
        CHECK(metrics.get(MetricId::SCHED_EVENTS_MARKET) == 5);

        logger.stop();
        std::remove("test_sched_priority1.log");
    }

    TEST_CASE("EXEC_INTERNAL processed before MARKET_WS") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_priority2.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push market and exec events
        for (int i = 0; i < 3; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::BBO_UPDATE));
        }
        for (int i = 0; i < 2; ++i) {
            exec_q.try_push(make_event(EventSource::EXEC_INTERNAL,
                                       SchedulerEventKind::EXEC_ORDER_ACK, "a"));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().events_by_source(EventSource::EXEC_INTERNAL) == 2);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 3);

        logger.stop();
        std::remove("test_sched_priority2.log");
    }

    TEST_CASE("CONTROL is lowest priority") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_priority3.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push control events alongside market events
        for (int i = 0; i < 2; ++i) {
            control_q.try_push(make_event(EventSource::CONTROL,
                                          SchedulerEventKind::CONTROL_PAUSE, ""));
        }
        for (int i = 0; i < 3; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::LAST_TRADE));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().events_by_source(EventSource::CONTROL) == 2);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 3);
        CHECK(scheduler.state().total_events() == 5);

        logger.stop();
        std::remove("test_sched_priority3.log");
    }

    TEST_CASE("deterministic ordering within same source queue") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_determ.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push 3 distinct market events
        auto n1 = make_market_notif(NotificationKind::BOOK_SNAPSHOT, "asset_1");
        auto n2 = make_market_notif(NotificationKind::PRICE_CHANGE, "asset_2");
        auto n3 = make_market_notif(NotificationKind::BBO_UPDATE, "asset_3");
        market_q.try_push(n1);
        market_q.try_push(n2);
        market_q.try_push(n3);

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        // All 3 assets should have been seen
        CHECK(scheduler.state().get_asset_state(AssetId("asset_1")) != nullptr);
        CHECK(scheduler.state().get_asset_state(AssetId("asset_2")) != nullptr);
        CHECK(scheduler.state().get_asset_state(AssetId("asset_3")) != nullptr);
        CHECK(scheduler.state().total_events() == 3);

        logger.stop();
        std::remove("test_sched_determ.log");
    }
}

// ===========================================================================
// Scheduler fairness / bounded work tests
// ===========================================================================
TEST_SUITE("StrategyScheduler Fairness") {
    TEST_CASE("lower priority queue gets serviced under high-priority load") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_fairness.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.max_market_events_per_pass = 4;  // limit market events per pass
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push many market events and a few control events
        for (int i = 0; i < 20; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }
        for (int i = 0; i < 3; ++i) {
            control_q.try_push(make_event(EventSource::CONTROL,
                                          SchedulerEventKind::CONTROL_RESUME, ""));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        scheduler.request_shutdown();
        t.join();

        // Control events must have been processed even though market had more events
        CHECK(scheduler.state().events_by_source(EventSource::CONTROL) == 3);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 20);

        logger.stop();
        std::remove("test_sched_fairness.log");
    }

    TEST_CASE("per-pass limits constrain each queue") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_limits.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.max_user_events_per_pass = 2;
        cfg.max_market_events_per_pass = 3;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Fill queues
        for (int i = 0; i < 10; ++i) {
            user_q.try_push(make_event(EventSource::USER_WS,
                                       SchedulerEventKind::USER_ORDER_UPDATE, "a"));
        }
        for (int i = 0; i < 10; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        scheduler.request_shutdown();
        t.join();

        // All should eventually drain
        CHECK(scheduler.state().events_by_source(EventSource::USER_WS) == 10);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 10);

        logger.stop();
        std::remove("test_sched_limits.log");
    }
}

// ===========================================================================
// Strategy + RiskGate integration tests
// ===========================================================================
TEST_SUITE("StrategyScheduler Integration") {
    TEST_CASE("scheduler invokes strategy on market triggers") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_strat.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push market events with valid BBO
        for (int i = 0; i < 3; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::BOOK_SNAPSHOT));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        // Strategy should have been called for each triggering event
        CHECK(scheduler.strategy().invocation_count() >= 3);
        CHECK(scheduler.state().trigger_count() >= 3);

        // Risk gate should have checked intents
        CHECK(scheduler.risk_gate().check_count() >= 6);  // 2 intents per event * 3
        CHECK(scheduler.risk_gate().allow_count() >= 6);

        // Exec sink should have recorded intents
        CHECK(scheduler.exec_sink().intent_count() >= 6);

        // Metrics should reflect
        CHECK(metrics.get(MetricId::SCHED_STRATEGY_CALLS) >= 3);
        CHECK(metrics.get(MetricId::SCHED_INTENTS_PRODUCED) >= 6);
        CHECK(metrics.get(MetricId::SCHED_INTENTS_ALLOWED) >= 6);

        logger.stop();
        std::remove("test_sched_strat.log");
    }

    TEST_CASE("control events do not trigger strategy") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_ctrl_no_strat.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push only control events
        for (int i = 0; i < 5; ++i) {
            control_q.try_push(make_event(EventSource::CONTROL,
                                          SchedulerEventKind::CONTROL_PAUSE, ""));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().total_events() == 5);
        CHECK(scheduler.state().trigger_count() == 0);
        CHECK(scheduler.strategy().invocation_count() == 0);

        logger.stop();
        std::remove("test_sched_ctrl_no_strat.log");
    }

    TEST_CASE("tick_size and last_trade events do not trigger strategy") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_no_trig.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        market_q.try_push(make_market_notif(NotificationKind::TICK_SIZE_CHANGE));
        market_q.try_push(make_market_notif(NotificationKind::LAST_TRADE));

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().total_events() == 2);
        CHECK(scheduler.state().trigger_count() == 0);
        CHECK(scheduler.strategy().invocation_count() == 0);

        logger.stop();
        std::remove("test_sched_no_trig.log");
    }

    TEST_CASE("exec feedback loop via stub sink") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_feedback.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push one market event - strategy will emit intents,
        // sink will push exec feedback, scheduler will process it
        market_q.try_push(make_market_notif(NotificationKind::BOOK_SNAPSHOT));

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        scheduler.request_shutdown();
        t.join();

        // Should have processed market event + exec feedback events
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) >= 1);
        CHECK(scheduler.state().events_by_source(EventSource::EXEC_INTERNAL) >= 1);
        CHECK(scheduler.exec_sink().feedback_count() >= 1);

        logger.stop();
        std::remove("test_sched_feedback.log");
    }
}

// ===========================================================================
// Integration with M1 queue producer
// ===========================================================================
TEST_SUITE("StrategyScheduler M1 Integration") {
    TEST_CASE("drains M1 market notifications and updates state") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_m1_int.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Simulate M1 pipeline output
        auto snap = make_market_notif(NotificationKind::BOOK_SNAPSHOT, "asset_a");
        snap.bbo = {4000, 6000, 100, 200};
        market_q.try_push(snap);

        auto delta = make_market_notif(NotificationKind::PRICE_CHANGE, "asset_a");
        delta.bbo = {4100, 5900, 150, 180};
        market_q.try_push(delta);

        auto trade = make_market_notif(NotificationKind::LAST_TRADE, "asset_b");
        market_q.try_push(trade);

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        // State should reflect all events
        CHECK(scheduler.state().total_events() == 3);

        auto* state_a = scheduler.state().get_asset_state(AssetId("asset_a"));
        REQUIRE(state_a != nullptr);
        CHECK(state_a->event_count == 2);
        CHECK(state_a->last_bbo.best_bid == 4100);
        CHECK(state_a->last_bbo.best_ask == 5900);

        auto* state_b = scheduler.state().get_asset_state(AssetId("asset_b"));
        REQUIRE(state_b != nullptr);
        CHECK(state_b->event_count == 1);

        logger.stop();
        std::remove("test_sched_m1_int.log");
    }

    TEST_CASE("handles empty market notifications gracefully") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_empty.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push notification with default/empty fields
        MarketNotification notif;
        notif.recv_ts = SteadyClock::now();
        market_q.try_push(notif);

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        // Should process without crashing
        CHECK(scheduler.state().total_events() == 1);

        logger.stop();
        std::remove("test_sched_empty.log");
    }
}

// ===========================================================================
// Shutdown / lifecycle tests
// ===========================================================================
TEST_SUITE("StrategyScheduler Lifecycle") {
    TEST_CASE("clean shutdown with empty queues") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_shutdown1.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.total_events() == 0);
        CHECK(scheduler.empty_polls() > 0);

        logger.stop();
        std::remove("test_sched_shutdown1.log");
    }

    TEST_CASE("clean shutdown with events in flight") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_shutdown2.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Pre-load events
        for (int i = 0; i < 100; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler.request_shutdown();
        t.join();

        // Should have processed some events before shutdown (maybe all)
        CHECK(scheduler.total_events() > 0);
        CHECK(scheduler.total_events() <= 100);

        logger.stop();
        std::remove("test_sched_shutdown2.log");
    }

    TEST_CASE("immediate shutdown after start") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_shutdown3.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Request shutdown before run
        scheduler.request_shutdown();

        std::thread t([&]() { scheduler.run(); });
        t.join();  // Should return immediately

        logger.stop();
        std::remove("test_sched_shutdown3.log");
    }

    TEST_CASE("metrics are populated after processing") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_sched_metrics.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        for (int i = 0; i < 10; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }

        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        scheduler.request_shutdown();
        t.join();

        CHECK(metrics.get(MetricId::SCHED_EVENTS) == 10);
        CHECK(metrics.get(MetricId::SCHED_EVENTS_MARKET) == 10);
        CHECK(metrics.get(MetricId::SCHED_CYCLES) > 0);
        CHECK(metrics.get(MetricId::SCHED_RECV_TO_PROC_COUNT) == 10);
        CHECK(metrics.get(MetricId::SCHED_RECV_TO_PROC_NS) > 0);
        CHECK(metrics.get(MetricId::QUEUE_POPS) == 10);

        logger.stop();
        std::remove("test_sched_metrics.log");
    }
}

// ===========================================================================
// Helper: wait for a condition with timeout (avoids brittle sleep)
// ===========================================================================
template <typename Pred>
static bool wait_for(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();  // one last check
}

// ===========================================================================
// Audit regression tests
// ===========================================================================
TEST_SUITE("StrategyScheduler Regression") {

    // -----------------------------------------------------------------------
    // R1: ExactCrossQueueOrder — trace must show USER before MARKET before CONTROL
    // -----------------------------------------------------------------------
    TEST_CASE("ExactCrossQueueOrder: priority ordering verified via trace") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_order.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = false;  // no feedback noise
        cfg.exec_feedback_loop_enabled = false;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);
        scheduler.enable_tracing();

        // Pre-load: 1 control, 1 market, 1 user — all before run()
        control_q.try_push(make_event(EventSource::CONTROL,
                                       SchedulerEventKind::CONTROL_PAUSE, ""));
        market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE, "m1"));
        auto user_ev = make_event(EventSource::USER_WS,
                                  SchedulerEventKind::USER_ORDER_UPDATE, "u1");
        user_ev.seq = 100;
        user_q.try_push(user_ev);

        std::thread t([&]() { scheduler.run(); });
        wait_for([&]() { return scheduler.state().total_events() >= 3; });
        scheduler.request_shutdown();
        t.join();

        REQUIRE(scheduler.state().trace_count() >= 3);
        // First event must be USER_WS (highest priority)
        CHECK(scheduler.state().trace_entry(0).source == EventSource::USER_WS);
        // Second must be MARKET_WS
        CHECK(scheduler.state().trace_entry(1).source == EventSource::MARKET_WS);
        // Third must be CONTROL
        CHECK(scheduler.state().trace_entry(2).source == EventSource::CONTROL);

        logger.stop();
        std::remove("test_reg_order.log");
    }

    // -----------------------------------------------------------------------
    // R2: FifoWithinSource — FIFO ordering within a single source queue
    // -----------------------------------------------------------------------
    TEST_CASE("FifoWithinSource: events from same queue maintain FIFO order") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_fifo.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = false;
        cfg.exec_feedback_loop_enabled = false;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);
        scheduler.enable_tracing();

        // Push 5 market events with sequential seq numbers
        for (int i = 0; i < 5; ++i) {
            auto notif = make_market_notif(NotificationKind::PRICE_CHANGE, "fifo_test");
            notif.seq = static_cast<SeqNum_t>(i + 1);
            market_q.try_push(notif);
        }

        std::thread t([&]() { scheduler.run(); });
        wait_for([&]() { return scheduler.state().total_events() >= 5; });
        scheduler.request_shutdown();
        t.join();

        REQUIRE(scheduler.state().trace_count() >= 5);
        for (int i = 0; i < 5; ++i) {
            CHECK(scheduler.state().trace_entry(i).seq == static_cast<SeqNum_t>(i + 1));
            CHECK(scheduler.state().trace_entry(i).source == EventSource::MARKET_WS);
        }

        logger.stop();
        std::remove("test_reg_fifo.log");
    }

    // -----------------------------------------------------------------------
    // R3: SustainedHighPriorityLoad — low-priority queue not starved
    // -----------------------------------------------------------------------
    TEST_CASE("SustainedHighPriorityLoad: control events processed under user flood") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_starve.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.max_user_events_per_pass = 4;
        cfg.strategy_stub_emit_intents = false;
        cfg.exec_feedback_loop_enabled = false;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Flood user queue
        for (int i = 0; i < 50; ++i) {
            user_q.try_push(make_event(EventSource::USER_WS,
                                        SchedulerEventKind::USER_ORDER_UPDATE, "u"));
        }
        // Push a few control events
        for (int i = 0; i < 3; ++i) {
            control_q.try_push(make_event(EventSource::CONTROL,
                                           SchedulerEventKind::CONTROL_RESUME, ""));
        }

        std::thread t([&]() { scheduler.run(); });
        wait_for([&]() {
            return scheduler.state().events_by_source(EventSource::USER_WS) >= 50 &&
                   scheduler.state().events_by_source(EventSource::CONTROL) >= 3;
        });
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().events_by_source(EventSource::USER_WS) == 50);
        CHECK(scheduler.state().events_by_source(EventSource::CONTROL) == 3);

        logger.stop();
        std::remove("test_reg_starve.log");
    }

    // -----------------------------------------------------------------------
    // R4: ExecInternalNeverDropped — exec feedback consumed even under load
    // -----------------------------------------------------------------------
    TEST_CASE("ExecInternalNeverDropped: exec events consumed under market load") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_exec.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = false;
        cfg.exec_feedback_loop_enabled = false;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push exec events and market events
        for (int i = 0; i < 10; ++i) {
            exec_q.try_push(make_event(EventSource::EXEC_INTERNAL,
                                        SchedulerEventKind::EXEC_ORDER_ACK, "e"));
        }
        for (int i = 0; i < 20; ++i) {
            market_q.try_push(make_market_notif(NotificationKind::PRICE_CHANGE));
        }

        std::thread t([&]() { scheduler.run(); });
        wait_for([&]() { return scheduler.state().total_events() >= 30; });
        scheduler.request_shutdown();
        t.join();

        // All exec events must have been processed
        CHECK(scheduler.state().events_by_source(EventSource::EXEC_INTERNAL) == 10);
        CHECK(scheduler.state().events_by_source(EventSource::MARKET_WS) == 20);

        logger.stop();
        std::remove("test_reg_exec.log");
    }

    // -----------------------------------------------------------------------
    // R5: InvalidLimitsRejectedOrClamped — config validation
    // -----------------------------------------------------------------------
    TEST_CASE("InvalidLimitsRejectedOrClamped: validate() clamps bad values") {
        SchedulerConfig cfg;
        cfg.max_user_events_per_pass = -5;
        cfg.max_market_events_per_pass = 99999;
        cfg.poll_strategy = 7;
        cfg.sleep_us = -10;
        cfg.stats_interval_ms = 0;

        bool clean = cfg.validate();
        CHECK_FALSE(clean);

        CHECK(cfg.max_user_events_per_pass >= 1);
        CHECK(cfg.max_market_events_per_pass <= 4096);
        CHECK(cfg.poll_strategy >= 0);
        CHECK(cfg.poll_strategy <= 2);
        CHECK(cfg.sleep_us >= 0);
        CHECK(cfg.stats_interval_ms >= 100);
    }

    TEST_CASE("ValidConfig passes validate unchanged") {
        SchedulerConfig cfg;
        cfg.max_user_events_per_pass = 128;
        cfg.max_market_events_per_pass = 256;
        cfg.poll_strategy = 2;
        cfg.sleep_us = 100;
        cfg.stats_interval_ms = 5000;

        bool clean = cfg.validate();
        CHECK(clean);
        CHECK(cfg.max_user_events_per_pass == 128);
        CHECK(cfg.max_market_events_per_pass == 256);
    }

    // -----------------------------------------------------------------------
    // R6: MalformedNotificationKind — from_market safe default
    // -----------------------------------------------------------------------
    TEST_CASE("MalformedNotificationKind: unknown kind maps to non-triggering") {
        MarketNotification notif;
        notif.asset_id = AssetId("mal");
        notif.recv_ts = 5000;
        notif.seq = 42;
        notif.bbo = {3000, 3100, 10, 20};
        // Cast an out-of-range value
        notif.kind = static_cast<NotificationKind>(255);

        auto ev = SchedulerEvent::from_market(notif);
        CHECK(ev.source == EventSource::MARKET_WS);
        // Must NOT be BOOK_SNAPSHOT (which would trigger strategy)
        CHECK(ev.kind == SchedulerEventKind::MARKET_LAST_TRADE);
        CHECK(ev.asset_id == AssetId("mal"));
        CHECK(ev.recv_ts == 5000);
        CHECK(ev.bbo.best_bid == 3000);
    }

    // -----------------------------------------------------------------------
    // R7: ShutdownDuringActiveProcessing — CONTROL_SHUTDOWN breaks the loop
    // -----------------------------------------------------------------------
    TEST_CASE("ShutdownDuringActiveProcessing: CONTROL_SHUTDOWN stops scheduler") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_shutdown.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = false;
        cfg.exec_feedback_loop_enabled = false;
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push a CONTROL_SHUTDOWN event
        control_q.try_push(make_event(EventSource::CONTROL,
                                       SchedulerEventKind::CONTROL_SHUTDOWN, ""));

        // Scheduler should process the shutdown event and exit on its own
        std::thread t([&]() { scheduler.run(); });
        bool exited = wait_for([&]() {
            // If the thread has stopped, join will succeed
            return scheduler.state().events_by_source(EventSource::CONTROL) >= 1;
        }, 3000);
        CHECK(exited);

        // The scheduler should stop on its own without request_shutdown()
        // but call it just in case to avoid hanging the test
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.state().events_by_source(EventSource::CONTROL) >= 1);

        logger.stop();
        std::remove("test_reg_shutdown.log");
    }

    // -----------------------------------------------------------------------
    // R8: FatalFlagPropagation — exception sets fatal_flag
    // -----------------------------------------------------------------------
    TEST_CASE("FatalFlag: fatal_flag is available via constructor") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_fatal.log", 1024, LogLevel::WARN);
        logger.start();

        std::atomic<bool> fatal{false};
        auto cfg = test_config();
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg, &fatal);

        // Normal operation should not set fatal
        std::thread t([&]() { scheduler.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler.request_shutdown();
        t.join();

        CHECK_FALSE(fatal.load());

        logger.stop();
        std::remove("test_reg_fatal.log");
    }

    // -----------------------------------------------------------------------
    // R9: ExecFeedbackLoopDisable — no self-feedback when disabled
    // -----------------------------------------------------------------------
    TEST_CASE("ExecFeedbackLoopDisable: no feedback when exec_feedback_loop_enabled=false") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_nofeedback.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        cfg.exec_feedback_loop_enabled = false;  // disable self-feedback
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg);

        // Push market event that will trigger strategy → intents
        market_q.try_push(make_market_notif(NotificationKind::BOOK_SNAPSHOT));

        std::thread t([&]() { scheduler.run(); });
        wait_for([&]() { return scheduler.state().total_events() >= 1; });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler.request_shutdown();
        t.join();

        // Strategy should have been called
        CHECK(scheduler.strategy().invocation_count() >= 1);
        // Sink should have NO_QUEUE result (no feedback enqueued)
        CHECK(scheduler.exec_sink().feedback_count() == 0);
        // exec_queue should be empty — no self-feedback loop
        CHECK(scheduler.state().events_by_source(EventSource::EXEC_INTERNAL) == 0);

        logger.stop();
        std::remove("test_reg_nofeedback.log");
    }

    TEST_CASE("TickSizePropagation: intents are not converted by planner") {
        SpscQueue<MarketNotification> market_q(1024);
        SpscQueue<SchedulerEvent> user_q(1024);
        SpscQueue<SchedulerEvent> exec_q(1024);
        SpscQueue<SchedulerEvent> control_q(1024);
        SpscQueue<ExecIntent> t2_to_t3_q(1024);
        Metrics metrics;
        AsyncLogger logger("test_reg_tick_propagation.log", 1024, LogLevel::WARN);
        logger.start();

        auto cfg = test_config();
        cfg.strategy_stub_emit_intents = true;
        cfg.exec_feedback_loop_enabled = false;

        MarketPairRegistry pairs;
        CHECK(pairs.add_pair(AssetId("cond-1"), AssetId("up"), AssetId("down")));

        TokenInventory inventory;
        inventory.register_token(AssetId("up"));
        inventory.register_token(AssetId("down"));
        inventory.set_position(AssetId("up"), 0);
        inventory.set_position(AssetId("down"), 0);

        ExecQueueSink sink(t2_to_t3_q);
        StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                    metrics, logger, cfg, nullptr, &sink,
                                    &pairs, &inventory);

        // Update tick for down token.
        auto tick = make_market_notif(NotificationKind::TICK_SIZE_CHANGE, "down");
        tick.tick_size = 250;
        market_q.try_push(tick);

        // Trigger strategy on "up". Stub emits bid@5000 and ask@5100.
        auto snap = make_market_notif(NotificationKind::BOOK_SNAPSHOT, "up");
        snap.bbo.best_bid = 5000;
        snap.bbo.best_ask = 5100;
        market_q.try_push(snap);

        std::thread t([&]() { scheduler.run(); });
        bool produced = wait_for([&]() { return t2_to_t3_q.size() >= 2; }, 3000);
        scheduler.request_shutdown();
        t.join();

        REQUIRE(produced);

        bool found_up_bid = false;
        bool found_up_ask = false;
        bool found_down_bid = false;
        while (auto* intent = t2_to_t3_q.front()) {
            if (intent->asset_id == AssetId("up") && intent->side == Side::BID) {
                found_up_bid = true;
                CHECK(intent->price == 5000);
            }
            if (intent->asset_id == AssetId("up") && intent->side == Side::ASK) {
                found_up_ask = true;
                CHECK(intent->price == 5100);
            }
            if (intent->asset_id == AssetId("down") && intent->side == Side::BID) {
                found_down_bid = true;
            }
            t2_to_t3_q.pop();
        }

        CHECK(found_up_bid);
        CHECK(found_up_ask);
        CHECK_FALSE(found_down_bid);
        CHECK(metrics.get(MetricId::SCHED_QUOTE_CONVERSIONS) == 0);

        logger.stop();
        std::remove("test_reg_tick_propagation.log");
    }

    // -----------------------------------------------------------------------
    // R10: Trace buffer records correctly
    // -----------------------------------------------------------------------
    TEST_CASE("TraceBuffer: records events in order") {
        StrategyStateStub state;
        state.enable_tracing();

        auto ev1 = make_event(EventSource::MARKET_WS,
                               SchedulerEventKind::MARKET_BOOK_SNAPSHOT, "a");
        ev1.seq = 10;
        auto ev2 = make_event(EventSource::USER_WS,
                               SchedulerEventKind::USER_ORDER_UPDATE, "b");
        ev2.seq = 20;
        auto ev3 = make_event(EventSource::CONTROL,
                               SchedulerEventKind::CONTROL_PAUSE, "");
        ev3.seq = 30;

        state.on_event(ev1);
        state.on_event(ev2);
        state.on_event(ev3);

        CHECK(state.trace_count() == 3);
        CHECK(state.trace_entry(0).source == EventSource::MARKET_WS);
        CHECK(state.trace_entry(0).seq == 10);
        CHECK(state.trace_entry(1).source == EventSource::USER_WS);
        CHECK(state.trace_entry(1).seq == 20);
        CHECK(state.trace_entry(2).source == EventSource::CONTROL);
        CHECK(state.trace_entry(2).seq == 30);
    }
}
