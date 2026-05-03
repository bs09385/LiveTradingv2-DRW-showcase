#include "doctest/doctest.h"

#include "scheduler/risk_gate.h"

using namespace lt;

namespace {

// Writable inventory for testing
class TestInventory : public InventoryView {
public:
    void set(const AssetId& token, Qty_t pos) {
        positions_[token] = pos;
    }
    Qty_t position_for(const AssetId& token) const override {
        auto it = positions_.find(token);
        return it != positions_.end() ? it->second : 0;
    }
private:
    std::unordered_map<AssetId, Qty_t, AssetIdHash> positions_;
};

ExecutionIntent make_bid(const char* asset, const char* market,
                          Price_t price, Qty_t qty) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.asset_id = AssetId(asset);
    intent.market_id = AssetId(market);
    intent.price = price;
    intent.qty = qty;
    return intent;
}

ExecutionIntent make_ask(const char* asset, const char* market,
                          Price_t price, Qty_t qty) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.asset_id = AssetId(asset);
    intent.market_id = AssetId(market);
    intent.price = price;
    intent.qty = qty;
    return intent;
}

ExecutionIntent make_cancel() {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_CANCEL_BID;
    intent.exchange_order_id = OrderId("exch1");
    return intent;
}

}  // namespace

TEST_SUITE("RiskGate") {

TEST_CASE("Cancels always ALLOW") {
    RiskConfig cfg;
    cfg.max_position_per_token = 0;  // would deny any placement
    RiskGate gate(cfg, nullptr, nullptr, nullptr, nullptr);

    auto result = gate.evaluate(make_cancel());
    CHECK(result.decision == RiskDecision::ALLOW);
    CHECK(gate.allow_count() == 1);
}

TEST_CASE("No inventory: all placements ALLOW") {
    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, nullptr, nullptr, nullptr, nullptr);

    CHECK(gate.evaluate(make_bid("tok", "mkt", 5000, 100)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Position check: bid within limit") {
    TestInventory inv;
    inv.set(AssetId("tok"), 5);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    CHECK(gate.evaluate(make_bid("tok", "mkt", 5000, 5)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Position check: bid exceeds limit") {
    TestInventory inv;
    inv.set(AssetId("tok"), 5);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    auto r = gate.evaluate(make_bid("tok", "mkt", 5000, 6));
    CHECK(r.decision == RiskDecision::DENY);
    CHECK(r.reason == RiskDenyReason::POSITION_LIMIT);
    CHECK(gate.deny_count() == 1);
}

TEST_CASE("Position check: ask reduces position") {
    TestInventory inv;
    inv.set(AssetId("tok"), 8);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    // Ask sells, so projected = 8 - 5 = 3, within limit
    CHECK(gate.evaluate(make_ask("tok", "mkt", 5000, 5)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Position check: ask creates short beyond limit") {
    TestInventory inv;
    inv.set(AssetId("tok"), 0);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    // Ask sells, projected = 0 - 11 = -11, |projected| > 10
    CHECK(gate.evaluate(make_ask("tok", "mkt", 5000, 11)).decision == RiskDecision::DENY);
}

TEST_CASE("Net exposure check: within limit") {
    TestInventory inv;
    inv.set(AssetId("tok_up"), 5);
    inv.set(AssetId("tok_down"), 3);

    MarketPairRegistry registry;
    registry.add_pair(AssetId("mkt1"), AssetId("tok_up"), AssetId("tok_down"));

    RiskConfig cfg;
    cfg.max_position_per_token = 100;  // not the binding constraint
    cfg.max_net_exposure_per_market = 20;
    RiskGate gate(cfg, &inv, nullptr, &registry, nullptr);

    // Bid on tok_up: projected = 5+5=10, tok_down=3, net = 10+3 = 13 <= 20
    CHECK(gate.evaluate(make_bid("tok_up", "mkt1", 5000, 5)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Net exposure check: exceeds limit") {
    TestInventory inv;
    inv.set(AssetId("tok_up"), 10);
    inv.set(AssetId("tok_down"), 5);

    MarketPairRegistry registry;
    registry.add_pair(AssetId("mkt1"), AssetId("tok_up"), AssetId("tok_down"));

    RiskConfig cfg;
    cfg.max_position_per_token = 100;
    cfg.max_net_exposure_per_market = 20;
    RiskGate gate(cfg, &inv, nullptr, &registry, nullptr);

    // Bid on tok_up: projected = 10+6=16, tok_down=5, net = 16+5 = 21 > 20
    auto r_exp = gate.evaluate(make_bid("tok_up", "mkt1", 5000, 6));
    CHECK(r_exp.decision == RiskDecision::DENY);
    CHECK(r_exp.reason == RiskDenyReason::EXPOSURE_LIMIT);
}

TEST_CASE("Notional check: within limit") {
    WorkingOrderTracker tracker;
    // No working orders, so current notional = 0

    RiskConfig cfg;
    cfg.max_notional = 100000;
    RiskGate gate(cfg, nullptr, &tracker, nullptr, nullptr);

    // Intent notional = 5000 * 10 = 50000 < 100000
    CHECK(gate.evaluate(make_bid("tok", "mkt", 5000, 10)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Notional check: exceeds limit") {
    WorkingOrderTracker tracker;
    // Add working order: 5000 * 10 = 50000 notional
    ExecutionIntent existing;
    existing.action = IntentAction::WOULD_PLACE_BID;
    existing.client_order_id = OrderId("c1");
    existing.asset_id = AssetId("tok");
    existing.market_id = AssetId("mkt");
    existing.price = 5000;
    existing.qty = 10;
    tracker.on_intent_sent(existing);

    RiskConfig cfg;
    cfg.max_notional = 100000;
    RiskGate gate(cfg, nullptr, &tracker, nullptr, nullptr);

    // Current 50000 + new 5000*11=55000 = 105000 > 100000
    auto r_not = gate.evaluate(make_bid("tok", "mkt", 5000, 11));
    CHECK(r_not.decision == RiskDecision::DENY);
    CHECK(r_not.reason == RiskDenyReason::NOTIONAL_LIMIT);
}

TEST_CASE("cancel_all_on_violation: flag set on deny") {
    TestInventory inv;
    inv.set(AssetId("tok"), 10);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    cfg.cancel_all_on_violation = true;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    CHECK(gate.pending_cancel_all() == false);
    gate.evaluate(make_bid("tok", "mkt", 5000, 1));  // projected=11 > 10 => DENY
    CHECK(gate.pending_cancel_all() == true);

    gate.clear_pending_cancel_all();
    CHECK(gate.pending_cancel_all() == false);
}

TEST_CASE("cancel_all_on_violation: flag NOT set when disabled") {
    TestInventory inv;
    inv.set(AssetId("tok"), 10);

    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    cfg.cancel_all_on_violation = false;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    gate.evaluate(make_bid("tok", "mkt", 5000, 1));
    CHECK(gate.pending_cancel_all() == false);
}

TEST_CASE("Metrics: STRAT_RISK_DENIED incremented on deny") {
    TestInventory inv;
    inv.set(AssetId("tok"), 10);

    Metrics metrics;
    RiskConfig cfg;
    cfg.max_position_per_token = 10;
    RiskGate gate(cfg, &inv, nullptr, nullptr, &metrics);

    gate.evaluate(make_bid("tok", "mkt", 5000, 1));
    CHECK(metrics.get(MetricId::STRAT_RISK_DENIED) == 1);
}

TEST_CASE("check_count tracks all evaluations") {
    RiskConfig cfg;
    RiskGate gate(cfg, nullptr, nullptr, nullptr, nullptr);

    gate.evaluate(make_bid("tok", "mkt", 5000, 1));
    gate.evaluate(make_cancel());
    gate.evaluate(make_ask("tok", "mkt", 5000, 1));
    CHECK(gate.check_count() == 3);
}

TEST_CASE("Position check: zero max blocks everything") {
    TestInventory inv;
    inv.set(AssetId("tok"), 0);

    RiskConfig cfg;
    cfg.max_position_per_token = 0;
    RiskGate gate(cfg, &inv, nullptr, nullptr, nullptr);

    CHECK(gate.evaluate(make_bid("tok", "mkt", 5000, 1)).decision == RiskDecision::DENY);
    CHECK(gate.evaluate(make_ask("tok", "mkt", 5000, 1)).decision == RiskDecision::DENY);
    // But cancels still pass
    CHECK(gate.evaluate(make_cancel()).decision == RiskDecision::ALLOW);
}

TEST_CASE("Net exposure: no market pair -> skip check") {
    TestInventory inv;
    inv.set(AssetId("tok"), 0);

    MarketPairRegistry registry;
    // No pairs registered

    RiskConfig cfg;
    cfg.max_position_per_token = 100;
    cfg.max_net_exposure_per_market = 1;
    RiskGate gate(cfg, &inv, nullptr, &registry, nullptr);

    // No market pair found, so net exposure check is skipped
    CHECK(gate.evaluate(make_bid("tok", "mkt1", 5000, 50)).decision == RiskDecision::ALLOW);
}

TEST_CASE("Multiple checks: first failure short-circuits") {
    TestInventory inv;
    inv.set(AssetId("tok"), 50);

    WorkingOrderTracker tracker;

    RiskConfig cfg;
    cfg.max_position_per_token = 50;
    cfg.max_notional = 1000000;
    RiskGate gate(cfg, &inv, &tracker, nullptr, nullptr);

    // Position check fails first (50 + 1 = 51 > 50)
    CHECK(gate.evaluate(make_bid("tok", "mkt", 5000, 1)).decision == RiskDecision::DENY);
    CHECK(gate.deny_count() == 1);
}

}  // TEST_SUITE
