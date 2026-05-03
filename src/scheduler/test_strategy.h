#pragma once

#include <cstdio>

#include "common/market_pair.h"
#include "common/types.h"
#include "events/scheduler_events.h"
#include "scheduler/quote_planner.h"
#include "scheduler/strategy.h"
#include "scheduler/working_order_tracker.h"

namespace lt {

// 3-phase test strategy state machine.
//
// Phase 1 (GTC place + hold + cancel):
//   Place GTC BID@100 + ASK@9900 in one batch. Wait for both accepted,
//   then both LIVE. Hold 10s. Cancel both. Wait for both cancel confirms.
//
// Phase 2 (near-BBO limits + fill/hedge):
//   Refresh BBO. Place GTC BID at best_ask-1tick, ASK at best_bid+1tick.
//   Wait up to 10s for fills. On partial fill: FAK hedge filled side +
//   cancel unfilled. Both filled: skip to P3. Neither filled: cancel both.
//
// Phase 3 (FAK market orders):
//   FAK BUY at latest_ask (crosses spread). Wait for ack/fill.
//   FAK SELL at latest_bid (crosses spread). Wait for ack/fill.
//   Self-disable. Log summary.
//
// T2-owned: no thread safety needed.
enum class TestPhase : uint8_t {
    WAIT_FOR_BBO = 0,

    // Phase 1: GTC at extremes
    P1_PLACE,
    P1_WAIT_ACCEPTED,
    P1_WAIT_LIVE,
    P1_HOLD,
    P1_CANCEL,
    P1_WAIT_CANCELED,

    // Phase 2: near-BBO limits
    P2_PLACE,
    P2_WAIT_ACCEPTED,
    P2_WAIT_FILLS,
    P2_CLEANUP,
    P2_WAIT_CLEANUP,

    // Phase 3: FAK taker
    P3_FAK_BUY,
    P3_WAIT_BUY,
    P3_FAK_SELL,
    P3_WAIT_SELL,

    DONE,
};

// Dual-order tracking slot
struct OrderSlot {
    OrderId client_id;
    OrderId exchange_id;
    bool accepted = false;
    bool live = false;
    bool filled = false;
    bool canceled = false;
    bool rejected = false;
    Qty_t filled_qty = 0;
    Side side = Side::BID;

    void reset() {
        client_id = OrderId();
        exchange_id = OrderId();
        accepted = false;
        live = false;
        filled = false;
        canceled = false;
        rejected = false;
        filled_qty = 0;
        side = Side::BID;
    }
};

class TestStrategy : public Strategy {
public:
    TestStrategy(const WorkingOrderTracker* tracker,
                 const MarketPairRegistry* market_pairs,
                 const char* log_path = "smoke_test.log");
    ~TestStrategy();

    IntentBatch evaluate(const StrategyContext& ctx) override;

    void set_enabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

    void on_gateway_degraded() override {}
    void on_gateway_recovered() override {}

    void set_planner(const QuotePlanner* planner) override { planner_ = planner; }
    int spread_ticks() const override { return 30; }
    Qty_t quote_size() const override { return 5 * kQtyScale; }

    // Test accessors
    TestPhase phase() const { return phase_; }
    const OrderSlot& slot(int i) const { return slot_[i]; }

private:
    static constexpr Qty_t kMinLimitQty = 5 * kQtyScale;   // Polymarket minimum limit order (5 shares)
    static constexpr Qty_t kMinTakerNotional = 10000;     // $1 in 10000x fixed-point (price units, not qty)
    static constexpr Timestamp_ns kTimeoutNs = 30'000'000'000LL;  // 30 seconds
    static constexpr Timestamp_ns kHoldNs = 10'000'000'000LL;     // 10 seconds

    void transition(TestPhase next, Timestamp_ns now);
    bool check_timeout(Timestamp_ns now);
    void refresh_bbo(const SchedulerEvent& event);
    bool process_order_event(const SchedulerEvent& event);
    static Qty_t taker_qty_for_price(Price_t price);

    // Phase handlers
    IntentBatch handle_wait_for_bbo(const SchedulerEvent& event);
    IntentBatch handle_p1_place(Timestamp_ns now);
    IntentBatch handle_p1_wait_accepted(const SchedulerEvent& event);
    IntentBatch handle_p1_wait_live(const SchedulerEvent& event);
    IntentBatch handle_p1_hold(Timestamp_ns now);
    IntentBatch handle_p1_cancel(Timestamp_ns now);
    IntentBatch handle_p1_wait_canceled(const SchedulerEvent& event);
    IntentBatch handle_p2_place(Timestamp_ns now);
    IntentBatch handle_p2_wait_accepted(const SchedulerEvent& event);
    IntentBatch handle_p2_wait_fills(const SchedulerEvent& event, Timestamp_ns now);
    IntentBatch handle_p2_cleanup(Timestamp_ns now);
    IntentBatch handle_p2_wait_cleanup(const SchedulerEvent& event);
    IntentBatch handle_p3_fak_buy(Timestamp_ns now);
    IntentBatch handle_p3_wait_buy(const SchedulerEvent& event);
    IntentBatch handle_p3_fak_sell(Timestamp_ns now);
    IntentBatch handle_p3_wait_sell(const SchedulerEvent& event);

    const WorkingOrderTracker* tracker_;
    const MarketPairRegistry* market_pairs_;
    const QuotePlanner* planner_ = nullptr;
    bool enabled_ = false;

    TestPhase phase_ = TestPhase::WAIT_FOR_BBO;
    uint32_t next_intent_id_ = 0;
    Timestamp_ns state_entered_ts_ = 0;

    // Captured from first BBO
    AssetId captured_asset_id_;
    AssetId captured_market_id_;

    // Continuously refreshed BBO
    Price_t latest_bid_ = kInvalidPrice;
    Price_t latest_ask_ = kInvalidPrice;

    // Dual-order tracking: slot_[0] = BID, slot_[1] = ASK
    OrderSlot slot_[2];

    // Phase 2 cleanup tracking
    bool cleanup_hedge_done_ = false;
    bool cleanup_cancel_done_ = false;
    int cleanup_unfilled_idx_ = -1;
    bool cleanup_retry_issued_ = false;

    // Log file handle
    FILE* log_file_ = nullptr;
};

}  // namespace lt
