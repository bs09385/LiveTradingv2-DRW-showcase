#include "common/config.h"

#include <fstream>
#include <sstream>

#include "simdjson.h"

namespace lt {

Expected<EngineConfig> load_config(const std::string& path) {
    EngineConfig cfg;

    std::ifstream file(path);
    if (!file.is_open()) {
        return ErrorCode::CONFIG_FILE_NOT_FOUND;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string json_str = ss.str();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(json_str).get(doc);
    if (err) {
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    // Helper to read optional string
    auto read_str = [&](const char* key, std::string& out) {
        std::string_view sv;
        if (doc[key].get_string().get(sv) == simdjson::SUCCESS) {
            out = std::string(sv);
        }
    };

    // Helper to read optional int
    auto read_int = [&](const char* key, int64_t& out) {
        int64_t val;
        if (doc[key].get_int64().get(val) == simdjson::SUCCESS) {
            out = val;
        }
    };

    auto read_uint = [&](const char* key, std::size_t& out) {
        uint64_t val;
        if (doc[key].get_uint64().get(val) == simdjson::SUCCESS) {
            out = static_cast<std::size_t>(val);
        }
    };

    read_str("ws_endpoint", cfg.ws_endpoint);
    read_int("ping_interval_ms", cfg.ping_interval_ms);
    read_int("pong_timeout_ms", cfg.pong_timeout_ms);
    read_int("reconnect_base_ms", cfg.reconnect_base_ms);
    read_int("reconnect_max_ms", cfg.reconnect_max_ms);
    { int64_t v = cfg.ws_redundancy; read_int("ws_redundancy", v); cfg.ws_redundancy = static_cast<int>(v); }
    read_int("ws_redundancy_stagger_ms", cfg.ws_redundancy_stagger_ms);
    read_uint("strategy_queue_capacity", cfg.strategy_queue_capacity);
    read_uint("log_queue_capacity", cfg.log_queue_capacity);
    read_int("scheduler_stats_interval_ms", cfg.scheduler_stats_interval_ms);
    read_str("log_file", cfg.log_file);
    read_str("metrics_file", cfg.metrics_file);
    read_int("metrics_dump_interval_ms", cfg.metrics_dump_interval_ms);
    read_str("log_level", cfg.log_level);

    // Scheduler M2 settings
    auto read_bool = [&](const char* key, bool& out) {
        bool val;
        if (doc[key].get_bool().get(val) == simdjson::SUCCESS) {
            out = val;
        }
    };

    auto read_int32 = [&](const char* key, int& out) {
        int64_t val;
        if (doc[key].get_int64().get(val) == simdjson::SUCCESS) {
            if (val < static_cast<int64_t>(INT32_MIN) ||
                val > static_cast<int64_t>(INT32_MAX)) {
                // Value out of int32 range -- keep default
                return;
            }
            out = static_cast<int>(val);
        }
    };

    read_bool("scheduler_enabled", cfg.scheduler_enabled);
    read_int32("scheduler_poll_strategy", cfg.scheduler_poll_strategy);
    read_int("scheduler_sleep_us", cfg.scheduler_sleep_us);
    read_int32("max_user_events_per_pass", cfg.max_user_events_per_pass);
    read_int32("max_exec_events_per_pass", cfg.max_exec_events_per_pass);
    read_int32("max_market_events_per_pass", cfg.max_market_events_per_pass);
    read_int32("max_control_events_per_pass", cfg.max_control_events_per_pass);
    read_int32("max_binance_md_events_per_pass", cfg.max_binance_md_events_per_pass);
    read_bool("strategy_stub_emit_intents", cfg.strategy_stub_emit_intents);
    read_bool("exec_feedback_loop_enabled", cfg.exec_feedback_loop_enabled);
    read_uint("book_delta_queue_capacity", cfg.book_delta_queue_capacity);
    read_uint("user_queue_capacity", cfg.user_queue_capacity);
    read_uint("exec_queue_capacity", cfg.exec_queue_capacity);
    read_uint("control_queue_capacity", cfg.control_queue_capacity);

    // Read asset_ids array
    simdjson::dom::array arr;
    if (doc["asset_ids"].get_array().get(arr) == simdjson::SUCCESS) {
        for (auto elem : arr) {
            std::string_view sv;
            if (elem.get_string().get(sv) == simdjson::SUCCESS) {
                cfg.asset_ids.emplace_back(sv);
            }
        }
    }

    // Read market_pairs array
    simdjson::dom::array market_pairs_arr;
    if (doc["market_pairs"].get_array().get(market_pairs_arr) == simdjson::SUCCESS) {
        for (auto elem : market_pairs_arr) {
            simdjson::dom::object obj;
            if (elem.get_object().get(obj) != simdjson::SUCCESS) {
                continue;
            }

            std::string_view condition_id_sv;
            std::string_view token_id_up_sv;
            std::string_view token_id_down_sv;
            if (obj["condition_id"].get_string().get(condition_id_sv) != simdjson::SUCCESS) {
                continue;
            }
            if (obj["token_id_up"].get_string().get(token_id_up_sv) != simdjson::SUCCESS) {
                continue;
            }
            if (obj["token_id_down"].get_string().get(token_id_down_sv) != simdjson::SUCCESS) {
                continue;
            }

            MarketPairConfig pair_cfg;
            pair_cfg.condition_id = std::string(condition_id_sv);
            pair_cfg.token_id_up = std::string(token_id_up_sv);
            pair_cfg.token_id_down = std::string(token_id_down_sv);
            cfg.market_pairs.push_back(std::move(pair_cfg));
        }
    }

    // User WS M3 settings
    read_bool("user_ws_enabled", cfg.user_ws_enabled);
    read_str("user_ws_endpoint", cfg.user_ws_endpoint);
    read_int("user_ws_stale_threshold_ms", cfg.user_ws_stale_threshold_ms);
    read_int("user_ws_reconnect_base_ms", cfg.user_ws_reconnect_base_ms);
    read_int("user_ws_reconnect_max_ms", cfg.user_ws_reconnect_max_ms);
    read_int("user_ws_ping_interval_ms", cfg.user_ws_ping_interval_ms);
    read_int32("user_ws_max_auth_failures", cfg.user_ws_max_auth_failures);
    read_int32("user_ws_redundancy", cfg.user_ws_redundancy);
    read_int("user_ws_redundancy_stagger_ms", cfg.user_ws_redundancy_stagger_ms);

    // Read user_ws_markets array
    simdjson::dom::array markets_arr;
    if (doc["user_ws_markets"].get_array().get(markets_arr) == simdjson::SUCCESS) {
        for (auto elem : markets_arr) {
            std::string_view sv;
            if (elem.get_string().get(sv) == simdjson::SUCCESS) {
                cfg.user_ws_markets.emplace_back(sv);
            }
        }
    }

    // Execution Gateway M4 settings
    read_bool("gateway_enabled", cfg.gateway_enabled);
    read_str("rest_base_url", cfg.rest_base_url);
    read_str("rest_host", cfg.rest_host);
    read_str("rest_port", cfg.rest_port);
    read_int("rest_request_timeout_ms", cfg.rest_request_timeout_ms);
    read_int32("rest_pipeline_depth", cfg.rest_pipeline_depth);
    read_uint("strategy_to_exec_capacity", cfg.strategy_to_exec_capacity);
    read_int32("rate_limit_global_per_10s", cfg.rate_limit_global_per_10s);
    read_int32("rate_limit_order_per_10s", cfg.rate_limit_order_per_10s);
    read_int32("rate_limit_cancel_per_10s", cfg.rate_limit_cancel_per_10s);
    read_int("rate_limit_backoff_base_ms", cfg.rate_limit_backoff_base_ms);
    read_int("rate_limit_backoff_max_ms", cfg.rate_limit_backoff_max_ms);
    read_int("heartbeat_interval_ms", cfg.heartbeat_interval_ms);
    read_int32("heartbeat_max_failures", cfg.heartbeat_max_failures);
    read_bool("heartbeat_cancel_on_failure", cfg.heartbeat_cancel_on_failure);

    auto read_uint8 = [&](const char* key, uint8_t& out) {
        int64_t val;
        if (doc[key].get_int64().get(val) == simdjson::SUCCESS) {
            out = static_cast<uint8_t>(val);
        }
    };
    read_uint8("default_signature_type", cfg.default_signature_type);
    read_bool("default_defer_exec", cfg.default_defer_exec);
    read_bool("default_post_only", cfg.default_post_only);
    read_bool("batch_orders_enabled", cfg.batch_orders_enabled);
    read_int32("batch_max_size", cfg.batch_max_size);
    read_int32("cancel_connection_redundancy", cfg.cancel_connection_redundancy);
    read_bool("rest_use_http2", cfg.rest_use_http2);
    read_int32("rest_max_concurrent_streams", cfg.rest_max_concurrent_streams);
    read_int32("order_connection_pool_size", cfg.order_connection_pool_size);

    // Strategy selection
    read_str("strategy_type", cfg.strategy_type);

    // QuoterV2 strategy settings
    read_int32("quoter_v2_offset", cfg.quoter_v2_offset);
    read_int32("quoter_v2_skew_strength", cfg.quoter_v2_skew_strength);
    read_int32("quoter_v2_max_skew", cfg.quoter_v2_max_skew);
    read_int32("quoter_v2_max_inventory", cfg.quoter_v2_max_inventory);
    read_int32("quoter_v2_quote_size", cfg.quoter_v2_quote_size);
    read_int32("quoter_v2_min_order_size", cfg.quoter_v2_min_order_size);
    read_int32("quoter_v2_emergency_qty", cfg.quoter_v2_emergency_qty);
    read_int32("quoter_v2_price_floor", cfg.quoter_v2_price_floor);
    read_int32("quoter_v2_price_ceiling", cfg.quoter_v2_price_ceiling);
    read_int32("quoter_v2_initial_split_size", cfg.quoter_v2_initial_split_size);
    read_int32("quoter_v2_inventory_low_water", cfg.quoter_v2_inventory_low_water);
    read_int32("quoter_v2_inventory_replenish_size", cfg.quoter_v2_inventory_replenish_size);
    read_int32("quoter_v2_inventory_merge_threshold", cfg.quoter_v2_inventory_merge_threshold);
    read_int32("quoter_v2_inventory_merge_size", cfg.quoter_v2_inventory_merge_size);
    read_int("quoter_v2_inventory_cooldown_ms", cfg.quoter_v2_inventory_cooldown_ms);
    read_int32("quoter_v2_max_replaces_per_second", cfg.quoter_v2_max_replaces_per_second);
    read_int("quoter_v2_min_quote_lifetime_ms", cfg.quoter_v2_min_quote_lifetime_ms);
    read_int("quoter_v2_degraded_refresh_ms", cfg.quoter_v2_degraded_refresh_ms);
    read_bool("quoter_v2_deterministic_timing", cfg.quoter_v2_deterministic_timing);
    // V2.1: Three-tier inventory + gamma + FAK rework
    read_int32("quoter_v2_soft_max_inventory", cfg.quoter_v2_soft_max_inventory);
    read_int32("quoter_v2_hard_max_inventory", cfg.quoter_v2_hard_max_inventory);
    read_int("quoter_v2_market_duration_ms", cfg.quoter_v2_market_duration_ms);
    read_int("quoter_v2_hard_cutoff_ms", cfg.quoter_v2_hard_cutoff_ms);
    read_int32("quoter_v2_gamma_power_x1000", cfg.quoter_v2_gamma_power_x1000);
    read_int32("quoter_v2_offset_growth_fp4", cfg.quoter_v2_offset_growth_fp4);
    read_int32("quoter_v2_time_floor_mult_x1000", cfg.quoter_v2_time_floor_mult_x1000);
    read_int("quoter_v2_time_floor_threshold_ms", cfg.quoter_v2_time_floor_threshold_ms);
    read_int("quoter_v2_fak_cooldown_ms", cfg.quoter_v2_fak_cooldown_ms);
    read_int32("quoter_v2_fak_gamma_floor_x1000", cfg.quoter_v2_fak_gamma_floor_x1000);

    // M5 Risk settings
    read_int32("execution_mode", cfg.execution_mode);
    read_int32("risk_max_position_per_token", cfg.risk_max_position_per_token);
    read_int32("risk_max_net_exposure_per_market", cfg.risk_max_net_exposure_per_market);
    read_int("risk_max_notional", cfg.risk_max_notional);
    read_int("risk_max_loss", cfg.risk_max_loss);
    read_bool("risk_cancel_all_on_violation", cfg.risk_cancel_all_on_violation);

    // Inventory service
    read_bool("inventory_service_enabled", cfg.inventory_service_enabled);
    read_bool("inventory_service_dry_run", cfg.inventory_service_dry_run);
    read_str("inventory_command_template", cfg.inventory_command_template);
    read_int("inventory_service_poll_sleep_ms", cfg.inventory_service_poll_sleep_ms);
    read_uint("inventory_queue_capacity", cfg.inventory_queue_capacity);

    // Relayer config
    read_str("relayer_host", cfg.relayer_host);
    read_str("relayer_port", cfg.relayer_port);
    read_int("relayer_poll_interval_ms", cfg.relayer_poll_interval_ms);
    read_int32("relayer_max_poll_attempts", cfg.relayer_max_poll_attempts);
    read_bool("inventory_use_direct_rpc", cfg.inventory_use_direct_rpc);
    read_int("max_gas_price_gwei", cfg.max_gas_price_gwei);
    { int64_t v = static_cast<int64_t>(cfg.inventory_gas_limit);
      read_int("inventory_gas_limit", v);
      cfg.inventory_gas_limit = static_cast<uint64_t>(v); }
    read_int32("inventory_rpc_timeout_ms", cfg.inventory_rpc_timeout_ms);

    // M6 UI Bridge settings
    read_bool("ui_bridge_enabled", cfg.ui_bridge_enabled);
    read_int32("ui_ws_port", cfg.ui_ws_port);
    read_str("ui_ws_bind_address", cfg.ui_ws_bind_address);
    read_str("ui_ws_auth_token", cfg.ui_ws_auth_token);
    read_int32("ui_snapshot_rate_hz", cfg.ui_snapshot_rate_hz);
    read_int32("ui_book_depth", cfg.ui_book_depth);
    read_uint("ui_book_queue_capacity", cfg.ui_book_queue_capacity);
    read_uint("ui_state_queue_capacity", cfg.ui_state_queue_capacity);

    // Rotation settings (legacy)
    read_bool("rotation_enabled", cfg.rotation_enabled);
    read_int32("rotation_timeframe", cfg.rotation_timeframe);
    read_int("rotation_discovery_poll_ms", cfg.rotation_discovery_poll_ms);
    read_int("rotation_pre_rotation_ms", cfg.rotation_pre_rotation_ms);
    read_int("rotation_no_trade_start_ms", cfg.rotation_no_trade_start_ms);
    read_int("rotation_no_trade_end_ms", cfg.rotation_no_trade_end_ms);

    // Slot manager settings (multi-timeframe)
    read_bool("slot_manager_enabled", cfg.slot_manager_enabled);
    read_int("slot_pre_subscribe_ms", cfg.slot_pre_subscribe_ms);
    read_int("slot_cancel_lead_ms", cfg.slot_cancel_lead_ms);
    read_int("slot_no_trade_start_ms", cfg.slot_no_trade_start_ms);
    read_int("slot_no_trade_end_ms", cfg.slot_no_trade_end_ms);
    read_bool("slot_enable_5m", cfg.slot_enable_5m);
    read_bool("slot_enable_15m", cfg.slot_enable_15m);

    // Watcher settings
    read_bool("watcher_enabled", cfg.watcher_enabled);
    read_int("watcher_discovery_poll_ms", cfg.watcher_discovery_poll_ms);
    read_int("watcher_status_poll_ms", cfg.watcher_status_poll_ms);
    read_int("watcher_stale_threshold_ms", cfg.watcher_stale_threshold_ms);
    read_int32("ladder_update_interval_ms", cfg.ladder_update_interval_ms);
    read_int32("ladder_max_depth", cfg.ladder_max_depth);
    read_str("discovery_api_url", cfg.discovery_api_url);
    read_str("clob_api_url", cfg.clob_api_url);

    // Thread pinning
    read_bool("pin_threads", cfg.pin_threads);
    read_int32("pin_core_t0", cfg.pin_core_t0);
    read_int32("pin_core_t1", cfg.pin_core_t1);
    read_int32("pin_core_t2", cfg.pin_core_t2);
    read_int32("pin_core_t3", cfg.pin_core_t3);
    read_int32("pin_core_t6", cfg.pin_core_t6);
    read_int32("pin_core_rtds", cfg.pin_core_rtds);
    read_int32("pin_core_rec", cfg.pin_core_rec);

    // Account balance polling
    read_int32("account_poll_interval_ms", cfg.account_poll_interval_ms);
    read_int("initial_usdc_balance", cfg.initial_usdc_balance);
    read_str("polygon_rpc_url", cfg.polygon_rpc_url);
    read_int32("balance_poll_interval_ms", cfg.balance_poll_interval_ms);

    // RTDS settings
    read_bool("rtds_enabled", cfg.rtds_enabled);
    read_str("rtds_endpoint", cfg.rtds_endpoint);
    read_int("rtds_ping_interval_ms", cfg.rtds_ping_interval_ms);
    read_int("rtds_reconnect_base_ms", cfg.rtds_reconnect_base_ms);
    read_int("rtds_reconnect_max_ms", cfg.rtds_reconnect_max_ms);
    read_int("rtds_stale_threshold_ms", cfg.rtds_stale_threshold_ms);
    read_uint("rtds_queue_capacity", cfg.rtds_queue_capacity);

    // Binance market-data settings
    read_bool("binance_md_enabled", cfg.binance_md_enabled);
    read_str("binance_md_endpoint", cfg.binance_md_endpoint);
    read_str("binance_md_streams", cfg.binance_md_streams);
    read_int("binance_md_reconnect_base_ms", cfg.binance_md_reconnect_base_ms);
    read_int("binance_md_reconnect_max_ms", cfg.binance_md_reconnect_max_ms);
    read_int("binance_md_stale_threshold_ms", cfg.binance_md_stale_threshold_ms);
    read_int("binance_md_rotate_interval_ms", cfg.binance_md_rotate_interval_ms);
    read_uint("binance_md_queue_capacity", cfg.binance_md_queue_capacity);
    read_int32("pin_core_binance_md", cfg.pin_core_binance_md);

    // Data recording settings
    read_bool("recording_enabled", cfg.recording_enabled);
    read_str("recording_output_dir", cfg.recording_output_dir);
    read_int("recording_flush_interval_ms", cfg.recording_flush_interval_ms);
    read_int("recording_min_disk_space_mb", cfg.recording_min_disk_space_mb);
    read_uint("recording_queue_capacity", cfg.recording_queue_capacity);

    // Trade journal settings
    read_bool("journal_enabled", cfg.journal_enabled);
    read_int32("journal_level", cfg.journal_level);
    read_uint("journal_queue_capacity", cfg.journal_queue_capacity);

    return Expected<EngineConfig>(std::move(cfg));
}

}  // namespace lt
