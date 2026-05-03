#pragma once

#include <cstdint>

#include "common/market_pair.h"
#include "common/pnl_tracker.h"
#include "common/types.h"
#include "events/scheduler_events.h"
#include "logger/metrics.h"
#include "scheduler/quote_planner.h"
#include "scheduler/strategy.h"
#include "scheduler/working_order_tracker.h"

namespace lt {

// =============================================================================
// QuoterV2Strategy — boilerplate template
// =============================================================================
//
// The proprietary quoting / risk logic has been removed from this snapshot.
// What remains is the public interface and wiring contract: a Strategy
// implementation that the scheduler can construct, configure, drive with
// events, and dispatch to the execution gateway.
//
// To build your own strategy on top of this skeleton:
//   1. Add fields to QuoterV2Config that drive your sizing / pricing.
//   2. Hold per-market state (e.g. working quotes, last-fill prices) in a
//      private map keyed by condition_id.
//   3. In evaluate(), inspect ctx.event(), read books from ctx.shadow_book(),
//      and emit zero or more ExecIntent entries into the returned IntentBatch.
//   4. Process exec / user feedback events to keep working-order state in sync.
//
// All hot-path rules apply: no heap allocation in evaluate(), no blocking,
// no exceptions; return ErrorCode-style sentinels instead.
//
// =============================================================================

enum class SkewMode : uint8_t {
    SYMMETRIC = 0,
    ASYMMETRIC = 1,
};

// Configuration kept verbatim so config loader and main.cpp wiring still
// compile without modification. Field semantics belong to the strategy and
// are intentionally undocumented in this public skeleton.
struct QuoterV2Config {
    Price_t offset = 300;
    int32_t skew_strength = 20;
    int32_t max_skew = 200;
    int32_t max_inventory = 50;
    int32_t quote_size = 20;
    int32_t min_order_size = 5;
    int32_t emergency_qty = 10;
    Price_t price_floor = 900;
    Price_t price_ceiling = 9100;
    int32_t initial_split_size = 200;
    int32_t inventory_low_water = 100;
    int32_t inventory_replenish_size = 100;
    int32_t inventory_merge_threshold = 250;
    int32_t inventory_merge_size = 50;
    int64_t inventory_cooldown_ms = 30000;
    int max_replaces_per_second = 5;
    int64_t min_quote_lifetime_ms = 500;
    int64_t degraded_refresh_ms = 5000;
    bool deterministic_timing = false;
    SkewMode skew_mode = SkewMode::SYMMETRIC;

    int32_t soft_max_inventory = 20;
    int32_t hard_max_inventory = 50;

    int64_t market_duration_ms = 300000;
    int64_t hard_cutoff_ms = 12000;

    int32_t gamma_power_x1000 = 1200;
    int32_t offset_growth_fp4 = 100;
    int32_t time_floor_mult_x1000 = 3000;
    int64_t time_floor_threshold_ms = 20000;

    int64_t fak_cooldown_ms = 200;
    int32_t fak_gamma_floor_x1000 = 0;

    Price_t stop_loss_distance_fp4 = 1500;
};

// Stub Strategy implementation. evaluate() returns an empty batch — wire your
// own quoting logic in here. All other overrides are minimal pass-throughs.
class QuoterV2Strategy : public Strategy {
public:
    QuoterV2Strategy(const QuoterV2Config& config,
                     const WorkingOrderTracker* tracker,
                     const MarketPairRegistry* market_pairs,
                     Metrics* metrics)
        : config_(config),
          tracker_(tracker),
          market_pairs_(market_pairs),
          metrics_(metrics) {}

    IntentBatch evaluate(const StrategyContext& /*ctx*/) override {
        return IntentBatch{};
    }

    void set_enabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

    void set_spread_ticks(int ticks) override { config_.offset = ticks * 100; }
    void set_size(Qty_t size) override {
        config_.quote_size = static_cast<int32_t>(size / kQtyScale);
    }

    int spread_ticks() const override { return config_.offset / 100; }
    Qty_t quote_size() const override { return qty_from_int(config_.quote_size); }

    void set_planner(const QuotePlanner* planner) override { planner_ = planner; }

    void reset_all_quotes() override {}

    void on_gateway_degraded() override { degraded_ = true; }
    void on_gateway_recovered() override { degraded_ = false; }
    void on_rate_limited() override { degraded_ = true; }

    // QuoterV2-specific hooks retained for scheduler / main.cpp wiring.
    void set_dry_run(bool v) { dry_run_ = v; }
    void set_pnl_tracker(PnlTracker* tracker) { pnl_tracker_ = tracker; }

    const QuoterV2Config& config() const { return config_; }

private:
    QuoterV2Config config_;
    const WorkingOrderTracker* tracker_;
    const MarketPairRegistry* market_pairs_;
    const QuotePlanner* planner_ = nullptr;
    Metrics* metrics_;
    PnlTracker* pnl_tracker_ = nullptr;

    bool enabled_ = false;
    bool degraded_ = false;
    bool dry_run_ = true;
};

}  // namespace lt
