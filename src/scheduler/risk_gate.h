#pragma once

#include <cstdint>

#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "events/scheduler_events.h"
#include "logger/metrics.h"
#include "recorder/journal_types.h"
#include "scheduler/working_order_tracker.h"

namespace lt {

// RiskResult: returned by RiskGate::evaluate() with deny reason.
struct RiskResult {
    RiskDecision decision = RiskDecision::ALLOW;
    RiskDenyReason reason = RiskDenyReason::NONE;
};

struct RiskConfig {
    Qty_t max_position_per_token = 50;      // max absolute position per token
    Qty_t max_net_exposure_per_market = 50;  // max net exposure across both tokens
    int64_t max_notional = 500000;           // max total working notional
    int64_t max_loss = 100000;               // max unrealized loss (reserved, not enforced yet)
    bool cancel_all_on_violation = false;    // trigger cancel-all on any deny
};

// Real risk gate: evaluates placement intents against position, notional,
// and exposure limits. Cancels always ALLOW.
//
// Reads from InventoryView (atomic, cross-thread safe for T1-written positions)
// and WorkingOrderTracker (T2-owned).
//
// T2-owned: no thread safety needed for own state.
class RiskGate {
public:
    RiskGate(const RiskConfig& config,
             const InventoryView* inventory,
             const WorkingOrderTracker* tracker,
             const MarketPairRegistry* market_pairs,
             Metrics* metrics);

    // Evaluate a single intent. Returns decision + deny reason.
    RiskResult evaluate(const ExecutionIntent& intent);

    // Check and clear pending_cancel_all flag (set when violation + cancel_all_on_violation)
    bool pending_cancel_all() const { return pending_cancel_all_; }
    void clear_pending_cancel_all() { pending_cancel_all_ = false; }

    // Update mark prices for loss calculation (reserved for future use)
    void update_mark(const AssetId& /*token_id*/, Price_t /*mark_price*/) {
        // Not enforced in M5 — loss check is stubbed
    }

    // Config accessors
    void set_config(const RiskConfig& config) { config_ = config; }
    const RiskConfig& config() const { return config_; }

    int64_t check_count() const { return check_count_; }
    int64_t allow_count() const { return allow_count_; }
    int64_t deny_count() const { return deny_count_; }

private:
    RiskConfig config_;
    const InventoryView* inventory_;
    const WorkingOrderTracker* tracker_;
    const MarketPairRegistry* market_pairs_;
    Metrics* metrics_;

    bool pending_cancel_all_ = false;

    int64_t check_count_ = 0;
    int64_t allow_count_ = 0;
    int64_t deny_count_ = 0;
};

}  // namespace lt
