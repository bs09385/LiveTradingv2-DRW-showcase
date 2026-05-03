#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "common/config.h"
#include "common/market_pair.h"
#include "common/types.h"
#include "events/scheduler_events.h"
#include "queue/spsc_queue.h"
#include "ui_bridge/ui_types.h"

namespace lt {

class Metrics;
class AsyncLogger;
class PnlTracker;
class TokenInventory;

struct IpcBridgeConfig {
    int ws_port = 9090;
    int snapshot_rate_hz = 20;
    int book_depth = 10;

    // Watcher settings
    bool watcher_enabled = false;
    int64_t watcher_discovery_poll_ms = 30000;
    int64_t watcher_status_poll_ms = 10000;
    int64_t watcher_stale_threshold_ms = 15000;
    int ladder_update_interval_ms = 100;
    int ladder_max_depth = 200;

    // Account positions value polling
    int account_poll_interval_ms = 15000;

    // On-chain balance polling
    std::string polygon_rpc_url;
    int balance_poll_interval_ms = 1000;
    std::string eoa_address;  // signer EOA for POL balance display

    // WebSocket auth token (empty = no auth, backward compatible)
    std::string auth_token = "";
    std::string bind_address = "0.0.0.0";
};

class IpcBridge {
public:
    IpcBridge(SpscQueue<UiBookUpdate>& book_queue,
              SpscQueue<UiStateSnapshot>& state_queue,
              SpscQueue<SchedulerEvent>& control_queue,
              Metrics& metrics,
              AsyncLogger& logger,
              const IpcBridgeConfig& config,
              const MarketPairRegistry* market_pairs,
              const std::vector<MarketPairConfig>* market_pair_configs,
              std::atomic<bool>* fatal_flag = nullptr);
    ~IpcBridge();

    IpcBridge(const IpcBridge&) = delete;
    IpcBridge& operator=(const IpcBridge&) = delete;

    // Run the bridge event loop (blocks). Called from T6 thread.
    void run();

    // Thread-safe shutdown request.
    void request_shutdown();

    // Provide EngineConfig for WatcherService (must call before run() if watcher enabled).
    void set_engine_config(const EngineConfig& config);

    // Set account identity for UI snapshot display. Call before run().
    void set_account_info(const std::string& name, const std::string& address);

    // Set account credentials and/or address for account value polling. Call before run().
    void set_account_credentials(const std::string& api_key,
                                  const std::string& api_secret,
                                  const std::string& api_passphrase,
                                  const std::string& address);

    // Set token inventory for USDC balance display and on-chain polling. Call before run().
    void set_token_inventory(TokenInventory* inventory);

    // Set PnL tracker for realized PnL display. Call before run().
    void set_pnl_tracker(const PnlTracker* tracker);

    // Set rotation info for UI display. Thread-safe (called from T7).
    void set_rotation_info(const std::string& condition_id,
                           int64_t window_start, int64_t window_end,
                           int rotation_count, bool in_no_trade);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
