#pragma once

#include <cstdio>

#include "common/market_pair.h"
#include "common/types.h"
#include "events/scheduler_events.h"
#include "inventory/inventory_sink.h"
#include "scheduler/strategy.h"

namespace lt {

// 3-phase inventory-operations test strategy.
//
// Phase 1 (SPLIT):
//   Wait for first valid BBO to capture condition_id, then fire
//   InventoryOpRequest SPLIT for kSplitQty (2 shares).
//
// Phase 2 (MERGE):
//   Hold 10s after SPLIT, then fire MERGE for kMergeQty (1 share).
//
// Phase 3 (REDEEM):
//   Wait for MARKET_RESOLVED matching captured condition_id, then fire
//   REDEEM for the winning token (quantity=0 means "all available").
//
// No orders are placed — all actions go through InventoryOpSink::try_request().
// T2-owned: no thread safety needed.
enum class InvTestPhase : uint8_t {
    WAIT_FOR_BBO = 0,
    SPLIT_REQUEST,
    WAIT_SPLIT_HOLD,
    MERGE_REQUEST,
    WAIT_RESOLVED,
    REDEEM_REQUEST,
    DONE,
};

class InventoryTestStrategy : public Strategy {
public:
    InventoryTestStrategy(const MarketPairRegistry* market_pairs,
                          const char* log_path = "inventory_test.log");
    ~InventoryTestStrategy();

    IntentBatch evaluate(const StrategyContext& ctx) override;

    void set_enabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

    void on_gateway_degraded() override {}
    void on_gateway_recovered() override {}

    // Test accessors
    InvTestPhase phase() const { return phase_; }
    const AssetId& captured_condition_id() const { return captured_condition_id_; }
    uint32_t next_request_id() const { return next_request_id_; }

private:
    static constexpr Qty_t kSplitQty = 2 * kQtyScale;
    static constexpr Qty_t kMergeQty = 1 * kQtyScale;
    static constexpr Timestamp_ns kTimeoutNs = 30'000'000'000LL;  // 30 seconds
    static constexpr Timestamp_ns kHoldNs = 10'000'000'000LL;     // 10 seconds

    void transition(InvTestPhase next, Timestamp_ns now);
    bool check_timeout(Timestamp_ns now);

    // Phase handlers
    void handle_wait_for_bbo(const StrategyContext& ctx);
    void handle_split_request(const StrategyContext& ctx);
    void handle_wait_split_hold(const StrategyContext& ctx);
    void handle_merge_request(const StrategyContext& ctx);
    void handle_wait_resolved(const StrategyContext& ctx);
    void handle_redeem_request(const StrategyContext& ctx);

    const MarketPairRegistry* market_pairs_;
    bool enabled_ = false;

    InvTestPhase phase_ = InvTestPhase::WAIT_FOR_BBO;
    uint32_t next_request_id_ = 0;
    Timestamp_ns state_entered_ts_ = 0;

    // Captured from first BBO
    AssetId captured_condition_id_;

    // Log file handle
    FILE* log_file_ = nullptr;
};

}  // namespace lt
