#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

#include "rotation/rotation_coordinator.h"
#include "common/config.h"
#include "common/discovery.h"
#include "ui_bridge/ui_serializer.h"

using namespace lt;

TEST_SUITE("Rotation") {

    // --- RotationTimingContext tests ---

    TEST_CASE("RotationTimingContext defaults") {
        RotationTimingContext ctx;
        CHECK(ctx.window_start_unix_s == 0);
        CHECK(ctx.window_end_unix_s == 0);
        CHECK(ctx.period_s == 300);
        CHECK(ctx.no_trade_start_ms == 0);
        CHECK(ctx.no_trade_end_ms == 0);
        CHECK(ctx.enabled == false);
    }

    TEST_CASE("Timing context no-trade zone detection") {
        RotationTimingContext ctx;
        ctx.window_start_unix_s = 1000;
        ctx.window_end_unix_s = 1300;  // 5 min window
        ctx.no_trade_start_ms = 30000;
        ctx.no_trade_end_ms = 30000;
        ctx.enabled = true;

        // Helper: check if a given time is in a no-trade zone
        auto in_no_trade = [&](int64_t now_s) -> bool {
            int64_t elapsed_ms = (now_s - ctx.window_start_unix_s) * 1000;
            int64_t remaining_ms = (ctx.window_end_unix_s - now_s) * 1000;
            return (elapsed_ms < ctx.no_trade_start_ms) ||
                   (remaining_ms < ctx.no_trade_end_ms);
        };

        // At window start: elapsed=0ms < 30000ms -> no-trade
        CHECK(in_no_trade(1000) == true);

        // 10s into window: elapsed=10000ms < 30000ms -> no-trade
        CHECK(in_no_trade(1010) == true);

        // 29s into window: elapsed=29000ms < 30000ms -> no-trade
        CHECK(in_no_trade(1029) == true);

        // 30s into window: elapsed=30000ms == 30000ms -> just outside
        CHECK(in_no_trade(1030) == false);

        // Middle of window: should be active trading
        CHECK(in_no_trade(1150) == false);

        // 31s before end: remaining=31000ms > 30000ms -> active
        CHECK(in_no_trade(1269) == false);

        // 30s before end: remaining=30000ms == 30000ms -> just outside
        CHECK(in_no_trade(1270) == false);

        // 29s before end: remaining=29000ms < 30000ms -> no-trade
        CHECK(in_no_trade(1271) == true);

        // At window end: remaining=0ms < 30000ms -> no-trade
        CHECK(in_no_trade(1300) == true);
    }

    TEST_CASE("No-trade with asymmetric zones") {
        RotationTimingContext ctx;
        ctx.window_start_unix_s = 0;
        ctx.window_end_unix_s = 300;
        ctx.no_trade_start_ms = 10000;  // 10s start
        ctx.no_trade_end_ms = 60000;    // 60s end
        ctx.enabled = true;

        auto in_no_trade = [&](int64_t now_s) -> bool {
            int64_t elapsed_ms = (now_s - ctx.window_start_unix_s) * 1000;
            int64_t remaining_ms = (ctx.window_end_unix_s - now_s) * 1000;
            return (elapsed_ms < ctx.no_trade_start_ms) ||
                   (remaining_ms < ctx.no_trade_end_ms);
        };

        CHECK(in_no_trade(5) == true);    // 5s in, < 10s start zone
        CHECK(in_no_trade(10) == false);   // exactly 10s, outside
        CHECK(in_no_trade(100) == false);  // mid window
        CHECK(in_no_trade(239) == false);  // 61s remaining, active
        CHECK(in_no_trade(240) == false);  // 60s remaining, just outside
        CHECK(in_no_trade(241) == true);   // 59s remaining, no-trade
    }

    TEST_CASE("No-trade disabled when context not enabled") {
        RotationTimingContext ctx;
        ctx.window_start_unix_s = 1000;
        ctx.window_end_unix_s = 1300;
        ctx.no_trade_start_ms = 30000;
        ctx.no_trade_end_ms = 30000;
        ctx.enabled = false;

        // Even at the start of window, should not trigger no-trade
        // (caller should check ctx.enabled first)
        CHECK(ctx.enabled == false);
    }

    // --- RotationPhase state machine tests ---

    TEST_CASE("RotationPhase values") {
        CHECK(static_cast<int>(RotationPhase::NORMAL) == 0);
        CHECK(static_cast<int>(RotationPhase::PAUSE_REQUESTED) == 1);
        CHECK(static_cast<int>(RotationPhase::PAUSED) == 2);
        CHECK(static_cast<int>(RotationPhase::RESUME_REQUESTED) == 3);
    }

    TEST_CASE("Phase state machine transitions via atomic") {
        std::atomic<int> phase{static_cast<int>(RotationPhase::NORMAL)};

        // NORMAL -> PAUSE_REQUESTED (T7 writes)
        phase.store(static_cast<int>(RotationPhase::PAUSE_REQUESTED),
                    std::memory_order_release);
        CHECK(phase.load(std::memory_order_acquire) ==
              static_cast<int>(RotationPhase::PAUSE_REQUESTED));

        // PAUSE_REQUESTED -> PAUSED (T2 writes)
        phase.store(static_cast<int>(RotationPhase::PAUSED),
                    std::memory_order_release);
        CHECK(phase.load(std::memory_order_acquire) ==
              static_cast<int>(RotationPhase::PAUSED));

        // PAUSED -> RESUME_REQUESTED (T7 writes)
        phase.store(static_cast<int>(RotationPhase::RESUME_REQUESTED),
                    std::memory_order_release);
        CHECK(phase.load(std::memory_order_acquire) ==
              static_cast<int>(RotationPhase::RESUME_REQUESTED));

        // RESUME_REQUESTED -> NORMAL (T2 writes)
        phase.store(static_cast<int>(RotationPhase::NORMAL),
                    std::memory_order_release);
        CHECK(phase.load(std::memory_order_acquire) ==
              static_cast<int>(RotationPhase::NORMAL));
    }

    TEST_CASE("Phase transitions between two threads") {
        std::atomic<int> phase{static_cast<int>(RotationPhase::NORMAL)};
        std::atomic<bool> done{false};
        bool t2_saw_pause = false;
        bool t2_saw_resume = false;

        // Simulate T2 role
        std::thread t2([&]() {
            // Wait for PAUSE_REQUESTED
            while (phase.load(std::memory_order_acquire) !=
                   static_cast<int>(RotationPhase::PAUSE_REQUESTED)) {
                if (done.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
            t2_saw_pause = true;

            // Ack pause
            phase.store(static_cast<int>(RotationPhase::PAUSED),
                        std::memory_order_release);

            // Wait for RESUME_REQUESTED
            while (phase.load(std::memory_order_acquire) !=
                   static_cast<int>(RotationPhase::RESUME_REQUESTED)) {
                if (done.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
            t2_saw_resume = true;

            // Ack resume
            phase.store(static_cast<int>(RotationPhase::NORMAL),
                        std::memory_order_release);
        });

        // Simulate T7 role
        // Request pause
        phase.store(static_cast<int>(RotationPhase::PAUSE_REQUESTED),
                    std::memory_order_release);

        // Wait for PAUSED
        while (phase.load(std::memory_order_acquire) !=
               static_cast<int>(RotationPhase::PAUSED)) {
            std::this_thread::yield();
        }

        // Do rotation work...

        // Request resume
        phase.store(static_cast<int>(RotationPhase::RESUME_REQUESTED),
                    std::memory_order_release);

        // Wait for NORMAL
        while (phase.load(std::memory_order_acquire) !=
               static_cast<int>(RotationPhase::NORMAL)) {
            std::this_thread::yield();
        }

        done.store(true, std::memory_order_relaxed);
        t2.join();

        CHECK(t2_saw_pause == true);
        CHECK(t2_saw_resume == true);
        CHECK(phase.load() == static_cast<int>(RotationPhase::NORMAL));
    }

    // --- Window boundary calculation tests ---

    TEST_CASE("Window boundary calculation for 5-minute windows") {
        int64_t period = 300;  // 5 minutes

        // window_start = (unix_s / period) * period
        // window_end = window_start + period

        auto window_start = [period](int64_t unix_s) {
            return (unix_s / period) * period;
        };
        auto window_end = [period](int64_t unix_s) {
            return (unix_s / period) * period + period;
        };

        // Exactly on a boundary
        CHECK(window_start(0) == 0);
        CHECK(window_end(0) == 300);

        CHECK(window_start(300) == 300);
        CHECK(window_end(300) == 600);

        // Middle of a window
        CHECK(window_start(150) == 0);
        CHECK(window_end(150) == 300);

        // Just before boundary
        CHECK(window_start(299) == 0);
        CHECK(window_end(299) == 300);

        // Just after boundary
        CHECK(window_start(301) == 300);
        CHECK(window_end(301) == 600);

        // Large timestamp (e.g. 2024-01-01 00:00:00 UTC = 1704067200)
        int64_t ts = 1704067200;
        CHECK(window_start(ts) == ts);  // should be exactly on boundary (divisible by 300)
        CHECK(window_end(ts) == ts + 300);

        CHECK(window_start(ts + 123) == ts);
        CHECK(window_end(ts + 123) == ts + 300);
    }

    TEST_CASE("Window boundary for 15-minute timeframe") {
        // 15-minute = 900s
        int64_t period = 900;
        auto ws = [period](int64_t s) { return (s / period) * period; };
        auto we = [period](int64_t s) { return (s / period) * period + period; };

        CHECK(ws(0) == 0);
        CHECK(we(0) == 900);
        CHECK(ws(450) == 0);
        CHECK(we(450) == 900);
        CHECK(ws(900) == 900);
        CHECK(we(900) == 1800);
    }

    // --- RotationConfig tests ---

    TEST_CASE("RotationConfig defaults") {
        RotationConfig cfg;
        CHECK(cfg.timeframe == BtcTimeframe::BTC_5M);
        CHECK(cfg.discovery_poll_ms == 15000);
        CHECK(cfg.pre_rotation_ms == 5000);
        CHECK(cfg.no_trade_start_ms == 0);
        CHECK(cfg.no_trade_end_ms == 0);
        CHECK(cfg.discovery_api_url == "https://gamma-api.polymarket.com");
    }

    // --- RotationCallbacks wiring tests ---

    TEST_CASE("RotationCallbacks all nullable") {
        RotationCallbacks cb;
        // All functions should be empty (nullptr)
        CHECK(!cb.stop_market_ws);
        CHECK(!cb.stop_user_ws);
        CHECK(!cb.start_market_ws);
        CHECK(!cb.start_user_ws);
        CHECK(!cb.register_market);
        CHECK(!cb.register_token);
        CHECK(!cb.seed_market_state);
        CHECK(!cb.seed_scheduler_state);
        CHECK(!cb.update_ui_rotation);
    }

    TEST_CASE("RotationCallbacks fire correctly") {
        int stop_market_count = 0;
        int stop_user_count = 0;
        std::vector<std::string> started_token_ids;
        std::vector<std::string> started_condition_ids;
        int register_count = 0;

        RotationCallbacks cb;
        cb.stop_market_ws = [&]() { ++stop_market_count; };
        cb.stop_user_ws = [&]() { ++stop_user_count; };
        cb.start_market_ws = [&](const std::vector<std::string>& ids) {
            started_token_ids = ids;
        };
        cb.start_user_ws = [&](const std::vector<std::string>& ids) {
            started_condition_ids = ids;
        };
        cb.register_market = [&](const std::string&, const std::string&,
                                  const std::string&, bool, uint16_t) {
            ++register_count;
            return true;
        };

        cb.stop_market_ws();
        cb.stop_user_ws();
        cb.start_market_ws({"tok1", "tok2"});
        cb.start_user_ws({"cond1"});
        cb.register_market("c", "u", "d", false, 0);

        CHECK(stop_market_count == 1);
        CHECK(stop_user_count == 1);
        CHECK(started_token_ids.size() == 2);
        CHECK(started_token_ids[0] == "tok1");
        CHECK(started_condition_ids.size() == 1);
        CHECK(register_count == 1);
    }

    // --- Discovery helper tests ---

    TEST_CASE("timeframe_slug returns correct slugs") {
        CHECK(std::string(timeframe_slug(BtcTimeframe::BTC_5M)) == "5m");
        CHECK(std::string(timeframe_slug(BtcTimeframe::BTC_15M)) == "15m");
    }

    TEST_CASE("timeframe_period_seconds returns correct values") {
        CHECK(timeframe_period_seconds(BtcTimeframe::BTC_5M) == 300);
        CHECK(timeframe_period_seconds(BtcTimeframe::BTC_15M) == 900);
    }

    TEST_CASE("extract_host extracts hostname from URL") {
        CHECK(extract_host("https://gamma-api.polymarket.com") == "gamma-api.polymarket.com");
        CHECK(extract_host("https://example.com/path") == "example.com");
        CHECK(extract_host("http://localhost:8080") == "localhost:8080");
    }

    TEST_CASE("parse_gamma_response with empty body") {
        auto result = parse_gamma_response("");
        CHECK(!result.has_value());
    }

    TEST_CASE("parse_gamma_response with invalid JSON") {
        auto result = parse_gamma_response("not json");
        CHECK(!result.has_value());
    }

    TEST_CASE("parse_gamma_response with empty array") {
        auto result = parse_gamma_response("[]");
        CHECK(!result.has_value());
    }

    // --- RotationCoordinator constructor tests ---

    TEST_CASE("RotationCoordinator initial state") {
        AsyncLogger logger("test_rotation_coord.log");
        logger.start();

        std::atomic<bool> shutdown{false};
        std::atomic<bool> fatal{false};

        RotationConfig cfg;
        RotationCoordinator coord(cfg, logger, shutdown, fatal);

        CHECK(coord.rotation_phase().load() == static_cast<int>(RotationPhase::NORMAL));
        CHECK(coord.rotation_count() == 0);
        CHECK(coord.timing_context().enabled == false);
        CHECK(coord.current_market().condition_id.empty());
        CHECK(!coord.next_market().has_value());

        logger.stop();
        std::remove("test_rotation_coord.log");
    }

    // --- Config field tests ---

    TEST_CASE("EngineConfig rotation fields default to disabled") {
        EngineConfig cfg;
        CHECK(cfg.rotation_enabled == false);
        CHECK(cfg.rotation_timeframe == 0);
        CHECK(cfg.rotation_discovery_poll_ms == 15000);
        CHECK(cfg.rotation_pre_rotation_ms == 5000);
        CHECK(cfg.rotation_no_trade_start_ms == 0);
        CHECK(cfg.rotation_no_trade_end_ms == 0);
    }

    TEST_CASE("EngineConfig rotation fields parsed from JSON") {
        const char* path = "test_rotation_config_temp.json";
        {
            std::ofstream f(path);
            f << R"({
                "rotation_enabled": true,
                "rotation_timeframe": 1,
                "rotation_discovery_poll_ms": 20000,
                "rotation_pre_rotation_ms": 3000,
                "rotation_no_trade_start_ms": 15000,
                "rotation_no_trade_end_ms": 45000
            })";
        }

        auto result = load_config(path);
        CHECK(result.ok());
        CHECK(result.value.rotation_enabled == true);
        CHECK(result.value.rotation_timeframe == 1);
        CHECK(result.value.rotation_discovery_poll_ms == 20000);
        CHECK(result.value.rotation_pre_rotation_ms == 3000);
        CHECK(result.value.rotation_no_trade_start_ms == 15000);
        CHECK(result.value.rotation_no_trade_end_ms == 45000);

        std::remove(path);
    }

    // --- UI rotation info serialization test ---

    TEST_CASE("UiRotationInfo defaults") {
        UiRotationInfo info;
        CHECK(std::string(info.market_condition) == "");
        CHECK(info.window_start == 0);
        CHECK(info.window_end == 0);
        CHECK(info.rotation_count == 0);
        CHECK(info.in_no_trade == false);
    }
}
