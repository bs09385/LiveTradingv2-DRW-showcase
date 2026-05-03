#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "common/discovery.h"
#include "logger/async_logger.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

// Timing context passed from T7 to T2 strategy. Written by T7 before
// RESUME_REQUESTED, read by T2 after RESUME_REQUESTED. No concurrent access.
struct RotationTimingContext {
    int64_t window_start_unix_s = 0;
    int64_t window_end_unix_s = 0;
    int64_t period_s = 300;
    int64_t no_trade_start_ms = 0;
    int64_t no_trade_end_ms = 0;
    bool enabled = false;
};

// Atomic state machine shared between T7 (writer of 0->1, 2->3) and T2 (writer of 1->2, 3->0).
enum class RotationPhase : int {
    NORMAL           = 0,  // Normal operation
    PAUSE_REQUESTED  = 1,  // T7 requests T2 to pause
    PAUSED           = 2,  // T2 has paused (cancelled orders, disabled strategy)
    RESUME_REQUESTED = 3   // T7 has completed rotation, requests T2 to resume
};

// Callbacks invoked by RotationCoordinator to control other threads.
// All called from T7 (the main thread rotation loop).
struct RotationCallbacks {
    std::function<void()> stop_market_ws;
    std::function<void()> stop_user_ws;
    std::function<void(const std::vector<std::string>& token_ids)> start_market_ws;
    std::function<void(const std::vector<std::string>& condition_ids)> start_user_ws;
    std::function<bool(const std::string& cond, const std::string& up, const std::string& down, bool neg_risk, uint16_t fee_rate_bps)> register_market;
    std::function<void(const std::string& id)> register_token;
    std::function<void(const std::string& id)> seed_market_state;
    std::function<void(const std::string& id)> seed_scheduler_state;
    // UI update: push rotation state to IpcBridge (thread-safe, called from T7)
    std::function<void(const std::string& condition_id, int64_t window_start,
                       int64_t window_end, int rotation_count, bool in_no_trade)> update_ui_rotation;
    // Position bootstrap: fetch positions from Data API after discovery
    std::function<void(const std::vector<std::string>& condition_ids)> bootstrap_positions;

    // Inventory: fire redeem for old market after rotation (nullable)
    std::function<bool(const std::string& condition_id,
                       const std::string& token_id_up,
                       const std::string& token_id_down)> fire_redeem;
    std::function<bool()> is_exec_queue_drained;  // returns true when strategy_to_exec_queue is empty
};

// Configuration for the rotation coordinator.
struct RotationConfig {
    BtcTimeframe timeframe = BtcTimeframe::BTC_5M;
    int64_t discovery_poll_ms = 15000;
    int64_t pre_rotation_ms = 5000;
    int64_t no_trade_start_ms = 0;
    int64_t no_trade_end_ms = 0;
    std::string discovery_api_url = "https://gamma-api.polymarket.com";
};

// RotationCoordinator — runs on the main thread (T7) after all other threads
// are started. Handles:
//   1. Discovery polling to find current and next BTC markets
//   2. Coordinated rotation at each window boundary
//   3. Timing context updates to strategy
//
// Pre-registration strategy: At startup, discovers current AND next markets
// and registers both. At the first rotation, switches to the already-registered
// "next" market. Then discovers the new "next" market during the pause.
class RotationCoordinator {
public:
    RotationCoordinator(const RotationConfig& config,
                        AsyncLogger& logger,
                        std::atomic<bool>& shutdown_flag,
                        std::atomic<bool>& fatal_flag);

    // Blocking rotation loop. Performs initial discovery, registers markets
    // via callbacks, starts T0/T1, then loops handling rotations until shutdown.
    // Returns when shutdown is requested.
    void run(RotationCallbacks cb);

    // Thread-safe shutdown request.
    void request_shutdown();

    // T7-T2 shared atomic phase. T2 reads/writes this in its main loop.
    std::atomic<int>& rotation_phase() { return rotation_phase_; }

    // Timing context — written by T7 before RESUME_REQUESTED, read by T2 after.
    const RotationTimingContext& timing_context() const { return timing_context_; }

    // Initial discovery results (available after run() starts, before callbacks fire).
    const DiscoveredMarket& current_market() const { return current_market_; }
    const std::optional<DiscoveredMarket>& next_market() const { return next_market_; }

    // Rotation count (for UI display).
    int rotation_count() const { return rotation_count_; }

private:
    // Discover market for a specific timestamp window.
    std::optional<DiscoveredMarket> discover_market(int64_t window_ts);

    // Compute window boundaries from a unix timestamp.
    int64_t window_start(int64_t unix_s) const;
    int64_t window_end(int64_t unix_s) const;

    // Execute the coordinated pause protocol.
    void execute_rotation(RotationCallbacks& cb, const DiscoveredMarket& new_next);

    // Register a market pair via callbacks (all registries + seeding).
    void register_market_full(RotationCallbacks& cb, const DiscoveredMarket& mkt);

    // Sleep in small increments, checking for shutdown.
    bool sleep_ms(int64_t ms);

    RotationConfig config_;
    AsyncLogger& logger_;
    ProducerHandle log_handle_;
    std::atomic<bool>& shutdown_flag_;
    std::atomic<bool>& fatal_flag_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<int> rotation_phase_{static_cast<int>(RotationPhase::NORMAL)};
    RotationTimingContext timing_context_;

    DiscoveredMarket current_market_;
    std::optional<DiscoveredMarket> next_market_;
    std::optional<DiscoveredMarket> pre_discovered_next_;  // Market two windows ahead, pre-cached
    int rotation_count_ = 0;

    std::string discovery_host_;

    // Pending redeems for old markets after rotation.
    // Oracle resolution can take minutes; fire redeem after a delay.
    struct PendingRedeem {
        std::string condition_id;
        std::string token_id_up;
        std::string token_id_down;
        std::chrono::steady_clock::time_point fire_at;
    };
    std::vector<PendingRedeem> pending_redeems_;
    static constexpr int64_t kRedeemDelayMs = 180000;  // 3 minutes
};

}  // namespace lt
