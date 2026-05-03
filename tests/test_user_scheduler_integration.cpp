#include <doctest/doctest.h>

#include <thread>

#include "events/scheduler_events.h"
#include "events/user_events.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/strategy_scheduler.h"

using namespace lt;

namespace {

SchedulerEvent make_user_order_event(const char* aid, SeqNum_t seq) {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_ORDER_UPDATE;
    ev.asset_id = AssetId(aid);
    ev.recv_ts = 100000;
    ev.seq = seq;
    ev.order_id = OrderId("o1");
    ev.user_side = Side::BID;
    ev.user_price = 5500;
    return ev;
}

SchedulerEvent make_user_trade_event(const char* aid, SeqNum_t seq, bool is_fill) {
    SchedulerEvent ev;
    ev.source = EventSource::USER_WS;
    ev.kind = SchedulerEventKind::USER_TRADE_UPDATE;
    ev.asset_id = AssetId(aid);
    ev.recv_ts = 100000;
    ev.seq = seq;
    ev.trade_id = TradeId("t1");
    ev.user_side = Side::BID;
    ev.user_price = 5500;
    ev.user_fill_size = 50;
    ev.is_new_fill = is_fill;
    return ev;
}

SchedulerEvent make_market_event(const char* aid, SeqNum_t seq) {
    MarketNotification notif;
    notif.kind = NotificationKind::BBO_UPDATE;
    notif.asset_id = AssetId(aid);
    notif.recv_ts = 100000;
    notif.seq = seq;
    notif.bbo.best_bid = 5500;
    notif.bbo.best_ask = 5600;
    return SchedulerEvent::from_market(notif);
}

}  // namespace

TEST_SUITE("UserSchedulerIntegration") {
    TEST_CASE("user event processed before market event (priority)") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);
        scheduler.enable_tracing();

        // Push one market event and one user event
        MarketNotification notif;
        notif.kind = NotificationKind::BBO_UPDATE;
        notif.asset_id = AssetId("a1");
        notif.recv_ts = 100;
        notif.seq = 1;
        notif.bbo.best_bid = 5500;
        notif.bbo.best_ask = 5600;
        market_queue.try_push(notif);

        auto user_ev = make_user_order_event("a1", 2);
        user_queue.try_push(user_ev);

        // Push shutdown
        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        // User event should be processed first (highest priority)
        auto& state = scheduler.state();
        CHECK(state.trace_count() >= 2);
        CHECK(state.trace_entry(0).source == EventSource::USER_WS);
        CHECK(state.trace_entry(1).source == EventSource::MARKET_WS);
    }

    TEST_CASE("USER_ORDER_UPDATE kind recorded in StrategyStateStub") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        auto ev = make_user_order_event("a1", 1);
        user_queue.try_push(ev);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        CHECK(scheduler.state().events_by_kind(SchedulerEventKind::USER_ORDER_UPDATE) == 1);
    }

    TEST_CASE("USER_TRADE_UPDATE kind recorded in StrategyStateStub") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        auto ev = make_user_trade_event("a1", 1, true);
        user_queue.try_push(ev);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        CHECK(scheduler.state().events_by_kind(SchedulerEventKind::USER_TRADE_UPDATE) == 1);
    }

    TEST_CASE("multiple user events maintain FIFO order") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);
        scheduler.enable_tracing();

        // Push 3 user events with sequential seq numbers
        for (int i = 1; i <= 3; ++i) {
            auto ev = make_user_order_event("a1", static_cast<SeqNum_t>(i));
            user_queue.try_push(ev);
        }

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        auto& state = scheduler.state();
        CHECK(state.trace_count() >= 3);
        // All 3 user events should be FIFO
        CHECK(state.trace_entry(0).seq == 1);
        CHECK(state.trace_entry(1).seq == 2);
        CHECK(state.trace_entry(2).seq == 3);
    }

    TEST_CASE("SCHED_EVENTS_USER metric incremented") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        auto ev = make_user_order_event("a1", 1);
        user_queue.try_push(ev);
        auto ev2 = make_user_trade_event("a1", 2, false);
        user_queue.try_push(ev2);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        CHECK(metrics.get(MetricId::SCHED_EVENTS_USER) == 2);
    }

    TEST_CASE("user event with is_new_fill flag propagates") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);
        scheduler.enable_tracing();

        auto ev = make_user_trade_event("a1", 1, true);
        user_queue.try_push(ev);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        auto& state = scheduler.state();
        CHECK(state.events_by_source(EventSource::USER_WS) == 1);
        CHECK(state.trace_count() >= 1);
        CHECK(state.trace_entry(0).source == EventSource::USER_WS);
        CHECK(state.trace_entry(0).kind == SchedulerEventKind::USER_TRADE_UPDATE);
    }

    TEST_CASE("user events processed with market events in same pass") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        // Push 2 user + 3 market events
        for (int i = 0; i < 2; ++i) {
            auto ev = make_user_order_event("a1", static_cast<SeqNum_t>(i + 1));
            user_queue.try_push(ev);
        }
        for (int i = 0; i < 3; ++i) {
            MarketNotification notif;
            notif.kind = NotificationKind::PRICE_CHANGE;
            notif.asset_id = AssetId("a1");
            notif.recv_ts = 100;
            notif.seq = static_cast<SeqNum_t>(10 + i);
            market_queue.try_push(notif);
        }

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        CHECK(metrics.get(MetricId::SCHED_EVENTS_USER) == 2);
        CHECK(metrics.get(MetricId::SCHED_EVENTS_MARKET) == 3);
        CHECK(scheduler.state().total_events() == 6);  // 2 user + 3 market + 1 control
    }

    TEST_CASE("[audit2] USER_WS events increment trigger count and strategy calls") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        // Push 3 user events (order + trade + order)
        auto ev1 = make_user_order_event("a1", 1);
        user_queue.try_push(ev1);
        auto ev2 = make_user_trade_event("a1", 2, true);
        user_queue.try_push(ev2);
        auto ev3 = make_user_order_event("a1", 3);
        user_queue.try_push(ev3);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        // All 3 user events should trigger strategy evaluation
        CHECK(scheduler.state().trigger_count() == 3);
        CHECK(metrics.get(MetricId::SCHED_STRATEGY_CALLS) == 3);
    }

    TEST_CASE("user event updates per-asset state") {
        SpscQueue<MarketNotification> market_queue(1024);
        SpscQueue<SchedulerEvent> user_queue(1024);
        SpscQueue<SchedulerEvent> exec_queue(1024);
        SpscQueue<SchedulerEvent> control_queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_integration.log", 1024, LogLevel::ERROR);

        SchedulerConfig cfg;
        cfg.exec_feedback_loop_enabled = false;

        StrategyScheduler scheduler(market_queue, user_queue, exec_queue,
                                    control_queue, metrics, logger, cfg);

        auto ev = make_user_order_event("a1", 1);
        user_queue.try_push(ev);

        SchedulerEvent shutdown;
        shutdown.source = EventSource::CONTROL;
        shutdown.kind = SchedulerEventKind::CONTROL_SHUTDOWN;
        control_queue.try_push(shutdown);

        scheduler.run();

        auto* asset_state = scheduler.state().get_asset_state(AssetId("a1"));
        REQUIRE(asset_state != nullptr);
        CHECK(asset_state->event_count >= 1);
    }
}
