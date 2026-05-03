#include "scheduler/risk_gate.h"

#include <cstdlib>  // abs

namespace lt {

RiskGate::RiskGate(const RiskConfig& config,
                   const InventoryView* inventory,
                   const WorkingOrderTracker* tracker,
                   const MarketPairRegistry* market_pairs,
                   Metrics* metrics)
    : config_(config), inventory_(inventory), tracker_(tracker),
      market_pairs_(market_pairs), metrics_(metrics) {}

RiskResult RiskGate::evaluate(const ExecutionIntent& intent) {
    ++check_count_;

    // Cancels always ALLOW — never prevent order cancellation.
    if (intent.action == IntentAction::WOULD_CANCEL_BID ||
        intent.action == IntentAction::WOULD_CANCEL_ASK ||
        intent.action == IntentAction::WOULD_CANCEL_ALL) {
        ++allow_count_;
        return {RiskDecision::ALLOW, RiskDenyReason::NONE};
    }

    // --- Position check (includes pending working-order exposure) ---
    if (inventory_) {
        Qty_t current_position = inventory_->position_for(intent.asset_id);
        Qty_t pending = tracker_ ? tracker_->pending_exposure_for_token(intent.asset_id) : 0;
        Qty_t impact = intent.qty;
        if (intent.action == IntentAction::WOULD_PLACE_ASK) {
            impact = -impact;  // selling reduces position
        }
        Qty_t projected = current_position + pending + impact;
        if (std::abs(projected) > config_.max_position_per_token) {
            ++deny_count_;
            if (metrics_) metrics_->inc(MetricId::STRAT_RISK_DENIED);
            if (config_.cancel_all_on_violation) pending_cancel_all_ = true;
            return {RiskDecision::DENY, RiskDenyReason::POSITION_LIMIT};
        }
    }

    // --- Net exposure check per market (includes pending working-order exposure) ---
    if (inventory_ && market_pairs_ && intent.market_id.len > 0) {
        const MarketPair* pair = market_pairs_->find_by_condition(intent.market_id);
        if (pair) {
            Qty_t pos_up = inventory_->position_for(pair->token_id_up);
            Qty_t pos_down = inventory_->position_for(pair->token_id_down);

            // Include pending working-order exposure
            if (tracker_) {
                pos_up += tracker_->pending_exposure_for_token(pair->token_id_up);
                pos_down += tracker_->pending_exposure_for_token(pair->token_id_down);
            }

            // Apply intent impact to the relevant token
            Qty_t impact = intent.qty;
            if (intent.action == IntentAction::WOULD_PLACE_ASK) {
                impact = -impact;
            }
            if (intent.asset_id == pair->token_id_up) {
                pos_up += impact;
            } else if (intent.asset_id == pair->token_id_down) {
                pos_down += impact;
            }

            Qty_t net_exposure = std::abs(pos_up) + std::abs(pos_down);
            if (net_exposure > config_.max_net_exposure_per_market) {
                ++deny_count_;
                if (metrics_) metrics_->inc(MetricId::STRAT_RISK_DENIED);
                if (config_.cancel_all_on_violation) pending_cancel_all_ = true;
                return {RiskDecision::DENY, RiskDenyReason::EXPOSURE_LIMIT};
            }
        }
    }

    // --- Notional check ---
    if (tracker_) {
        int64_t current_notional = tracker_->total_working_notional();
        int64_t intent_notional = static_cast<int64_t>(intent.price) * intent.qty;
        if (current_notional + intent_notional > config_.max_notional) {
            ++deny_count_;
            if (metrics_) metrics_->inc(MetricId::STRAT_RISK_DENIED);
            if (config_.cancel_all_on_violation) pending_cancel_all_ = true;
            return {RiskDecision::DENY, RiskDenyReason::NOTIONAL_LIMIT};
        }
    }

    // Loss check is reserved for future implementation.
    // Mark-to-market would require per-token mark prices, which aren't tracked yet.

    ++allow_count_;
    return {RiskDecision::ALLOW, RiskDenyReason::NONE};
}

}  // namespace lt
