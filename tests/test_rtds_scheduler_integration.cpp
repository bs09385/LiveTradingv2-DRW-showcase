#include "doctest/doctest.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "rtds/rtds_types.h"
#include "scheduler/strategy.h"
#include "scheduler/strategy_scheduler.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace lt;

namespace {

// Strategy that captures crypto price updates
class CryptoPriceCapture : public Strategy {
public:
    IntentBatch evaluate(const StrategyContext&) override { return {}; }
    void set_enabled(bool e) override { enabled_ = e; }
    bool enabled() const override { return enabled_; }
    void on_gateway_degraded() override {}
    void on_gateway_recovered() override {}
    void on_crypto_price(const CryptoPriceUpdate& update) override {
        captured.push_back(update);
    }

    std::vector<CryptoPriceUpdate> captured;
    bool enabled_ = true;
};

}  // namespace

TEST_SUITE("RtdsSchedulerIntegration") {

TEST_CASE("Scheduler drains RTDS queue and calls strategy") {
    Metrics metrics;
    AsyncLogger logger("test_rtds_sched.log", 1024, LogLevel::DEBUG);
    logger.start();

    SpscQueue<MarketNotification> market_q(64);
    SpscQueue<SchedulerEvent> user_q(64);
    SpscQueue<SchedulerEvent> exec_q(64);
    SpscQueue<SchedulerEvent> control_q(64);
    SpscQueue<CryptoPriceUpdate> rtds_q(64);

    CryptoPriceCapture strategy;

    SchedulerConfig sched_cfg;
    sched_cfg.poll_strategy = 1;  // sleep
    sched_cfg.sleep_us = 1000;

    StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                metrics, logger, sched_cfg,
                                nullptr, nullptr, nullptr, nullptr,
                                &strategy);
    scheduler.set_rtds_queue(&rtds_q);

    // Push some crypto prices
    CryptoPriceUpdate btc;
    btc.symbol = CryptoSymbol("btcusdt");
    btc.value = 67000.0;
    btc.exchange_ts_ms = 1000;
    btc.recv_ts = 100;
    rtds_q.try_push(btc);

    CryptoPriceUpdate eth;
    eth.symbol = CryptoSymbol("ethusdt");
    eth.value = 3500.0;
    eth.exchange_ts_ms = 1001;
    eth.recv_ts = 200;
    rtds_q.try_push(eth);

    // Run scheduler briefly
    std::thread t([&scheduler]() { scheduler.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler.request_shutdown();
    t.join();

    // Verify strategy received both prices
    REQUIRE(strategy.captured.size() == 2);
    CHECK(strategy.captured[0].symbol.view() == "btcusdt");
    CHECK(strategy.captured[0].value == doctest::Approx(67000.0));
    CHECK(strategy.captured[1].symbol.view() == "ethusdt");
    CHECK(strategy.captured[1].value == doctest::Approx(3500.0));

    logger.stop();
}

TEST_CASE("RTDS queue not wired - scheduler runs normally") {
    Metrics metrics;
    AsyncLogger logger("test_rtds_sched2.log", 1024, LogLevel::DEBUG);
    logger.start();

    SpscQueue<MarketNotification> market_q(64);
    SpscQueue<SchedulerEvent> user_q(64);
    SpscQueue<SchedulerEvent> exec_q(64);
    SpscQueue<SchedulerEvent> control_q(64);

    SchedulerConfig sched_cfg;
    sched_cfg.poll_strategy = 1;
    sched_cfg.sleep_us = 1000;

    // No strategy, no RTDS queue
    StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                metrics, logger, sched_cfg);

    std::thread t([&scheduler]() { scheduler.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    scheduler.request_shutdown();
    t.join();

    // Just verify it doesn't crash
    CHECK(true);

    logger.stop();
}

}  // TEST_SUITE
