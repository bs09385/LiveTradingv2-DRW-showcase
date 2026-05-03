#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/discovery.h"
#include "events/scheduler_events.h"
#include "logger/async_logger.h"
#include "queue/spsc_queue.h"
#include "slot/market_slot_types.h"

namespace lt {

// Callbacks invoked by MarketSlotManager to control other threads.
// All called from T7 (the main thread slot manager loop).
struct SlotManagerCallbacks {
    // WS dynamic subscription (additive — no stop/restart)
    std::function<void(const std::vector<std::string>& token_ids)> subscribe_market_ws;
    std::function<void(const std::vector<std::string>& token_ids)> unsubscribe_market_ws;
    std::function<void(const std::vector<std::string>& condition_ids)> subscribe_user_ws;
    std::function<void(const std::vector<std::string>& condition_ids)> unsubscribe_user_ws;

    // Initial WS start (all tokens at once)
    std::function<void(const std::vector<std::string>& token_ids)> start_market_ws;
    std::function<void(const std::vector<std::string>& condition_ids)> start_user_ws;

    // Registry seeding (T7 registers before data flows)
    std::function<bool(const std::string& cond, const std::string& up,
                       const std::string& down, bool neg_risk,
                       uint16_t fee_rate_bps)> register_market;
    std::function<void(const std::string& id)> register_token;
    std::function<void(const std::string& id)> seed_market_state;
    std::function<void(const std::string& id)> seed_scheduler_state;
    std::function<void(const std::string& id)> seed_strategy_book;

    // Position bootstrap: fetch positions from Data API after discovery
    std::function<void(const std::vector<std::string>& condition_ids)> bootstrap_positions;

    // Inventory: fire redeem for old market
    std::function<bool(const std::string& condition_id,
                       const std::string& token_id_up,
                       const std::string& token_id_down)> fire_redeem;

    // UI update
    std::function<void(SlotName slot, const std::string& condition_id,
                       int64_t window_start, int64_t window_end,
                       SlotPhase phase)> update_ui_slot;
};

// Configuration for MarketSlotManager.
struct SlotManagerConfig {
    int64_t discovery_poll_ms = 15000;
    int64_t pre_subscribe_ms = 150000;    // 2.5 minutes before window start
    int64_t cancel_lead_ms = 1000;        // cancel orders 1s before window end
    int64_t no_trade_start_ms = 0;
    int64_t no_trade_end_ms = 0;
    int64_t redeem_delay_ms = 180000;     // 3 minutes after resolve
    std::string discovery_api_url = "https://gamma-api.polymarket.com";
    bool enable_5m = true;
    bool enable_15m = true;
};

// MarketSlotManager — runs on T7 (main thread after startup).
// Manages 6 slots: current/next/previous for both 5M and 15M.
// Two independent rotation timers. Per-slot lifecycle events
// pushed to slot_queue (T7->T2) for T2 processing.
class MarketSlotManager {
public:
    MarketSlotManager(const SlotManagerConfig& config,
                      SpscQueue<SchedulerEvent>& slot_queue,
                      AsyncLogger& logger,
                      std::atomic<bool>& shutdown_flag,
                      std::atomic<bool>& fatal_flag);

    // Blocking main loop. Performs initial discovery, starts T0/T1,
    // then loops handling rotations until shutdown.
    void run(SlotManagerCallbacks cb);

    // Thread-safe shutdown request.
    void request_shutdown();

    // Read-only accessors
    const MarketSlot& slot(SlotName s) const { return slots_[static_cast<int>(s)]; }
    const SlotManagerConfig& config() const { return config_; }

private:
    // Discovery helpers
    std::optional<DiscoveredMarket> discover_market(BtcTimeframe tf, int64_t window_start_s);
    void register_market_full(SlotManagerCallbacks& cb, const DiscoveredMarket& mkt);
    int64_t window_start_for(BtcTimeframe tf, int64_t unix_s) const;
    int64_t window_end_for(BtcTimeframe tf, int64_t unix_s) const;

    // Initial setup: discover and register all 4 active slots
    bool initial_discovery(SlotManagerCallbacks& cb);

    // Per-timeframe rotation check
    void check_rotation(int64_t now_s, BtcTimeframe tf, SlotManagerCallbacks& cb);

    // Previous market background processing (resolve + redeem)
    void check_previous_markets(int64_t now_s, SlotManagerCallbacks& cb);

    // Discovery polling for pending DISCOVERING slots
    void check_pending_discoveries(int64_t now_s, SlotManagerCallbacks& cb);

    // Push a slot event to the T7->T2 queue
    void push_slot_event(SchedulerEventKind kind, SlotName slot, const std::string& condition_id);

    // Helper: sleep in small increments checking shutdown
    bool sleep_ms(int64_t ms);

    // State
    SlotManagerConfig config_;
    SpscQueue<SchedulerEvent>& slot_queue_;
    AsyncLogger& logger_;
    ProducerHandle log_handle_;
    std::atomic<bool>& shutdown_flag_;
    std::atomic<bool>& fatal_flag_;
    std::atomic<bool> stop_requested_{false};

    MarketSlot slots_[kSlotCount]{};
    PreviousMarketState previous_5m_;
    PreviousMarketState previous_15m_;

    std::string discovery_host_;

    // Track market_resolved events received (set from T0 via WS data path)
    // T7 polls these flags periodically in check_previous_markets.
    // Written by T0 thread (via atomic), read by T7.
    std::atomic<bool> resolved_5m_{false};
    std::atomic<bool> resolved_15m_{false};
};

}  // namespace lt
