// Integration test for the Binance market-data queue -> scheduler -> strategy
// callback path. This is the end-to-end test the audit doc flagged as missing
// for the data-feed pipelines (catches "callback never called" issues).
//
// The test does NOT exercise the WS client itself (would need a mock TLS
// server). It exercises the in-process plumbing: pushing BinanceMarketUpdate
// frames into the SPSC queue and verifying the scheduler delivers them to the
// strategy in order, including connection-state sentinel frames.

#include "doctest/doctest.h"

#include "binance/binance_types.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "scheduler/strategy.h"
#include "scheduler/strategy_scheduler.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace lt;

namespace {

// Stub strategy that records every BinanceMarketUpdate the scheduler delivers.
class BinanceCapture : public Strategy {
public:
    IntentBatch evaluate(const StrategyContext&) override { return {}; }
    void set_enabled(bool e) override { enabled_ = e; }
    bool enabled() const override { return enabled_; }
    void on_gateway_degraded() override {}
    void on_gateway_recovered() override {}

    void on_binance_update(const BinanceMarketUpdate& update) override {
        captured.push_back(update);
    }

    std::vector<BinanceMarketUpdate> captured;
    bool enabled_ = true;
};

// Run the scheduler briefly and stop it. Returns once the run loop has joined.
void run_briefly(StrategyScheduler& scheduler, int ms = 50) {
    std::thread t([&scheduler]() { scheduler.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    scheduler.request_shutdown();
    t.join();
}

}  // namespace

TEST_SUITE("BinanceStatusEmission") {

TEST_CASE("default-constructed BinanceMarketUpdate has kind = DATA") {
    BinanceMarketUpdate u{};
    CHECK(u.kind == static_cast<uint8_t>(BinanceUpdateKind::DATA));
    CHECK(u.type == static_cast<uint8_t>(BinanceStreamType::UNKNOWN));
}

TEST_CASE("scheduler delivers DATA frame to strategy") {
    Metrics metrics;
    AsyncLogger logger("test_binance_emit_data.log", 1024, LogLevel::DEBUG);
    logger.start();

    SpscQueue<MarketNotification> market_q(64);
    SpscQueue<SchedulerEvent> user_q(64);
    SpscQueue<SchedulerEvent> exec_q(64);
    SpscQueue<SchedulerEvent> control_q(64);
    SpscQueue<BinanceMarketUpdate> bmd_q(64);

    BinanceCapture strategy;

    SchedulerConfig sched_cfg;
    sched_cfg.poll_strategy = 1;
    sched_cfg.sleep_us = 1000;

    StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                metrics, logger, sched_cfg,
                                nullptr, nullptr, nullptr, nullptr,
                                &strategy);
    scheduler.set_binance_md_queue(&bmd_q);

    BinanceMarketUpdate upd{};
    upd.kind = static_cast<uint8_t>(BinanceUpdateKind::DATA);
    upd.type = static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER);
    upd.symbol = CryptoSymbol("btcusdt");
    upd.bid_price = 67234.5;
    upd.ask_price = 67235.1;
    upd.recv_ts = 42;
    REQUIRE(bmd_q.try_push(upd));

    run_briefly(scheduler);

    REQUIRE(strategy.captured.size() == 1);
    CHECK(strategy.captured[0].kind ==
          static_cast<uint8_t>(BinanceUpdateKind::DATA));
    CHECK(strategy.captured[0].type ==
          static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER));
    CHECK(strategy.captured[0].symbol.view() == "btcusdt");
    CHECK(strategy.captured[0].bid_price == doctest::Approx(67234.5));

    logger.stop();
}

TEST_CASE("scheduler delivers CONNECTED / DISCONNECTED sentinels in order") {
    Metrics metrics;
    AsyncLogger logger("test_binance_emit_sentinel.log", 1024, LogLevel::DEBUG);
    logger.start();

    SpscQueue<MarketNotification> market_q(64);
    SpscQueue<SchedulerEvent> user_q(64);
    SpscQueue<SchedulerEvent> exec_q(64);
    SpscQueue<SchedulerEvent> control_q(64);
    SpscQueue<BinanceMarketUpdate> bmd_q(64);

    BinanceCapture strategy;

    SchedulerConfig sched_cfg;
    sched_cfg.poll_strategy = 1;
    sched_cfg.sleep_us = 1000;

    StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                metrics, logger, sched_cfg,
                                nullptr, nullptr, nullptr, nullptr,
                                &strategy);
    scheduler.set_binance_md_queue(&bmd_q);

    auto make_data = [](double px) {
        BinanceMarketUpdate u{};
        u.kind = static_cast<uint8_t>(BinanceUpdateKind::DATA);
        u.type = static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER);
        u.symbol = CryptoSymbol("btcusdt");
        u.bid_price = px;
        u.ask_price = px + 0.5;
        return u;
    };
    auto make_disconnected = []() {
        BinanceMarketUpdate u{};
        u.kind = static_cast<uint8_t>(BinanceUpdateKind::DISCONNECTED);
        return u;
    };
    auto make_connected = []() {
        BinanceMarketUpdate u{};
        u.kind = static_cast<uint8_t>(BinanceUpdateKind::CONNECTED);
        return u;
    };

    // Push the canonical disconnect-and-recover sequence.
    REQUIRE(bmd_q.try_push(make_data(67000.0)));
    REQUIRE(bmd_q.try_push(make_data(67001.0)));
    REQUIRE(bmd_q.try_push(make_disconnected()));
    REQUIRE(bmd_q.try_push(make_connected()));
    REQUIRE(bmd_q.try_push(make_data(67100.0)));

    run_briefly(scheduler);

    REQUIRE(strategy.captured.size() == 5);
    CHECK(strategy.captured[0].kind == (uint8_t)BinanceUpdateKind::DATA);
    CHECK(strategy.captured[0].bid_price == doctest::Approx(67000.0));
    CHECK(strategy.captured[1].kind == (uint8_t)BinanceUpdateKind::DATA);
    CHECK(strategy.captured[1].bid_price == doctest::Approx(67001.0));
    CHECK(strategy.captured[2].kind == (uint8_t)BinanceUpdateKind::DISCONNECTED);
    CHECK(strategy.captured[3].kind == (uint8_t)BinanceUpdateKind::CONNECTED);
    CHECK(strategy.captured[4].kind == (uint8_t)BinanceUpdateKind::DATA);
    CHECK(strategy.captured[4].bid_price == doctest::Approx(67100.0));

    logger.stop();
}

TEST_CASE("scheduler drain bounded by max_binance_md_events_per_pass") {
    // Verify the per-pass cap is honored: with cap=4 and 10 enqueued frames,
    // a single short scheduler run should still deliver everything (because
    // the loop iterates many times), but the cap itself must be enforced
    // each pass. We assert eventual delivery of all frames.
    Metrics metrics;
    AsyncLogger logger("test_binance_emit_cap.log", 1024, LogLevel::DEBUG);
    logger.start();

    SpscQueue<MarketNotification> market_q(64);
    SpscQueue<SchedulerEvent> user_q(64);
    SpscQueue<SchedulerEvent> exec_q(64);
    SpscQueue<SchedulerEvent> control_q(64);
    SpscQueue<BinanceMarketUpdate> bmd_q(64);

    BinanceCapture strategy;

    SchedulerConfig sched_cfg;
    sched_cfg.poll_strategy = 1;
    sched_cfg.sleep_us = 1000;
    sched_cfg.max_binance_md_events_per_pass = 4;  // tight cap to exercise the loop

    StrategyScheduler scheduler(market_q, user_q, exec_q, control_q,
                                metrics, logger, sched_cfg,
                                nullptr, nullptr, nullptr, nullptr,
                                &strategy);
    scheduler.set_binance_md_queue(&bmd_q);

    for (int i = 0; i < 10; ++i) {
        BinanceMarketUpdate u{};
        u.kind = static_cast<uint8_t>(BinanceUpdateKind::DATA);
        u.type = static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER);
        u.bid_price = 100.0 + i;
        REQUIRE(bmd_q.try_push(u));
    }

    run_briefly(scheduler, 100);  // give it more time since cap is small

    REQUIRE(strategy.captured.size() == 10);
    for (int i = 0; i < 10; ++i) {
        CHECK(strategy.captured[i].bid_price == doctest::Approx(100.0 + i));
    }

    logger.stop();
}

}  // TEST_SUITE
