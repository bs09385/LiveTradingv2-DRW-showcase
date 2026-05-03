#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/error.h"

namespace lt {

struct MarketPairConfig {
    std::string condition_id;
    std::string token_id_up;
    std::string token_id_down;
};

struct EngineConfig {
    std::string ws_endpoint = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    std::vector<std::string> asset_ids;
    std::vector<MarketPairConfig> market_pairs;
    int64_t ping_interval_ms = 10000;
    int64_t pong_timeout_ms = 5000;
    int64_t reconnect_base_ms = 1000;
    int64_t reconnect_max_ms = 30000;
    int ws_redundancy = 1;               // parallel market WS connections (1-20)
    int64_t ws_redundancy_stagger_ms = 200;
    std::size_t strategy_queue_capacity = 65536;
    std::size_t log_queue_capacity = 8192;
    int64_t scheduler_stats_interval_ms = 5000;
    std::string log_file = "engine.log";
    std::string metrics_file = "metrics.log";
    std::string smoke_test_log_file = "smoke_test.log";  // set by main to logs/ timestamped path
    int64_t metrics_dump_interval_ms = 5000;
    std::string log_level = "INFO";

    // Scheduler M2 settings
    bool scheduler_enabled = true;
    int scheduler_poll_strategy = 2;  // 0=spin, 1=sleep, 2=hybrid
    int64_t scheduler_sleep_us = 100;
    int max_user_events_per_pass = 256;
    int max_exec_events_per_pass = 256;
    int max_market_events_per_pass = 256;
    int max_control_events_per_pass = 64;
    int max_binance_md_events_per_pass = 256;
    bool strategy_stub_emit_intents = false;
    bool exec_feedback_loop_enabled = true;  // M2 self-feedback; disable when real T3 exists
    std::size_t user_queue_capacity = 4096;
    std::size_t exec_queue_capacity = 4096;
    std::size_t control_queue_capacity = 1024;

    // Book delta queue (T0->T2 shadow books)
    std::size_t book_delta_queue_capacity = 8192;

    // User WS M3 settings
    bool user_ws_enabled = false;
    std::string user_ws_endpoint = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
    std::vector<std::string> user_ws_markets;  // condition IDs
    int64_t user_ws_stale_threshold_ms = 30000;
    int64_t user_ws_reconnect_base_ms = 1000;
    int64_t user_ws_reconnect_max_ms = 30000;
    int64_t user_ws_ping_interval_ms = 10000;
    int user_ws_max_auth_failures = 3;
    int user_ws_redundancy = 1;          // parallel user WS connections (1-20)
    int64_t user_ws_redundancy_stagger_ms = 200;

    // Execution Gateway M4 settings
    bool gateway_enabled = false;
    std::string rest_base_url = "https://clob.polymarket.com";
    std::string rest_host = "clob.polymarket.com";
    std::string rest_port = "443";
    int64_t rest_request_timeout_ms = 5000;
    int rest_pipeline_depth = 4;
    std::size_t strategy_to_exec_capacity = 4096;
    // Rate limiting
    int rate_limit_global_per_10s = 9000;
    int rate_limit_order_per_10s = 3500;
    int rate_limit_cancel_per_10s = 3000;
    int64_t rate_limit_backoff_base_ms = 500;
    int64_t rate_limit_backoff_max_ms = 30000;
    // Heartbeat
    int64_t heartbeat_interval_ms = 5000;
    int heartbeat_max_failures = 3;
    bool heartbeat_cancel_on_failure = false;
    // Order defaults
    uint8_t default_signature_type = 0;
    bool default_defer_exec = false;
    bool default_post_only = false;
    // Batch orders
    bool batch_orders_enabled = false;
    int batch_max_size = 15;
    // Redundant cancel connections
    int cancel_connection_redundancy = 1;
    // HTTP/2 transport
    bool rest_use_http2 = false;
    int rest_max_concurrent_streams = 100;
    int order_connection_pool_size = 3;

    // Strategy selection
    std::string strategy_type = "quoter_v2";  // "quoter_v2", "test", or "inventory_test"

    // QuoterV2 strategy settings (used when strategy_type == "quoter_v2")
    int quoter_v2_offset = 300;                  // half-spread from FV (fp4)
    int quoter_v2_skew_strength = 20;            // price shift per share of delta (fp4/share)
    int quoter_v2_max_skew = 200;                // skew clamp (fp4)
    int quoter_v2_max_inventory = 50;            // emergency flatten trigger (shares)
    int quoter_v2_quote_size = 20;               // size per side per token (shares)
    int quoter_v2_min_order_size = 5;            // min placement size (shares)
    int quoter_v2_emergency_qty = 10;            // FAK emergency size (shares)
    int quoter_v2_price_floor = 900;             // stop quoting below (fp4, 0.09)
    int quoter_v2_price_ceiling = 9100;          // stop quoting above (fp4, 0.91)
    int quoter_v2_initial_split_size = 200;      // split on market start (shares)
    int quoter_v2_inventory_low_water = 100;     // replenish when either token below this (shares)
    int quoter_v2_inventory_replenish_size = 100;// amount to split when replenishing (shares)
    int quoter_v2_inventory_merge_threshold = 250; // merge when both tokens above this (shares)
    int quoter_v2_inventory_merge_size = 50;     // amount to merge (shares)
    int64_t quoter_v2_inventory_cooldown_ms = 30000;  // cooldown between inventory ops
    int quoter_v2_max_replaces_per_second = 5;   // churn limiter
    int64_t quoter_v2_min_quote_lifetime_ms = 500;    // min time before replace
    int64_t quoter_v2_degraded_refresh_ms = 5000;     // widen interval when degraded
    bool quoter_v2_deterministic_timing = false;      // false=real clock for live, true=logical clock for replay
    // V2.1: Three-tier inventory + gamma + FAK rework
    int quoter_v2_soft_max_inventory = 20;
    int quoter_v2_hard_max_inventory = 50;
    int64_t quoter_v2_market_duration_ms = 300000;
    int64_t quoter_v2_hard_cutoff_ms = 12000;
    int quoter_v2_gamma_power_x1000 = 1200;
    int quoter_v2_offset_growth_fp4 = 100;
    int quoter_v2_time_floor_mult_x1000 = 3000;
    int64_t quoter_v2_time_floor_threshold_ms = 20000;
    int64_t quoter_v2_fak_cooldown_ms = 200;
    int quoter_v2_fak_gamma_floor_x1000 = 0;

    // M5 Risk settings
    int execution_mode = 0;                    // 0=DRY_RUN, 1=LIVE
    int risk_max_position_per_token = 50;      // max position per token (lots)
    int risk_max_net_exposure_per_market = 50;  // max net exposure per market (lots)
    int64_t risk_max_notional = 500000;         // max total working notional (price * qty)
    int64_t risk_max_loss = 100000;             // max unrealized loss
    bool risk_cancel_all_on_violation = false;  // cancel all on any risk violation

    // Inventory service (dedicated non-hot-path worker)
    bool inventory_service_enabled = false;
    bool inventory_service_dry_run = true;
    std::string inventory_command_template = "";
    int64_t inventory_service_poll_sleep_ms = 10;
    std::size_t inventory_queue_capacity = 256;

    // Relayer config (inventory service -> Polymarket Relayer v2)
    std::string relayer_host = "relayer-v2.polymarket.com";
    std::string relayer_port = "443";
    int64_t relayer_poll_interval_ms = 5000;
    int relayer_max_poll_attempts = 60;
    // Direct RPC for inventory (replaces relayer when enabled)
    bool inventory_use_direct_rpc = false;
    int64_t max_gas_price_gwei = 100;
    uint64_t inventory_gas_limit = 500000;
    int inventory_rpc_timeout_ms = 10000;

    // M6 UI Bridge settings
    bool ui_bridge_enabled = false;
    int ui_ws_port = 9090;
    std::string ui_ws_bind_address = "0.0.0.0";
    std::string ui_ws_auth_token = "";
    int ui_snapshot_rate_hz = 20;
    int ui_book_depth = 10;
    std::size_t ui_book_queue_capacity = 256;
    std::size_t ui_state_queue_capacity = 256;

    // Rotation settings (legacy single-timeframe mode)
    bool rotation_enabled = false;
    int rotation_timeframe = 0;                    // 0=BTC_5M, 1=BTC_15M
    int64_t rotation_discovery_poll_ms = 15000;    // How often to poll for next market
    int64_t rotation_pre_rotation_ms = 5000;       // Start rotation this long before window end
    int64_t rotation_no_trade_start_ms = 0;         // No-trade zone at window start (ms, 0=disabled)
    int64_t rotation_no_trade_end_ms = 0;           // No-trade zone before window end (ms, 0=disabled)

    // Slot manager settings (multi-timeframe, replaces rotation)
    bool slot_manager_enabled = false;
    int64_t slot_pre_subscribe_ms = 150000;        // Subscribe to next market this early (2.5 min)
    int64_t slot_cancel_lead_ms = 1000;            // Cancel orders this early before window end
    int64_t slot_no_trade_start_ms = 0;            // No-trade zone at window start
    int64_t slot_no_trade_end_ms = 0;              // No-trade zone before window end
    bool slot_enable_5m = true;                    // Trade 5-minute BTC markets
    bool slot_enable_15m = true;                   // Trade 15-minute BTC markets

    // Watcher settings (UI-only, no hot-path impact)
    bool watcher_enabled = false;
    int64_t watcher_discovery_poll_ms = 30000;     // REST discovery interval
    int64_t watcher_status_poll_ms = 10000;        // REST market status interval
    int64_t watcher_stale_threshold_ms = 15000;    // WS data staleness threshold
    int ladder_update_interval_ms = 100;           // Ladder snapshot throttle
    int ladder_max_depth = 200;                    // Max levels per side in ladder
    std::string discovery_api_url = "https://gamma-api.polymarket.com";
    std::string clob_api_url = "https://clob.polymarket.com";

    // Thread pinning (-1 = don't pin)
    bool pin_threads = true;
    int pin_core_t0 = 0;        // Market WS
    int pin_core_t1 = 1;        // User WS
    int pin_core_t2 = 2;        // Strategy scheduler (most latency-sensitive)
    int pin_core_t3 = 3;        // Execution gateway
    int pin_core_t6 = 4;        // IPC bridge
    int pin_core_rtds = 5;      // RTDS WebSocket
    int pin_core_rec = 6;       // Data recorder

    // Account balance polling
    int account_poll_interval_ms = 15000;
    int64_t initial_usdc_balance = 0;  // micro-USDC (fp6), e.g. 1000 USDC = 1000000000
    std::string polygon_rpc_url;       // Polygon RPC for on-chain balance fetch (empty = skip)
    int balance_poll_interval_ms = 1000;  // on-chain USDC balance poll interval

    // RTDS (Real-Time Data Socket) settings
    bool rtds_enabled = false;
    std::string rtds_endpoint = "wss://ws-live-data.polymarket.com";
    int64_t rtds_ping_interval_ms = 5000;
    int64_t rtds_reconnect_base_ms = 1000;
    int64_t rtds_reconnect_max_ms = 30000;
    int64_t rtds_stale_threshold_ms = 15000;
    std::size_t rtds_queue_capacity = 4096;

    // Binance Spot market-data WebSocket settings
    bool binance_md_enabled = false;
    std::string binance_md_endpoint = "wss://stream.binance.com:9443";
    // Comma-separated stream list (lowercase symbol @ stream).
    // Default: BTC bookTicker + trade (lowest-latency BTC price/flow signal).
    std::string binance_md_streams = "btcusdt@bookTicker,btcusdt@trade";
    int64_t binance_md_reconnect_base_ms = 1000;
    int64_t binance_md_reconnect_max_ms = 30000;
    int64_t binance_md_stale_threshold_ms = 60000;
    int64_t binance_md_rotate_interval_ms = 82800000;  // 23h
    std::size_t binance_md_queue_capacity = 4096;
    int pin_core_binance_md = 7;

    // Data recording settings
    bool recording_enabled = false;
    std::string recording_output_dir = "data";
    int64_t recording_flush_interval_ms = 1000;
    int64_t recording_min_disk_space_mb = 500;
    std::size_t recording_queue_capacity = 8192;  // per-source queue capacity

    // Trade journal settings
    bool journal_enabled = false;
    int journal_level = 1;                         // 0=lifecycle only, 1=full (strategy/risk)
    std::size_t journal_queue_capacity = 8192;

};

Expected<EngineConfig> load_config(const std::string& path);

}  // namespace lt
