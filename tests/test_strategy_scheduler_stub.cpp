#include <doctest/doctest.h>

#include <thread>

#include "common/clock.h"
#include "events/event_variant.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/strategy_scheduler_stub.h"

using namespace lt;

TEST_SUITE("StrategySchedulerStub") {
    TEST_CASE("consumes events and counts them") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_scheduler.log", 1024, LogLevel::WARN);
        logger.start();

        StrategySchedulerStub scheduler(queue, metrics, logger, 60000);

        // Push some notifications
        for (int i = 0; i < 5; ++i) {
            MarketNotification notif;
            notif.kind = NotificationKind::BOOK_SNAPSHOT;
            notif.recv_ts = SteadyClock::now();
            notif.seq = static_cast<SeqNum_t>(i);
            queue.try_push(notif);
        }

        MarketNotification notif;
        notif.kind = NotificationKind::PRICE_CHANGE;
        notif.recv_ts = SteadyClock::now();
        queue.try_push(notif);

        // Run scheduler briefly
        std::thread t([&]() { scheduler.run(); });

        // Give it time to consume
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.total_events() == 6);
        CHECK(scheduler.events_by_kind(NotificationKind::BOOK_SNAPSHOT) == 5);
        CHECK(scheduler.events_by_kind(NotificationKind::PRICE_CHANGE) == 1);

        logger.stop();

        // Clean up test log
        std::remove("test_scheduler.log");
    }

    TEST_CASE("shutdown with empty queue") {
        SpscQueue<MarketNotification> queue(1024);
        Metrics metrics;
        AsyncLogger logger("test_scheduler2.log", 1024, LogLevel::WARN);
        logger.start();

        StrategySchedulerStub scheduler(queue, metrics, logger, 60000);

        std::thread t([&]() { scheduler.run(); });

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        scheduler.request_shutdown();
        t.join();

        CHECK(scheduler.total_events() == 0);

        logger.stop();
        std::remove("test_scheduler2.log");
    }
}
