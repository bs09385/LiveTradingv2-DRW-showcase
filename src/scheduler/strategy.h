#pragma once

#include "common/types.h"
#include "events/scheduler_events.h"
#include "rotation/rotation_coordinator.h"
#include "binance/binance_types.h"
#include "rtds/rtds_types.h"
#include "scheduler/strategy_context.h"

namespace lt { class QuotePlanner; }

namespace lt {

// Polymorphic strategy interface. Replaces direct StrategyStub usage in
// the scheduler when a real strategy is wired. Virtual dispatch cost
// (~2ns) is negligible vs work inside evaluate().
class Strategy {
public:
    virtual ~Strategy() = default;

    // Evaluate an event with full strategy context and produce zero or more
    // execution intents. The context provides access to shadow order books,
    // inventory, working orders, market pairs, and timing information.
    virtual IntentBatch evaluate(const StrategyContext& ctx) = 0;

    virtual void set_enabled(bool enabled) = 0;
    virtual bool enabled() const = 0;

    virtual void set_spread_ticks(int /*ticks*/) {}
    virtual void set_size(Qty_t /*size*/) {}

    // Planner pointer for live tick size lookup (set by scheduler at wiring time)
    virtual void set_planner(const QuotePlanner* /*planner*/) {}

    // Read-only accessors for UI snapshot display
    // Defaults are safe: wide spread + minimum size to avoid accidental BBO draining
    virtual int spread_ticks() const { return 30; }
    virtual Qty_t quote_size() const { return qty_from_int(5); }

    // Clear all internal quote state (called on mode switch DRY_RUN -> LIVE).
    virtual void reset_all_quotes() {}

    // Gateway health callbacks
    virtual void on_gateway_degraded() = 0;
    virtual void on_gateway_recovered() = 0;

    // Rate limiting callback (EXEC_INTERNAL RATE_LIMITED feedback)
    virtual void on_rate_limited() {}

    // Rotation timing context — called by T2 after rotation completes.
    virtual void set_rotation_timing(const RotationTimingContext& ctx) { (void)ctx; }

    // Crypto price update from RTDS (T_rtds -> T2 -> strategy).
    // Called for each crypto_prices update received from the RTDS WebSocket.
    // Default no-op; override to use crypto reference prices in quoting decisions.
    virtual void on_crypto_price(const CryptoPriceUpdate& update) { (void)update; }

    // Binance market-data update (T_binance_md -> T2 -> strategy).
    // Fired for each bookTicker/trade/aggTrade frame received from the
    // Binance Spot WebSocket. Default no-op; override to consume the
    // real-time BTC (or other crypto) reference price in quoting logic.
    virtual void on_binance_update(const BinanceMarketUpdate& update) { (void)update; }
};

}  // namespace lt
