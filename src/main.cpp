#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

#include "common/account.h"
#include "common/balance_fetcher.h"
#include "common/position_bootstrap.h"
#include "common/clock.h"
#include "common/config.h"
#include "common/thread_pin.h"
#include "common/derive_api_key.h"
#include "common/market_pair.h"
#include "common/pnl_tracker.h"
#include "common/token_inventory.h"
#include "events/book_delta.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "parser/market_message_parser.h"
#include "parser/user_message_parser.h"
#include "queue/spsc_queue.h"
#include "rotation/rotation_coordinator.h"
#include "slot/market_slot_manager.h"
#include "scheduler/execution_mode.h"
#include "scheduler/mode_filtered_sink.h"
#include "scheduler/risk_gate.h"
#include "scheduler/strategy_scheduler.h"
#include "scheduler/inventory_test_strategy.h"
#include "scheduler/test_strategy.h"
#include "scheduler/quoter_v2_strategy.h"
#include "scheduler/working_order_tracker.h"
#include "state/exec_state_store.h"
#include "state/market_state_store.h"
#include "ws/market_ws_client.h"
#include "ws/user_ws_client.h"
#include "exec/exec_intent.h"
#include "exec/exec_queue_sink.h"
#include "exec/execution_gateway.h"
#include "inventory/inventory_queue_sink.h"
#include "inventory/inventory_service.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"
#include "ui_bridge/ipc_bridge.h"
#include "ui_bridge/ui_types.h"
#include "rtds/rtds_types.h"
#include "rtds/rtds_message_parser.h"
#include "rtds/rtds_ws_client.h"
#include "binance/binance_types.h"
#include "binance/binance_message_parser.h"
#include "binance/binance_ws_client.h"
#include "recorder/data_recorder.h"
#include "recorder/journal_writer.h"

namespace {
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_fatal{false};
}  // namespace

void signal_handler(int sig) {
    (void)sig;
    g_shutdown.store(true, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    // Load config
    std::string config_path = "config/default.json";
    if (argc > 1) config_path = argv[1];

    auto cfg_result = lt::load_config(config_path);
    if (!cfg_result.ok()) {
        if (argc > 1) {
            // Explicit config path provided but failed to load -- fatal
            std::cerr << "FATAL: Failed to load config from: " << config_path << "\n";
            return 1;
        }
        std::cerr << "Failed to load config from: " << config_path << "\n";
        std::cerr << "Using defaults.\n";
    }
    auto cfg = cfg_result.ok() ? cfg_result.value : lt::EngineConfig{};

    // Load optional account file (second CLI arg)
    std::optional<lt::AccountInfo> account;
    if (argc > 2) {
        std::string account_path = argv[2];
        auto acct_result = lt::load_account(account_path);
        if (!acct_result.ok()) {
            std::cerr << "FATAL: Failed to load account from: " << account_path << "\n";
            return 1;
        }
        account = std::move(acct_result.value);
        std::cout << "Account: " << account->name << " (" << account->address << ")\n";

        // Auto-derive API credentials if missing
        if (account->api_key.empty() || account->api_secret.empty() ||
            account->api_passphrase.empty()) {
            std::cout << "  Deriving API credentials...\n";

            // Decode private key
            lt::Bytes32 pk_bytes{};
            if (!lt::hex_decode_to_bytes32(account->private_key, pk_bytes)) {
                std::cerr << "FATAL: Invalid private_key hex in account file\n";
                return 1;
            }

            auto derive_result = lt::derive_api_key(pk_bytes.data(), account->address);
            pk_bytes.fill(0);

            if (derive_result.ok()) {
                account->api_key = derive_result.value.api_key;
                account->api_secret = derive_result.value.api_secret;
                account->api_passphrase = derive_result.value.api_passphrase;

                // Write back to account file
                auto save_err = lt::save_account(account_path, *account);
                if (save_err == lt::ErrorCode::OK) {
                    std::cout << "  Derived API credentials saved to " << account_path << "\n";
                } else {
                    std::cerr << "  Warning: Could not save derived credentials to file\n";
                }
            } else {
                std::cerr << "  Warning: Could not derive API credentials (error "
                          << static_cast<int>(derive_result.error)
                          << "). Gateway auth may fail.\n";
            }
        }
    }

    // Create logs/ directory and generate timestamped log paths
    {
        std::filesystem::create_directories("logs");
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &tt);
#else
        localtime_r(&tt, &tm_buf);
#endif
        char ts[20];
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_buf);
        cfg.log_file = std::string("logs/engine_") + ts + ".log";
        cfg.metrics_file = std::string("logs/metrics_") + ts + ".log";
        cfg.smoke_test_log_file = std::string("logs/smoke_test_") + ts + ".log";
    }

    std::cout << "LiveTradingv2 Engine starting...\n";
    std::cout << "  Endpoint: " << cfg.ws_endpoint << "\n";
    std::cout << "  Queue capacity: " << cfg.strategy_queue_capacity << "\n";
    std::cout << "  Scheduler enabled: " << (cfg.scheduler_enabled ? "yes" : "no") << "\n";
    std::cout << "  User WS enabled: " << (cfg.user_ws_enabled ? "yes" : "no") << "\n";
    std::cout << "  Gateway enabled: " << (cfg.gateway_enabled ? "yes" : "no") << "\n";
    std::cout << "  Execution mode: " << lt::execution_mode_name(
        static_cast<lt::ExecutionMode>(cfg.execution_mode)) << "\n";
    std::cout << "  UI bridge enabled: " << (cfg.ui_bridge_enabled ? "yes" : "no") << "\n";
    std::cout << "  Strategy type: " << cfg.strategy_type << "\n";
    std::cout << "  Slot manager enabled: " << (cfg.slot_manager_enabled ? "yes" : "no") << "\n";
    std::cout << "  Rotation enabled (legacy): " << (cfg.rotation_enabled ? "yes" : "no") << "\n";
    std::cout << "  RTDS enabled: " << (cfg.rtds_enabled ? "yes" : "no") << "\n";
    std::cout << "  Recording enabled: " << (cfg.recording_enabled ? "yes" : "no") << "\n";
    std::cout << "  Journal enabled: " << (cfg.journal_enabled ? "yes" : "no") << "\n";

    // Create metrics
    lt::Metrics metrics;

    // Create async logger
    lt::AsyncLogger logger(cfg.log_file, cfg.log_queue_capacity,
                           lt::parse_log_level(cfg.log_level));
    auto main_log = logger.create_producer("main");

    // Build market pair registry from config metadata.
    lt::MarketPairRegistry market_pair_registry;
    for (const auto& pair_cfg : cfg.market_pairs) {
        bool added = market_pair_registry.add_pair(
            lt::AssetId(pair_cfg.condition_id),
            lt::AssetId(pair_cfg.token_id_up),
            lt::AssetId(pair_cfg.token_id_down));
        if (!added) {
            char buf[lt::LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                          "Invalid/duplicate market pair in config "
                          "(condition=%s token_up=%s token_down=%s)",
                          pair_cfg.condition_id.c_str(),
                          pair_cfg.token_id_up.c_str(),
                          pair_cfg.token_id_down.c_str());
            lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
        }
    }

    auto add_unique_id = [](std::vector<std::string>& out, std::string_view id) {
        for (const auto& existing : out) {
            if (existing == id) return;
        }
        out.emplace_back(id);
    };

    // Market WS subscribes by token IDs. If market_pairs are provided, include both tokens.
    std::vector<std::string> market_ws_asset_ids = cfg.asset_ids;
    for (const auto& pair_cfg : cfg.market_pairs) {
        add_unique_id(market_ws_asset_ids, pair_cfg.token_id_up);
        add_unique_id(market_ws_asset_ids, pair_cfg.token_id_down);
    }

    // User WS subscribes by condition IDs. If empty, derive from market_pairs when available.
    std::vector<std::string> user_ws_market_ids = cfg.user_ws_markets;
    if (user_ws_market_ids.empty()) {
        for (const auto& pair_cfg : cfg.market_pairs) {
            add_unique_id(user_ws_market_ids, pair_cfg.condition_id);
        }
    }

    // Shared inventory view used by planner (T2) and execution safety checks (T3).
    // Pre-register all known token IDs so the map is structurally frozen before
    // threads start (lock-free atomics require immutable map structure).
    auto token_inventory = std::make_unique<lt::TokenInventory>();
    for (const auto& pair_cfg : cfg.market_pairs) {
        token_inventory->register_token(lt::AssetId(pair_cfg.token_id_up));
        token_inventory->register_token(lt::AssetId(pair_cfg.token_id_down));
    }
    for (const auto& aid : cfg.asset_ids) {
        token_inventory->register_token(lt::AssetId(aid));
    }

    // Fetch USDC balance from Polygon RPC, fall back to config value.
    if (!cfg.polygon_rpc_url.empty() && account) {
        std::string balance_addr = account->safe_address.empty()
                                       ? account->address
                                       : account->safe_address;
        std::cout << "Fetching USDC balance for " << balance_addr << "...\n";
        auto bal = lt::fetch_usdc_balance(cfg.polygon_rpc_url, balance_addr);
        if (bal.ok()) {
            token_inventory->set_usdc_balance(bal.value);
            std::printf("  USDC balance: %.2f (on-chain)\n",
                        static_cast<double>(bal.value) / 1000000.0);
        } else {
            std::fprintf(stderr, "  Balance fetch failed, using config fallback\n");
            if (cfg.initial_usdc_balance > 0) {
                token_inventory->set_usdc_balance(cfg.initial_usdc_balance);
            }
        }
    } else if (cfg.initial_usdc_balance > 0) {
        token_inventory->set_usdc_balance(cfg.initial_usdc_balance);
    }

    // Bootstrap token positions from Polymarket Data API.
    // In non-rotation mode this runs here (before threads start).
    // In slot/rotation mode the callback runs after market discovery.
    auto bootstrap_positions_cb = [&](const std::vector<std::string>& condition_ids) {
        if (!account || condition_ids.empty()) return;
        std::cout << "Fetching initial positions for " << account->address << "...\n";
        int seeded = lt::bootstrap_positions(account->address, condition_ids,
                                             *token_inventory);
        if (seeded > 0) {
            std::printf("  Seeded %d position(s) from Data API\n", seeded);
        } else {
            std::cout << "  No open positions found\n";
        }
    };

    if (!cfg.slot_manager_enabled && !cfg.rotation_enabled &&
        account && !cfg.market_pairs.empty()) {
        std::vector<std::string> condition_ids;
        for (const auto& pair : cfg.market_pairs) {
            condition_ids.push_back(pair.condition_id);
        }
        bootstrap_positions_cb(condition_ids);
    }

    // FIFO PnL tracker (shared between T1 writes and T6 reads via atomic)
    lt::PnlTracker pnl_tracker;
    // Register complement pairs for cross-asset paired matching (dual-bid model)
    for (const auto& pair_cfg : cfg.market_pairs) {
        pnl_tracker.register_pair(lt::AssetId(pair_cfg.token_id_up),
                                   lt::AssetId(pair_cfg.token_id_down));
    }

    // Create market data queue (T0 -> T2)
    auto strategy_queue =
        std::make_unique<lt::SpscQueue<lt::MarketNotification>>(cfg.strategy_queue_capacity);

    // Create M2 stub queues
    auto user_queue =
        std::make_unique<lt::SpscQueue<lt::SchedulerEvent>>(cfg.user_queue_capacity);
    auto exec_queue =
        std::make_unique<lt::SpscQueue<lt::SchedulerEvent>>(cfg.exec_queue_capacity);
    auto control_queue =
        std::make_unique<lt::SpscQueue<lt::SchedulerEvent>>(cfg.control_queue_capacity);

    // Inventory operation queue (T2 producer -> inventory worker consumer)
    std::unique_ptr<lt::SpscQueue<lt::InventoryOpRequest>> inventory_op_queue;
    std::unique_ptr<lt::InventoryQueueSink> inventory_op_sink;
    std::unique_ptr<lt::InventoryService> inventory_service;
    if (cfg.inventory_service_enabled) {
        inventory_op_queue =
            std::make_unique<lt::SpscQueue<lt::InventoryOpRequest>>(cfg.inventory_queue_capacity);
        inventory_op_sink = std::make_unique<lt::InventoryQueueSink>(
            *inventory_op_queue, &metrics);

        lt::InventoryServiceConfig inv_cfg;
        inv_cfg.enabled = true;
        inv_cfg.dry_run = cfg.inventory_service_dry_run;
        inv_cfg.poll_sleep_ms = cfg.inventory_service_poll_sleep_ms;

        // Relayer config
        inv_cfg.relayer_host = cfg.relayer_host;
        inv_cfg.relayer_port = cfg.relayer_port;
        inv_cfg.relayer_poll_interval_ms = cfg.relayer_poll_interval_ms;
        inv_cfg.relayer_max_poll_attempts = cfg.relayer_max_poll_attempts;
        inv_cfg.use_direct_rpc = cfg.inventory_use_direct_rpc;
        inv_cfg.polygon_rpc_url = cfg.polygon_rpc_url;
        inv_cfg.max_gas_price_gwei = cfg.max_gas_price_gwei;
        inv_cfg.gas_limit = cfg.inventory_gas_limit;
        inv_cfg.rpc_timeout_ms = cfg.inventory_rpc_timeout_ms;

        // Copy builder credentials from account
        if (account) {
            inv_cfg.builder_api_key = account->builder_api_key;
            inv_cfg.builder_api_secret_b64 = account->builder_api_secret;
            inv_cfg.builder_api_passphrase = account->builder_api_passphrase;
        }

        // Copy private key bytes for inventory signer (before secrets are cleared)
        {
            std::string pk_hex;
            if (account) pk_hex = account->private_key;
            else {
                const char* env_pk = std::getenv("PRIVATE_KEY");
                if (env_pk) pk_hex = env_pk;
            }
            lt::Bytes32 pk_bytes{};
            if (!pk_hex.empty() && lt::hex_decode_to_bytes32(pk_hex, pk_bytes)) {
                std::memcpy(inv_cfg.private_key, pk_bytes.data(), 32);
                inv_cfg.has_private_key = true;
                pk_bytes.fill(0);
                for (auto& c : pk_hex) { volatile char& v = const_cast<volatile char&>(c); v = '\0'; }
            }
        }

        inventory_service = std::make_unique<lt::InventoryService>(
            *inventory_op_queue, metrics, logger, inv_cfg, token_inventory.get());
    }

    // Create book delta queue (T0 -> T2 shadow books)
    auto book_delta_queue =
        std::make_unique<lt::SpscQueue<lt::BookDelta>>(cfg.book_delta_queue_capacity);

    // Create state store (T0)
    lt::MarketStateStore state_store(*strategy_queue, metrics);
    // In rotation mode, don't pre-seed here — coordinator will do it.
    // In non-rotation mode, seed from config as before.
    if (!cfg.rotation_enabled) {
        state_store.reserve_assets(market_ws_asset_ids.size());
        for (const auto& aid : market_ws_asset_ids) {
            state_store.seed_asset(lt::AssetId(aid));
        }
        state_store.set_strict_assets(true);
        // Register DOWN tokens so their book events are filtered at T0
        for (const auto& pair_cfg : cfg.market_pairs) {
            state_store.register_down_token(lt::AssetId(pair_cfg.token_id_down));
        }
    }

    // Wire book delta queue to state store (T0 produces, T2 consumes)
    state_store.set_book_delta_queue(book_delta_queue.get());

    // Create parser (T0)
    lt::MarketMessageParser parser;
    lt::SeqNum_t seq_counter = 0;

    // Build scheduler config
    lt::SchedulerConfig sched_cfg;
    sched_cfg.max_user_events_per_pass = cfg.max_user_events_per_pass;
    sched_cfg.max_exec_events_per_pass = cfg.max_exec_events_per_pass;
    sched_cfg.max_market_events_per_pass = cfg.max_market_events_per_pass;
    sched_cfg.max_control_events_per_pass = cfg.max_control_events_per_pass;
    sched_cfg.max_binance_md_events_per_pass = cfg.max_binance_md_events_per_pass;
    sched_cfg.stats_interval_ms = cfg.scheduler_stats_interval_ms;
    sched_cfg.poll_strategy = cfg.scheduler_poll_strategy;
    sched_cfg.sleep_us = cfg.scheduler_sleep_us;
    sched_cfg.strategy_stub_emit_intents = cfg.strategy_stub_emit_intents;
    sched_cfg.exec_feedback_loop_enabled = cfg.exec_feedback_loop_enabled;

    // Create M4 execution gateway components (if enabled)
    std::unique_ptr<lt::SpscQueue<lt::ExecIntent>> strategy_to_exec_queue;
    std::unique_ptr<lt::ExecQueueSink> exec_queue_sink;
    std::unique_ptr<lt::Secp256k1OrderSigner> order_signer;
    std::unique_ptr<lt::ExecutionGateway> gateway;

    if (cfg.gateway_enabled) {
        // When gateway is enabled, disable self-feedback loop to prevent
        // multi-producer SPSC violation (T3 is now the real exec producer)
        sched_cfg.exec_feedback_loop_enabled = false;

        strategy_to_exec_queue = std::make_unique<lt::SpscQueue<lt::ExecIntent>>(
            cfg.strategy_to_exec_capacity);
        exec_queue_sink = std::make_unique<lt::ExecQueueSink>(*strategy_to_exec_queue, &metrics);

        // Load private key from account file or env
        std::string pk_hex_str;
        if (account) {
            pk_hex_str = account->private_key;
        } else {
            const char* env_pk = std::getenv("PRIVATE_KEY");
            if (env_pk) pk_hex_str = env_pk;
        }
        if (pk_hex_str.empty()) {
            std::cerr << "FATAL: PRIVATE_KEY required when gateway_enabled=true "
                         "(provide account file or set env var)\n";
            return 1;
        }
        const char* pk_hex = pk_hex_str.c_str();
        lt::Bytes32 pk_bytes{};
        if (!lt::hex_decode_to_bytes32(pk_hex, pk_bytes)) {
            std::cerr << "FATAL: PRIVATE_KEY must be 32 bytes hex\n";
            return 1;
        }
        order_signer = std::make_unique<lt::Secp256k1OrderSigner>(pk_bytes.data());
        // Zero local copies of key material
        pk_bytes.fill(0);
        for (auto& c : pk_hex_str) { volatile char& v = const_cast<volatile char&>(c); v = '\0'; }
        pk_hex_str.clear();

        // Build gateway config
        lt::GatewayConfig gw_cfg;
        gw_cfg.rest_base_url = cfg.rest_base_url;
        gw_cfg.rest_host = cfg.rest_host;
        gw_cfg.rest_port = cfg.rest_port;
        gw_cfg.request_timeout_ms = cfg.rest_request_timeout_ms;
        gw_cfg.rest_pipeline_depth = cfg.rest_pipeline_depth;
        gw_cfg.gateway_enabled = true;
        gw_cfg.rate_limit.global_tokens_per_10s = cfg.rate_limit_global_per_10s;
        gw_cfg.rate_limit.order_tokens_per_10s = cfg.rate_limit_order_per_10s;
        gw_cfg.rate_limit.cancel_tokens_per_10s = cfg.rate_limit_cancel_per_10s;
        gw_cfg.rate_limit.backoff_base_ms = cfg.rate_limit_backoff_base_ms;
        gw_cfg.rate_limit.backoff_max_ms = cfg.rate_limit_backoff_max_ms;
        gw_cfg.heartbeat.interval_ms = cfg.heartbeat_interval_ms;
        gw_cfg.heartbeat.max_consecutive_failures = cfg.heartbeat_max_failures;
        gw_cfg.heartbeat.cancel_all_on_failure = cfg.heartbeat_cancel_on_failure;
        gw_cfg.batch_orders_enabled = cfg.batch_orders_enabled;
        gw_cfg.batch_max_size = cfg.batch_max_size;
        gw_cfg.cancel_connection_redundancy = cfg.cancel_connection_redundancy;
        gw_cfg.use_http2 = cfg.rest_use_http2;
        gw_cfg.max_concurrent_streams = cfg.rest_max_concurrent_streams;
        gw_cfg.order_connection_pool_size = cfg.order_connection_pool_size;

        // Read owner UUID and address from account file or env
        std::string owner_uuid_str;
        std::string address_str;
        if (account) {
            owner_uuid_str = account->owner_uuid;
            address_str = account->address;
        } else {
            const char* env_uuid = std::getenv("POLY_OWNER_UUID");
            const char* env_addr = std::getenv("POLY_ADDRESS");
            if (env_uuid) owner_uuid_str = env_uuid;
            if (env_addr) address_str = env_addr;
        }
        if (address_str.empty()) {
            std::cerr << "FATAL: POLY_ADDRESS required when gateway_enabled "
                         "(provide account file or set env var)\n";
            return 1;
        }
        // owner_uuid defaults to address if not explicitly set (non-proxy wallets)
        if (owner_uuid_str.empty()) {
            owner_uuid_str = address_str;
        }
        gw_cfg.owner_uuid = owner_uuid_str;
        gw_cfg.maker_address = address_str;  // proxy/funder address

        // Derive signer address from private key (EOA)
        {
            uint8_t derived_addr[20]{};
            if (!order_signer->get_signer_address(derived_addr)) {
                std::cerr << "FATAL: Failed to derive address from PRIVATE_KEY\n";
                return 1;
            }
            std::string signer_hex = "0x" + lt::hex_encode(derived_addr, 20);
            gw_cfg.signer_address = signer_hex;

            // Log both addresses (proxy wallets have different maker vs signer)
            if (signer_hex != address_str) {
                std::cout << "  Maker (proxy): " << address_str << "\n";
                std::cout << "  Signer (EOA):  " << signer_hex << "\n";
            } else {
                std::cout << "  Address (EOA): " << address_str << "\n";
            }
        }

        gw_cfg.defer_exec = cfg.default_defer_exec;
        gw_cfg.post_only = cfg.default_post_only;
        gw_cfg.signature_type = cfg.default_signature_type;

        // Populate explicit auth fields from account (gateway branches on these)
        if (account) {
            gw_cfg.poly_api_key = account->api_key;
            gw_cfg.poly_api_secret_b64 = account->api_secret;
            gw_cfg.poly_api_passphrase = account->api_passphrase;
            gw_cfg.poly_api_address = gw_cfg.signer_address;
            // owner = API key UUID (Polymarket requires this, not an address)
            gw_cfg.owner_uuid = account->api_key;
        }

        gateway = std::make_unique<lt::ExecutionGateway>(
            *strategy_to_exec_queue, *exec_queue, metrics, logger,
            gw_cfg, *order_signer, &g_fatal, token_inventory.get());
    }

    // Create M6 UI Bridge queues (if enabled)
    std::unique_ptr<lt::SpscQueue<lt::UiBookUpdate>> ui_book_queue;
    std::unique_ptr<lt::SpscQueue<lt::UiStateSnapshot>> ui_state_queue;
    std::unique_ptr<lt::IpcBridge> ipc_bridge;

    if (cfg.ui_bridge_enabled) {
        ui_book_queue = std::make_unique<lt::SpscQueue<lt::UiBookUpdate>>(
            cfg.ui_book_queue_capacity);
        ui_state_queue = std::make_unique<lt::SpscQueue<lt::UiStateSnapshot>>(
            cfg.ui_state_queue_capacity);

        // Wire T0 book push
        state_store.set_ui_book_queue(ui_book_queue.get(), cfg.ui_snapshot_rate_hz);
    }

    // Create M5 components
    auto working_tracker = std::make_unique<lt::WorkingOrderTracker>();

    lt::ExecSink* active_sink = cfg.gateway_enabled && exec_queue_sink
        ? static_cast<lt::ExecSink*>(exec_queue_sink.get())
        : nullptr;

    // ModeFilteredSink wraps the active sink (gateway or stub)
    auto exec_mode = static_cast<lt::ExecutionMode>(cfg.execution_mode);
    auto mode_filtered_sink = std::make_unique<lt::ModeFilteredSink>(
        active_sink, &metrics, exec_mode);

    // Strategy factory — select based on config
    std::unique_ptr<lt::Strategy> strategy;
    if (cfg.strategy_type == "test") {
        strategy = std::make_unique<lt::TestStrategy>(
            working_tracker.get(), &market_pair_registry,
            cfg.smoke_test_log_file.c_str());
        std::cout << "  Strategy: TestStrategy (smoke test)\n";
    } else if (cfg.strategy_type == "inventory_test") {
        strategy = std::make_unique<lt::InventoryTestStrategy>(
            &market_pair_registry,
            cfg.smoke_test_log_file.c_str());
        std::cout << "  Strategy: InventoryTestStrategy (inventory ops test)\n";
    } else {
        lt::QuoterV2Config q2_config;
        q2_config.offset = cfg.quoter_v2_offset;
        q2_config.skew_strength = cfg.quoter_v2_skew_strength;
        q2_config.max_skew = cfg.quoter_v2_max_skew;
        q2_config.max_inventory = cfg.quoter_v2_max_inventory;
        q2_config.quote_size = cfg.quoter_v2_quote_size;
        q2_config.min_order_size = cfg.quoter_v2_min_order_size;
        q2_config.emergency_qty = cfg.quoter_v2_emergency_qty;
        q2_config.price_floor = cfg.quoter_v2_price_floor;
        q2_config.price_ceiling = cfg.quoter_v2_price_ceiling;
        q2_config.initial_split_size = cfg.quoter_v2_initial_split_size;
        q2_config.inventory_low_water = cfg.quoter_v2_inventory_low_water;
        q2_config.inventory_replenish_size = cfg.quoter_v2_inventory_replenish_size;
        q2_config.inventory_merge_threshold = cfg.quoter_v2_inventory_merge_threshold;
        q2_config.inventory_merge_size = cfg.quoter_v2_inventory_merge_size;
        q2_config.inventory_cooldown_ms = cfg.quoter_v2_inventory_cooldown_ms;
        q2_config.max_replaces_per_second = cfg.quoter_v2_max_replaces_per_second;
        q2_config.min_quote_lifetime_ms = cfg.quoter_v2_min_quote_lifetime_ms;
        q2_config.degraded_refresh_ms = cfg.quoter_v2_degraded_refresh_ms;
        q2_config.deterministic_timing = cfg.quoter_v2_deterministic_timing;
        // V2.1: Three-tier inventory + gamma + FAK rework
        q2_config.soft_max_inventory = cfg.quoter_v2_soft_max_inventory;
        q2_config.hard_max_inventory = cfg.quoter_v2_hard_max_inventory;
        q2_config.market_duration_ms = cfg.quoter_v2_market_duration_ms;
        q2_config.hard_cutoff_ms = cfg.quoter_v2_hard_cutoff_ms;
        q2_config.gamma_power_x1000 = cfg.quoter_v2_gamma_power_x1000;
        q2_config.offset_growth_fp4 = cfg.quoter_v2_offset_growth_fp4;
        q2_config.time_floor_mult_x1000 = cfg.quoter_v2_time_floor_mult_x1000;
        q2_config.time_floor_threshold_ms = cfg.quoter_v2_time_floor_threshold_ms;
        q2_config.fak_cooldown_ms = cfg.quoter_v2_fak_cooldown_ms;
        q2_config.fak_gamma_floor_x1000 = cfg.quoter_v2_fak_gamma_floor_x1000;
        auto q2 = std::make_unique<lt::QuoterV2Strategy>(
            q2_config, working_tracker.get(), &market_pair_registry, &metrics);
        q2->set_pnl_tracker(&pnl_tracker);
        strategy = std::move(q2);
        std::cout << "  Strategy: QuoterV2Strategy\n";
    }
    // Strategy starts disabled — user must explicitly enable via UI
    strategy->set_enabled(false);

    // Risk config — scale human-readable integers to kQtyScale
    lt::RiskConfig risk_config;
    risk_config.max_position_per_token = lt::qty_from_int(cfg.risk_max_position_per_token);
    risk_config.max_net_exposure_per_market = lt::qty_from_int(cfg.risk_max_net_exposure_per_market);
    risk_config.max_notional = lt::qty_from_int(cfg.risk_max_notional);
    risk_config.max_loss = lt::qty_from_int(cfg.risk_max_loss);
    risk_config.cancel_all_on_violation = cfg.risk_cancel_all_on_violation;

    auto risk_gate = std::make_unique<lt::RiskGate>(
        risk_config, token_inventory.get(), working_tracker.get(),
        &market_pair_registry, &metrics);

    // Create M2 strategy scheduler (T2)
    lt::StrategyScheduler scheduler(*strategy_queue, *user_queue, *exec_queue,
                                    *control_queue, metrics, logger, sched_cfg,
                                    &g_fatal, active_sink,
                                    &market_pair_registry, token_inventory.get(),
                                    strategy.get(),
                                    risk_gate.get(),
                                    mode_filtered_sink.get(),
                                    working_tracker.get(),
                                    inventory_op_sink.get());

    // In non-rotation mode, pre-seed scheduler asset state from config.
    // In rotation mode, the coordinator will seed dynamically via callbacks.
    if (!cfg.rotation_enabled) {
        scheduler.reserve_asset_state(market_ws_asset_ids.size() + user_ws_market_ids.size());
        for (const auto& aid : market_ws_asset_ids) {
            scheduler.seed_asset_state(lt::AssetId(aid));
        }
        for (const auto& mid : user_ws_market_ids) {
            scheduler.seed_asset_state(lt::AssetId(mid));
        }
        scheduler.set_strict_asset_state(true);
    }

    // Wire book delta queue to scheduler and seed shadow books (UP tokens only —
    // DOWN book events are filtered at T0, so no shadow books needed for them)
    scheduler.set_book_delta_queue(book_delta_queue.get());
    if (!cfg.rotation_enabled) {
        scheduler.reserve_strategy_books(cfg.market_pairs.size() + cfg.asset_ids.size());
        for (const auto& pair_cfg : cfg.market_pairs) {
            scheduler.seed_strategy_book(lt::AssetId(pair_cfg.token_id_up));
        }
        for (const auto& aid : cfg.asset_ids) {
            scheduler.seed_strategy_book(lt::AssetId(aid));
        }
    }

    // M7: Wire T2->T3 queue for depth monitoring
    if (cfg.gateway_enabled && strategy_to_exec_queue) {
        scheduler.set_exec_intent_queue(strategy_to_exec_queue.get());
    }

    // Wire M6 UI state push to T2 scheduler
    if (cfg.ui_bridge_enabled && ui_state_queue) {
        scheduler.set_ui_state_queue(ui_state_queue.get(), cfg.ui_snapshot_rate_hz);
    }

    // --- RTDS (Real-Time Data Socket) components ---
    std::unique_ptr<lt::SpscQueue<lt::CryptoPriceUpdate>> rtds_queue;
    std::unique_ptr<lt::RtdsWsClient> rtds_client;
    std::unique_ptr<lt::RtdsMessageParser> rtds_parser;

    // Per-source recording queues (separate SPSC per producer thread)
    std::unique_ptr<lt::SpscQueue<lt::RtdsRecord>> rtds_rec_queue;
    std::unique_ptr<lt::SpscQueue<lt::RawWsMessage>> market_rec_queue;  // raw JSON from T0
    std::unique_ptr<lt::SpscQueue<lt::RawWsMessage>> user_rec_queue;    // raw JSON from T1
    std::unique_ptr<lt::SpscQueue<lt::JournalRecord>> journal_rec_queue;  // T2 trade journal
    std::unique_ptr<lt::JournalWriter> journal_writer;
    std::unique_ptr<lt::DataRecorder> data_recorder;

    if (cfg.rtds_enabled) {
        rtds_queue = std::make_unique<lt::SpscQueue<lt::CryptoPriceUpdate>>(
            cfg.rtds_queue_capacity);
        scheduler.set_rtds_queue(rtds_queue.get());

        rtds_parser = std::make_unique<lt::RtdsMessageParser>();

        lt::RtdsWsConfig rtds_ws_cfg;
        rtds_ws_cfg.endpoint = cfg.rtds_endpoint;
        rtds_ws_cfg.ping_interval_ms = cfg.rtds_ping_interval_ms;
        rtds_ws_cfg.reconnect_base_ms = cfg.rtds_reconnect_base_ms;
        rtds_ws_cfg.reconnect_max_ms = cfg.rtds_reconnect_max_ms;
        rtds_ws_cfg.stale_threshold_ms = cfg.rtds_stale_threshold_ms;
        rtds_client = std::make_unique<lt::RtdsWsClient>(rtds_ws_cfg);

        std::cout << "  RTDS enabled: " << cfg.rtds_endpoint << "\n";
    }

    // --- Binance Spot market-data components ---
    std::unique_ptr<lt::SpscQueue<lt::BinanceMarketUpdate>> binance_md_queue;
    std::unique_ptr<lt::BinanceWsClient> binance_md_client;
    std::unique_ptr<lt::BinanceMessageParser> binance_md_parser;

    if (cfg.binance_md_enabled) {
        binance_md_queue = std::make_unique<lt::SpscQueue<lt::BinanceMarketUpdate>>(
            cfg.binance_md_queue_capacity);
        scheduler.set_binance_md_queue(binance_md_queue.get());

        binance_md_parser = std::make_unique<lt::BinanceMessageParser>();

        // Build combined-stream URL from endpoint + comma-separated stream list.
        //   wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade
        std::string url = cfg.binance_md_endpoint;
        while (!url.empty() && url.back() == '/') url.pop_back();
        url += "/stream?streams=";
        {
            std::string streams = cfg.binance_md_streams;
            bool first = true;
            std::size_t start = 0;
            for (std::size_t i = 0; i <= streams.size(); ++i) {
                if (i == streams.size() || streams[i] == ',') {
                    auto tok = streams.substr(start, i - start);
                    // trim whitespace
                    while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t'))
                        tok.erase(tok.begin());
                    while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t'))
                        tok.pop_back();
                    if (!tok.empty()) {
                        if (!first) url += '/';
                        url += tok;
                        first = false;
                    }
                    start = i + 1;
                }
            }
        }

        lt::BinanceWsConfig b_cfg;
        b_cfg.endpoint = url;
        b_cfg.reconnect_base_ms = cfg.binance_md_reconnect_base_ms;
        b_cfg.reconnect_max_ms = cfg.binance_md_reconnect_max_ms;
        b_cfg.stale_threshold_ms = cfg.binance_md_stale_threshold_ms;
        b_cfg.rotate_interval_ms = cfg.binance_md_rotate_interval_ms;
        binance_md_client = std::make_unique<lt::BinanceWsClient>(b_cfg);

        std::cout << "  Binance MD enabled: " << url << "\n";
    }

    if (cfg.recording_enabled || cfg.journal_enabled) {
        // Create per-source recording queues
        if (cfg.recording_enabled && cfg.rtds_enabled) {
            rtds_rec_queue = std::make_unique<lt::SpscQueue<lt::RtdsRecord>>(
                cfg.recording_queue_capacity);
        }
        // Raw WS queues: 64KB per entry, use smaller capacity (64 entries = ~4MB each)
        if (cfg.recording_enabled) {
            constexpr std::size_t kRawQueueCapacity = 64;
            market_rec_queue = std::make_unique<lt::SpscQueue<lt::RawWsMessage>>(kRawQueueCapacity);
            if (cfg.user_ws_enabled) {
                user_rec_queue = std::make_unique<lt::SpscQueue<lt::RawWsMessage>>(kRawQueueCapacity);
            }
        }
        // Journal queue: 256B per record, T2 -> T_rec
        if (cfg.journal_enabled) {
            journal_rec_queue = std::make_unique<lt::SpscQueue<lt::JournalRecord>>(
                cfg.journal_queue_capacity);
            bool is_dry_run = (cfg.execution_mode == 0);
            journal_writer = std::make_unique<lt::JournalWriter>(
                journal_rec_queue.get(), &metrics, cfg.journal_level, is_dry_run);
        }

        lt::DataRecorderConfig rec_cfg;
        rec_cfg.output_dir = cfg.recording_output_dir;
        rec_cfg.flush_interval_ms = cfg.recording_flush_interval_ms;
        rec_cfg.min_disk_space_mb = cfg.recording_min_disk_space_mb;
        rec_cfg.enabled = true;
        data_recorder = std::make_unique<lt::DataRecorder>(
            rec_cfg,
            rtds_rec_queue.get(),
            market_rec_queue.get(),
            user_rec_queue.get(),
            journal_rec_queue.get());

        if (cfg.recording_enabled) {
            std::cout << "  Recording enabled: " << cfg.recording_output_dir << "\n";
        }
        if (cfg.journal_enabled) {
            std::cout << "  Journal enabled: level=" << cfg.journal_level
                      << " capacity=" << cfg.journal_queue_capacity << "\n";
        }
    }

    // Wire journal writer to scheduler (T2 producer)
    if (journal_writer) {
        scheduler.set_journal_writer(journal_writer.get());
    }

    // Create M6 IpcBridge (T6) — only if enabled
    if (cfg.ui_bridge_enabled && ui_book_queue && ui_state_queue) {
        lt::IpcBridgeConfig ipc_cfg;
        ipc_cfg.ws_port = cfg.ui_ws_port;
        ipc_cfg.snapshot_rate_hz = cfg.ui_snapshot_rate_hz;
        ipc_cfg.book_depth = cfg.ui_book_depth;
        ipc_cfg.watcher_enabled = cfg.watcher_enabled;
        ipc_cfg.watcher_discovery_poll_ms = cfg.watcher_discovery_poll_ms;
        ipc_cfg.watcher_status_poll_ms = cfg.watcher_status_poll_ms;
        ipc_cfg.watcher_stale_threshold_ms = cfg.watcher_stale_threshold_ms;
        ipc_cfg.ladder_update_interval_ms = cfg.ladder_update_interval_ms;
        ipc_cfg.ladder_max_depth = cfg.ladder_max_depth;
        ipc_cfg.account_poll_interval_ms = cfg.account_poll_interval_ms;
        ipc_cfg.polygon_rpc_url = cfg.polygon_rpc_url;
        ipc_cfg.balance_poll_interval_ms = cfg.balance_poll_interval_ms;
        ipc_cfg.auth_token = cfg.ui_ws_auth_token;
        ipc_cfg.bind_address = cfg.ui_ws_bind_address;
        if (order_signer) {
            uint8_t eoa_addr[20]{};
            if (order_signer->get_signer_address(eoa_addr)) {
                ipc_cfg.eoa_address = "0x" + lt::hex_encode(eoa_addr, 20);
            }
        }

        ipc_bridge = std::make_unique<lt::IpcBridge>(
            *ui_book_queue, *ui_state_queue, *control_queue,
            metrics, logger, ipc_cfg,
            &market_pair_registry,
            &cfg.market_pairs, &g_fatal);

        // Pass engine config for watcher service (WS endpoints, etc.)
        if (cfg.watcher_enabled) {
            ipc_bridge->set_engine_config(cfg);
        }

        ipc_bridge->set_token_inventory(token_inventory.get());
        ipc_bridge->set_pnl_tracker(&pnl_tracker);

        // Pass account identity for UI snapshot display
        if (account) {
            ipc_bridge->set_account_info(account->name, account->address);

            // Pass account credentials for balance/position polling
            if (!account->api_key.empty()) {
                ipc_bridge->set_account_credentials(
                    account->api_key, account->api_secret,
                    account->api_passphrase, account->address);
            }
        }
    }

    // --- M3 User WS components (T1) ---
    // Shared parser and state store persist across reconnections in rotation mode.
    std::unique_ptr<lt::UserMessageParser> user_parser;
    std::unique_ptr<lt::ExecStateStore> exec_state_store;
    lt::SeqNum_t user_seq_counter = 0;

    if (cfg.user_ws_enabled) {
        user_parser = std::make_unique<lt::UserMessageParser>();
        exec_state_store = std::make_unique<lt::ExecStateStore>(
            *user_queue, metrics, &logger, &g_fatal, token_inventory.get(), &pnl_tracker);
    }

    // --- Managed T0/T1 threads (start/stop lambdas) ---
    // These allow rotation coordinator to restart WS clients with new subscriptions.
    std::unique_ptr<lt::MarketWsClient> ws_client;
    std::thread market_ws_thread;

    std::unique_ptr<lt::UserWsClient> user_ws_client;
    std::thread user_ws_thread;

    auto start_market_ws = [&](const std::vector<std::string>& token_ids) {
        lt::WsClientConfig ws_cfg;
        ws_cfg.endpoint = cfg.ws_endpoint;
        ws_cfg.asset_ids = token_ids;
        ws_cfg.ping_interval_ms = cfg.ping_interval_ms;
        ws_cfg.pong_timeout_ms = cfg.pong_timeout_ms;
        ws_cfg.reconnect_base_ms = cfg.reconnect_base_ms;
        ws_cfg.reconnect_max_ms = cfg.reconnect_max_ms;
        ws_cfg.redundancy = cfg.ws_redundancy;
        ws_cfg.redundancy_stagger_ms = cfg.ws_redundancy_stagger_ms;

        ws_client = std::make_unique<lt::MarketWsClient>(ws_cfg);

        ws_client->set_on_message(
            [&parser, &state_store, &metrics, &seq_counter, &main_log, &market_rec_queue](
                std::string_view payload, lt::Timestamp_ns recv_ts) {
                metrics.inc(lt::MetricId::WS_FRAMES_RECEIVED);
                metrics.add(lt::MetricId::WS_BYTES_RECEIVED,
                            static_cast<int64_t>(payload.size()));

                auto parse_start = lt::SteadyClock::now();
                auto event = std::make_unique<lt::MarketEvent>();
                auto err = parser.parse(payload, recv_ts, ++seq_counter, *event);
                auto parse_dt = lt::SteadyClock::now() - parse_start;
                metrics.record_latency(lt::MetricId::PARSE_LATENCY_NS,
                                       lt::MetricId::PARSE_LATENCY_COUNT, parse_dt);

                if (err == lt::ErrorCode::OK) {
                    metrics.inc(lt::MetricId::PARSE_OK);
                    std::visit(
                        [&metrics](const auto& p) {
                            using T = std::decay_t<decltype(p)>;
                            if constexpr (std::is_same_v<T, lt::BookSnapshot>)
                                metrics.inc(lt::MetricId::PARSE_BOOK);
                            else if constexpr (std::is_same_v<T, lt::PriceChangeEvent>)
                                metrics.inc(lt::MetricId::PARSE_PRICE_CHANGE);
                            else if constexpr (std::is_same_v<T, lt::BestBidAskEvent>)
                                metrics.inc(lt::MetricId::PARSE_BEST_BID_ASK);
                            else if constexpr (std::is_same_v<T, lt::TickSizeChangeEvent>)
                                metrics.inc(lt::MetricId::PARSE_TICK_SIZE_CHANGE);
                            else if constexpr (std::is_same_v<T, lt::LastTradePriceEvent>)
                                metrics.inc(lt::MetricId::PARSE_LAST_TRADE_PRICE);
                            else if constexpr (std::is_same_v<T, lt::NewMarketEvent>)
                                metrics.inc(lt::MetricId::NEW_MARKETS_RECEIVED);
                            else if constexpr (std::is_same_v<T, lt::MarketResolvedEvent>)
                                metrics.inc(lt::MetricId::MARKETS_RESOLVED);
                        },
                        event->payload);
                    state_store.apply(*event);

                    // Record raw JSON to market recording queue
                    if (market_rec_queue) {
                        lt::RawWsMessage msg{};
                        msg.recv_ts = recv_ts;
                        msg.wall_clock_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        auto len = std::min(payload.size(), static_cast<std::size_t>(lt::kMaxRawWsPayload));
                        std::memcpy(msg.payload, payload.data(), len);
                        msg.payload_len = static_cast<uint32_t>(len);
                        if (len < payload.size()) msg.flags = lt::kRawFlagTruncated;
                        if (market_rec_queue->try_push(msg))
                            metrics.inc(lt::MetricId::MARKET_RECORD_PUSHES);
                        else
                            metrics.inc(lt::MetricId::MARKET_RECORD_DROPS);
                    }
                } else if (err == lt::ErrorCode::PONG_MESSAGE) {
                    metrics.inc(lt::MetricId::PARSE_PONG);
                } else if (err == lt::ErrorCode::UNKNOWN_EVENT_TYPE) {
                    metrics.inc(lt::MetricId::PARSE_UNKNOWN);
                } else {
                    metrics.inc(lt::MetricId::PARSE_ERRORS);
                }
            });

        ws_client->set_on_connected([&main_log]() {
            lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Market WebSocket connected");
        });

        ws_client->set_on_disconnected([&main_log, &metrics](const std::string& reason) {
            metrics.inc(lt::MetricId::WS_RECONNECTS);
            metrics.inc(lt::MetricId::WS_ERRORS);
            char buf[lt::LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "Market WebSocket disconnected: %s", reason.c_str());
            lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
        });

        auto* client_ptr = ws_client.get();
        market_ws_thread = std::thread([client_ptr, &cfg]() {
            if (cfg.pin_threads) {
                lt::pin_thread_to_core(cfg.pin_core_t0);
                lt::set_thread_high_priority();
            }
            client_ptr->run();
        });
    };

    std::once_flag market_ws_stopped;
    auto stop_market_ws = [&]() {
        std::call_once(market_ws_stopped, [&]() {
            if (ws_client) ws_client->request_shutdown();
            if (market_ws_thread.joinable()) market_ws_thread.join();
            ws_client.reset();
        });
    };

    auto start_user_ws = [&](const std::vector<std::string>& condition_ids) {
        if (!cfg.user_ws_enabled || !user_parser || !exec_state_store) return;

        lt::UserWsClientConfig user_ws_cfg;
        user_ws_cfg.endpoint = cfg.user_ws_endpoint;
        user_ws_cfg.market_ids = condition_ids;
        user_ws_cfg.ping_interval_ms = cfg.user_ws_ping_interval_ms;
        user_ws_cfg.reconnect_base_ms = cfg.user_ws_reconnect_base_ms;
        user_ws_cfg.reconnect_max_ms = cfg.user_ws_reconnect_max_ms;
        user_ws_cfg.stale_threshold_ms = cfg.user_ws_stale_threshold_ms;
        user_ws_cfg.fatal_flag = &g_fatal;
        user_ws_cfg.max_auth_failures = cfg.user_ws_max_auth_failures;
        user_ws_cfg.redundancy = cfg.user_ws_redundancy;
        user_ws_cfg.redundancy_stagger_ms = cfg.user_ws_redundancy_stagger_ms;
        if (account) {
            user_ws_cfg.api_key = account->api_key;
            user_ws_cfg.api_secret = account->api_secret;
            user_ws_cfg.api_passphrase = account->api_passphrase;
        }

        user_ws_client = std::make_unique<lt::UserWsClient>(user_ws_cfg);

        user_ws_client->set_on_message(
            [&user_parser, &exec_state_store, &metrics, &user_seq_counter, &main_log,
             &user_rec_queue](
                std::string_view payload, lt::Timestamp_ns recv_ts) {
                metrics.inc(lt::MetricId::USER_WS_MESSAGES);
                lt::UserMessageEvent event;
                auto err = user_parser->parse(payload, recv_ts, ++user_seq_counter, event);

                if (err == lt::ErrorCode::OK) {
                    metrics.inc(lt::MetricId::USER_WS_PARSE_OK);

                    // Observability: truncation warnings
                    if (event.truncated_fields > 0) {
                        metrics.inc(lt::MetricId::USER_WS_FIELD_TRUNCATIONS);
                        char buf[lt::LogEntry::kMaxMsg];
                        std::snprintf(buf, sizeof(buf),
                                      "User WS: %u field(s) truncated in message",
                                      event.truncated_fields);
                        lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
                    }

                    std::visit(
                        [&](const auto& upd) {
                            using T = std::decay_t<decltype(upd)>;
                            lt::ApplyResult result = lt::ApplyResult::APPLIED;
                            if constexpr (std::is_same_v<T, lt::UserOrderUpdate>) {
                                if (upd.size_matched_exceeds_original) {
                                    char buf[lt::LogEntry::kMaxMsg];
                                    std::snprintf(buf, sizeof(buf),
                                                  "User WS: size_matched > original_size "
                                                  "(matched=%lld orig=%lld)",
                                                  static_cast<long long>(upd.size_matched),
                                                  static_cast<long long>(upd.original_size));
                                    lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
                                }
                                result = exec_state_store->apply_order_update(
                                    upd, recv_ts, event.seq);
                            } else if constexpr (std::is_same_v<T, lt::UserTradeUpdate>) {
                                result = exec_state_store->apply_trade_update(
                                    upd, recv_ts, event.seq);
                            }
                            if (result == lt::ApplyResult::QUEUE_OVERFLOW) {
                                lt::AsyncLogger::log(main_log, lt::LogLevel::FATAL,
                                                     "User WS queue overflow — fatal, shutting down");
                            }
                        },
                        event.payload);

                    // Record raw JSON to user recording queue
                    if (user_rec_queue) {
                        lt::RawWsMessage msg{};
                        msg.recv_ts = recv_ts;
                        msg.wall_clock_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        auto len = std::min(payload.size(), static_cast<std::size_t>(lt::kMaxRawWsPayload));
                        std::memcpy(msg.payload, payload.data(), len);
                        msg.payload_len = static_cast<uint32_t>(len);
                        if (len < payload.size()) msg.flags = lt::kRawFlagTruncated;
                        if (user_rec_queue->try_push(msg))
                            metrics.inc(lt::MetricId::USER_RECORD_PUSHES);
                        else
                            metrics.inc(lt::MetricId::USER_RECORD_DROPS);
                    }
                } else if (err == lt::ErrorCode::USER_WS_SERVER_ERROR) {
                    metrics.inc(lt::MetricId::USER_WS_SERVER_ERRORS);
                    metrics.inc(lt::MetricId::USER_WS_PARSE_FAIL);
                    char buf[lt::LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "User WS server error: %s", event.server_error_msg);
                    lt::AsyncLogger::log(main_log, lt::LogLevel::ERROR, buf);
                } else if (err == lt::ErrorCode::UNKNOWN_EVENT_TYPE) {
                    metrics.inc(lt::MetricId::USER_WS_UNKNOWN_EVENT_TYPES);
                    metrics.inc(lt::MetricId::USER_WS_PARSE_FAIL);
                    char buf[lt::LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "User WS unknown event type: %s", event.server_error_msg);
                    lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
                } else if (err == lt::ErrorCode::PONG_MESSAGE) {
                    // PONG is expected, no metric
                } else {
                    metrics.inc(lt::MetricId::USER_WS_PARSE_FAIL);
                    char buf[lt::LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "User WS parse error: %d", static_cast<int>(err));
                    lt::AsyncLogger::log(main_log, lt::LogLevel::ERROR, buf);
                }
            });

        user_ws_client->set_on_connected([&main_log]() {
            lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "User WebSocket connected");
        });

        user_ws_client->set_on_disconnected([&main_log, &metrics](const std::string& reason) {
            metrics.inc(lt::MetricId::USER_WS_RECONNECTS);
            if (reason.find("stale_watchdog") != std::string::npos) {
                metrics.inc(lt::MetricId::USER_WS_STALE_DETECTED);
            }
            if (reason.find("auth_failure") != std::string::npos) {
                metrics.inc(lt::MetricId::USER_WS_AUTH_FAILURES);
            }
            if (reason.find("ws_close") != std::string::npos) {
                metrics.inc(lt::MetricId::USER_WS_CLOSE_FRAMES);
            }
            char buf[lt::LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "User WebSocket disconnected: %s", reason.c_str());
            lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
        });

        auto* client_ptr = user_ws_client.get();
        user_ws_thread = std::thread([client_ptr, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_t1);
            client_ptr->run();
        });
    };

    std::once_flag user_ws_stopped;
    auto stop_user_ws = [&]() {
        std::call_once(user_ws_stopped, [&]() {
            if (user_ws_client) user_ws_client->request_shutdown();
            if (user_ws_thread.joinable()) user_ws_thread.join();
            user_ws_client.reset();
        });
    };

    // Clear sensitive account data now that all consumers have been constructed
    // Don't clear if rotation or slot manager will start T1 later with credentials.
    if (account && !cfg.rotation_enabled && !cfg.slot_manager_enabled) {
        account->clear_secrets();
    }

    // Install signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Start logger thread (T5) first
    logger.start();
    lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Engine starting (M5)");

    // Start M2 strategy scheduler thread (T2) — only if enabled
    std::thread scheduler_thread;
    if (cfg.scheduler_enabled) {
        scheduler_thread = std::thread([&scheduler, &cfg]() {
            if (cfg.pin_threads) {
                lt::pin_thread_to_core(cfg.pin_core_t2);
                lt::set_thread_high_priority();
            }
            scheduler.run();
        });
    } else {
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO,
                             "Scheduler disabled by config — T2 thread not started");
    }

    // Start M4 Execution Gateway thread (T3) — only if enabled
    std::thread gateway_thread;
    if (cfg.gateway_enabled && gateway) {
        gateway_thread = std::thread([&gateway, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_t3);
            gateway->run();
        });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Execution Gateway thread (T3) started");
    }

    // Start inventory service thread (non-hot-path worker)
    std::thread inventory_service_thread;
    if (cfg.inventory_service_enabled && inventory_service) {
        inventory_service_thread = std::thread([&inventory_service]() { inventory_service->run(); });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO,
                             "Inventory service thread started");
    }

    // Start M6 IPC Bridge thread (T6) — only if enabled
    std::thread ipc_bridge_thread;
    if (cfg.ui_bridge_enabled && ipc_bridge) {
        ipc_bridge_thread = std::thread([&ipc_bridge, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_t6);
            ipc_bridge->run();
        });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "IPC Bridge thread (T6) started");
    }

    // Start data recorder thread (T_rec)
    std::thread recorder_thread;
    if ((cfg.recording_enabled || cfg.journal_enabled) && data_recorder) {
        recorder_thread = std::thread([&data_recorder, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_rec);
            data_recorder->run();
        });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Data recorder thread started");
    }

    // Start RTDS WebSocket thread (T_rtds)
    std::thread rtds_thread;
    if (cfg.rtds_enabled && rtds_client) {
        rtds_client->set_on_message(
            [&rtds_parser, &rtds_queue, &rtds_rec_queue, &metrics, &main_log](
                std::string_view payload, lt::Timestamp_ns recv_ts) {
                metrics.inc(lt::MetricId::RTDS_MESSAGES);

                lt::CryptoPriceUpdate update;
                auto err = rtds_parser->parse(payload, recv_ts, update);

                if (err == lt::ErrorCode::OK) {
                    metrics.inc(lt::MetricId::RTDS_PARSE_OK);

                    // Push to T2 scheduler queue
                    if (rtds_queue) {
                        if (rtds_queue->try_push(update)) {
                            metrics.inc(lt::MetricId::RTDS_QUEUE_PUSHES);
                        } else {
                            metrics.inc(lt::MetricId::RTDS_QUEUE_DROPS);
                        }
                    }

                    // Push to RTDS recording queue (binary)
                    if (rtds_rec_queue) {
                        lt::RtdsRecord rec{};
                        rec.recv_ts = recv_ts;
                        rec.wall_clock_ms = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        rec.exchange_ts_ms = update.exchange_ts_ms;
                        rec.value = update.value;
                        std::memcpy(rec.symbol, update.symbol.data,
                                    std::min<size_t>(update.symbol.len, 23));
                        rec.symbol_len = update.symbol.len;
                        if (rtds_rec_queue->try_push(rec)) {
                            metrics.inc(lt::MetricId::RTDS_RECORD_PUSHES);
                        } else {
                            metrics.inc(lt::MetricId::RTDS_RECORD_DROPS);
                        }
                    }
                } else if (err == lt::ErrorCode::PONG_MESSAGE) {
                    metrics.inc(lt::MetricId::RTDS_PONG);
                } else if (err == lt::ErrorCode::UNKNOWN_EVENT_TYPE) {
                    metrics.inc(lt::MetricId::RTDS_UNKNOWN);
                } else {
                    metrics.inc(lt::MetricId::RTDS_PARSE_FAIL);
                }
            });

        rtds_client->set_on_connected([&main_log]() {
            lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "RTDS WebSocket connected");
        });

        rtds_client->set_on_disconnected([&main_log, &metrics](const std::string& reason) {
            metrics.inc(lt::MetricId::RTDS_RECONNECTS);
            char buf[lt::LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "RTDS WebSocket disconnected: %s", reason.c_str());
            lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
        });

        auto* rtds_ptr = rtds_client.get();
        rtds_thread = std::thread([rtds_ptr, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_rtds);
            rtds_ptr->run();
        });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "RTDS thread started");
    }

    // Start Binance market-data WebSocket thread (T_binance_md)
    std::thread binance_md_thread;
    if (cfg.binance_md_enabled && binance_md_client) {
        binance_md_client->set_on_message(
            [&binance_md_parser, &binance_md_queue, &metrics](
                std::string_view payload, lt::Timestamp_ns recv_ts) {
                metrics.inc(lt::MetricId::BINANCE_MD_MESSAGES);

                lt::BinanceMarketUpdate upd;
                auto err = binance_md_parser->parse(payload, recv_ts, upd);

                if (err == lt::ErrorCode::OK) {
                    // Stamp wall-clock at receive time so the scheduler can
                    // compute exchange-to-recv latency for trade frames.
                    // The lag from binance_ws_client::do_read returning to
                    // here is microseconds and dwarfed by network latency,
                    // so this is accurate enough for ms-scale measurement.
                    upd.recv_wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    metrics.inc(lt::MetricId::BINANCE_MD_PARSE_OK);
                    switch (static_cast<lt::BinanceStreamType>(upd.type)) {
                    case lt::BinanceStreamType::BOOK_TICKER:
                        metrics.inc(lt::MetricId::BINANCE_MD_BOOK_TICKERS);
                        break;
                    case lt::BinanceStreamType::TRADE:
                    case lt::BinanceStreamType::AGG_TRADE:
                        metrics.inc(lt::MetricId::BINANCE_MD_TRADES);
                        break;
                    default: break;
                    }
                    if (binance_md_queue) {
                        if (binance_md_queue->try_push(upd)) {
                            metrics.inc(lt::MetricId::BINANCE_MD_QUEUE_PUSHES);
                        } else {
                            metrics.inc(lt::MetricId::BINANCE_MD_QUEUE_DROPS);
                        }
                    }
                } else if (err == lt::ErrorCode::UNKNOWN_EVENT_TYPE) {
                    metrics.inc(lt::MetricId::BINANCE_MD_UNKNOWN);
                } else {
                    metrics.inc(lt::MetricId::BINANCE_MD_PARSE_FAIL);
                }
            });

        binance_md_client->set_on_connected([&main_log]() {
            lt::AsyncLogger::log(main_log, lt::LogLevel::INFO,
                                  "Binance MD WebSocket connected");
        });

        binance_md_client->set_on_disconnected(
            [&main_log, &metrics](const std::string& reason) {
                metrics.inc(lt::MetricId::BINANCE_MD_RECONNECTS);
                char buf[lt::LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                              "Binance MD WebSocket disconnected: %s", reason.c_str());
                lt::AsyncLogger::log(main_log, lt::LogLevel::WARN, buf);
            });

        // In-band status sentinels: pushed through the same SPSC queue as data
        // frames so the strategy sees connect/disconnect ordered with the data
        // stream. The strategy callback (Strategy::on_binance_update) is a
        // base no-op today; sentinels become meaningful when the strategy is
        // updated to consume Binance data in a follow-up.
        binance_md_client->set_on_status(
            [&binance_md_queue, &metrics](
                lt::BinanceUpdateKind kind, const std::string& reason) {
                lt::BinanceMarketUpdate sentinel{};
                sentinel.kind = static_cast<uint8_t>(kind);
                sentinel.recv_ts = lt::SteadyClock::now();
                if (binance_md_queue) {
                    if (binance_md_queue->try_push(sentinel)) {
                        metrics.inc(lt::MetricId::BINANCE_MD_QUEUE_PUSHES);
                    } else {
                        metrics.inc(lt::MetricId::BINANCE_MD_QUEUE_DROPS);
                    }
                }
                (void)reason;  // human-readable reason already logged in on_disconnected
            });

        auto* bptr = binance_md_client.get();
        binance_md_thread = std::thread([bptr, &cfg]() {
            if (cfg.pin_threads) lt::pin_thread_to_core(cfg.pin_core_binance_md);
            bptr->run();
        });
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Binance MD thread started");
    }

    // Start periodic metrics dump in a background thread
    std::thread metrics_thread([&metrics, &cfg]() {
        while (!g_shutdown.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg.metrics_dump_interval_ms));
            if (!g_shutdown.load(std::memory_order_relaxed)) {
                metrics.dump_to_file(cfg.metrics_file);
            }
        }
    });

    // Rotation coordinator (optional, only created when rotation_enabled)
    std::unique_ptr<lt::RotationCoordinator> rotation_coordinator;

    // Slot manager (optional, only created when slot_manager_enabled)
    std::unique_ptr<lt::MarketSlotManager> slot_manager;
    // T7->T2 slot event queue (separate from control_queue to preserve SPSC invariant)
    std::unique_ptr<lt::SpscQueue<lt::SchedulerEvent>> slot_queue;

    // Keep signal-handler work async-signal-safe: the handler only flips a flag.
    // Also monitor g_fatal so a scheduler exception triggers full shutdown.
    std::thread shutdown_thread([&]() {
        while (!g_shutdown.load(std::memory_order_relaxed) &&
               !g_fatal.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (g_fatal.load(std::memory_order_relaxed)) {
            g_shutdown.store(true, std::memory_order_relaxed);
        }
        // Request shutdown of managed components
        stop_market_ws();
        stop_user_ws();
        if (cfg.gateway_enabled && gateway) {
            gateway->request_shutdown();
        }
        if (cfg.inventory_service_enabled && inventory_service) {
            inventory_service->request_shutdown();
        }
        if (cfg.rtds_enabled && rtds_client) {
            rtds_client->request_shutdown();
        }
        if (cfg.binance_md_enabled && binance_md_client) {
            binance_md_client->request_shutdown();
        }
        if (cfg.ui_bridge_enabled && ipc_bridge) {
            ipc_bridge->request_shutdown();
        }
        if (rotation_coordinator) {
            rotation_coordinator->request_shutdown();
        }
        if (slot_manager) {
            slot_manager->request_shutdown();
        }
    });

    std::cout << "Starting...\n";

    // Wrap runtime (rotation/non-rotation run loop) in try-catch so that
    // any exception sets g_shutdown and falls through to the shutdown cascade
    // instead of calling std::terminate with unjoinable threads.
    try {

    if (cfg.slot_manager_enabled) {
        // --- SLOT MANAGER MODE ---
        // T7 (main thread) runs the multi-timeframe slot manager.
        // T0/T1 are started by the manager once initial discovery completes.

        lt::SlotManagerConfig sm_cfg;
        sm_cfg.discovery_poll_ms = cfg.rotation_discovery_poll_ms;
        sm_cfg.pre_subscribe_ms = cfg.slot_pre_subscribe_ms;
        sm_cfg.cancel_lead_ms = cfg.slot_cancel_lead_ms;
        sm_cfg.no_trade_start_ms = cfg.slot_no_trade_start_ms;
        sm_cfg.no_trade_end_ms = cfg.slot_no_trade_end_ms;
        sm_cfg.discovery_api_url = cfg.discovery_api_url;
        sm_cfg.enable_5m = cfg.slot_enable_5m;
        sm_cfg.enable_15m = cfg.slot_enable_15m;

        slot_queue = std::make_unique<lt::SpscQueue<lt::SchedulerEvent>>(64);
        slot_manager = std::make_unique<lt::MarketSlotManager>(
            sm_cfg, *slot_queue, logger, g_shutdown, g_fatal);

        // Wire slot queue to scheduler
        scheduler.set_slot_queue(slot_queue.get());

        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Starting slot manager (T7)");

        slot_manager->run({
            .subscribe_market_ws = [&](const std::vector<std::string>& token_ids) {
                if (ws_client) ws_client->send_subscribe_add(token_ids);
            },
            .unsubscribe_market_ws = [&](const std::vector<std::string>& token_ids) {
                if (ws_client) ws_client->send_unsubscribe(token_ids);
            },
            .subscribe_user_ws = [&](const std::vector<std::string>& condition_ids) {
                if (user_ws_client) user_ws_client->send_subscribe_add(condition_ids);
            },
            .unsubscribe_user_ws = [&](const std::vector<std::string>& condition_ids) {
                if (user_ws_client) user_ws_client->send_unsubscribe(condition_ids);
            },
            .start_market_ws = start_market_ws,
            .start_user_ws = start_user_ws,
            .register_market = [&](const std::string& c, const std::string& u,
                                    const std::string& d, bool neg_risk,
                                    uint16_t fee_rate_bps) {
                lt::MarketPair pair;
                pair.condition_id = lt::AssetId(c);
                pair.token_id_up = lt::AssetId(u);
                pair.token_id_down = lt::AssetId(d);
                pair.neg_risk = neg_risk;
                pair.fee_rate_bps = fee_rate_bps;
                state_store.register_down_token(lt::AssetId(d));
                pnl_tracker.register_pair(lt::AssetId(u), lt::AssetId(d));
                return market_pair_registry.add_pair(pair);
            },
            .register_token = [&](const std::string& id) {
                token_inventory->register_token(lt::AssetId(id));
            },
            .seed_market_state = [&](const std::string& id) {
                state_store.seed_asset(lt::AssetId(id));
            },
            .seed_scheduler_state = [&](const std::string& id) {
                scheduler.seed_asset_state(lt::AssetId(id));
                // Only seed shadow books for UP tokens (DOWN filtered at T0)
                if (!state_store.is_down_token(lt::AssetId(id))) {
                    scheduler.seed_strategy_book(lt::AssetId(id));
                }
            },
            .seed_strategy_book = [&](const std::string& id) {
                if (!state_store.is_down_token(lt::AssetId(id))) {
                    scheduler.seed_strategy_book(lt::AssetId(id));
                }
            },
            .bootstrap_positions = bootstrap_positions_cb,
            .fire_redeem = [&](const std::string& condition_id,
                               const std::string& token_id_up,
                               const std::string& token_id_down) -> bool {
                if (!inventory_op_sink) return false;
                lt::InventoryOpRequest req;
                req.type = lt::InventoryOpType::REDEEM;
                req.condition_id = lt::AssetId(condition_id);
                req.token_id_up = lt::AssetId(token_id_up);
                req.token_id_down = lt::AssetId(token_id_down);
                req.quantity = 0;
                return inventory_op_sink->try_request(req);
            },
            .update_ui_slot = [&](lt::SlotName slot, const std::string& cond,
                                   int64_t ws, int64_t we, lt::SlotPhase phase) {
                if (ipc_bridge) {
                    ipc_bridge->set_rotation_info(cond, ws, we, 0,
                                                   phase == lt::SlotPhase::CLOSING);
                }
            },
        });
        // slot_manager->run() blocks until shutdown

    } else if (cfg.rotation_enabled) {
        // --- ROTATION MODE ---
        // T7 (main thread) runs the rotation coordinator.
        // T0/T1 are started/stopped by the coordinator via callbacks.

        lt::RotationConfig rot_cfg;
        rot_cfg.timeframe = static_cast<lt::BtcTimeframe>(cfg.rotation_timeframe);
        rot_cfg.discovery_poll_ms = cfg.rotation_discovery_poll_ms;
        rot_cfg.pre_rotation_ms = cfg.rotation_pre_rotation_ms;
        rot_cfg.no_trade_start_ms = cfg.rotation_no_trade_start_ms;
        rot_cfg.no_trade_end_ms = cfg.rotation_no_trade_end_ms;
        rot_cfg.discovery_api_url = cfg.discovery_api_url;

        rotation_coordinator = std::make_unique<lt::RotationCoordinator>(
            rot_cfg, logger, g_shutdown, g_fatal);

        // Wire rotation coordinator to scheduler
        scheduler.set_rotation_coordinator(rotation_coordinator.get());

        // Set initial timing context on strategy before run loop
        // (coordinator will update it after first discovery)

        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Starting rotation coordinator (T7)");

        rotation_coordinator->run({
            .stop_market_ws = stop_market_ws,
            .stop_user_ws = stop_user_ws,
            .start_market_ws = start_market_ws,
            .start_user_ws = start_user_ws,
            .register_market = [&](const std::string& c, const std::string& u,
                                    const std::string& d, bool neg_risk,
                                    uint16_t fee_rate_bps) {
                lt::MarketPair pair;
                pair.condition_id = lt::AssetId(c);
                pair.token_id_up = lt::AssetId(u);
                pair.token_id_down = lt::AssetId(d);
                pair.neg_risk = neg_risk;
                pair.fee_rate_bps = fee_rate_bps;
                state_store.register_down_token(lt::AssetId(d));
                pnl_tracker.register_pair(lt::AssetId(u), lt::AssetId(d));
                return market_pair_registry.add_pair(pair);
            },
            .register_token = [&](const std::string& id) {
                token_inventory->register_token(lt::AssetId(id));
            },
            .seed_market_state = [&](const std::string& id) {
                state_store.seed_asset(lt::AssetId(id));
            },
            .seed_scheduler_state = [&](const std::string& id) {
                scheduler.seed_asset_state(lt::AssetId(id));
                // Only seed shadow books for UP tokens (DOWN filtered at T0)
                if (!state_store.is_down_token(lt::AssetId(id))) {
                    scheduler.seed_strategy_book(lt::AssetId(id));
                }
            },
            .update_ui_rotation = [&](const std::string& cond, int64_t ws, int64_t we,
                                       int count, bool no_trade) {
                if (ipc_bridge) {
                    ipc_bridge->set_rotation_info(cond, ws, we, count, no_trade);
                }
            },
            .bootstrap_positions = bootstrap_positions_cb,
            .fire_redeem = [&](const std::string& condition_id,
                               const std::string& token_id_up,
                               const std::string& token_id_down) -> bool {
                if (!inventory_op_sink) return false;
                lt::InventoryOpRequest req;
                req.type = lt::InventoryOpType::REDEEM;
                req.condition_id = lt::AssetId(condition_id);
                req.token_id_up = lt::AssetId(token_id_up);
                req.token_id_down = lt::AssetId(token_id_down);
                req.quantity = 0;  // redeem all available
                return inventory_op_sink->try_request(req);
            },
            .is_exec_queue_drained = [&]() -> bool {
                return !strategy_to_exec_queue || strategy_to_exec_queue->size() == 0;
            },
        });
        // coordinator.run() blocks until shutdown

    } else {
        // --- NON-ROTATION MODE ---
        // Start T0/T1 with config-defined IDs, wait for shutdown.

        start_market_ws(market_ws_asset_ids);
        lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "Market WS thread (T0) started");

        if (cfg.user_ws_enabled) {
            start_user_ws(user_ws_market_ids);
            lt::AsyncLogger::log(main_log, lt::LogLevel::INFO, "User WS thread (T1) started");
        }

        // Wait for shutdown signal
        while (!g_shutdown.load(std::memory_order_relaxed) &&
               !g_fatal.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    // Clear secrets after rotation is done (if they weren't cleared earlier)
    if (account) {
        account->clear_secrets();
    }

    } catch (const std::exception& ex) {
        std::cerr << "FATAL exception: " << ex.what() << "\n";
        g_shutdown.store(true, std::memory_order_relaxed);
    } catch (...) {
        std::cerr << "FATAL unknown exception\n";
        g_shutdown.store(true, std::memory_order_relaxed);
    }

    std::cout << "\nShutting down...\n";

    // Hard exit deadline: if graceful shutdown takes too long (threads stuck in
    // blocking HTTPS calls), force-exit to avoid requiring pkill.
    std::thread exit_watchdog([]() {
        std::this_thread::sleep_for(std::chrono::seconds(15));
        std::fprintf(stderr, "Shutdown timed out after 15s — forcing exit\n");
        std::_Exit(1);
    });
    exit_watchdog.detach();

    // Shutdown cascade (unified for both rotation and non-rotation paths)
    // T0 -> T_rtds -> T6 -> T1 -> T2 -> T3 -> T_rec -> background -> T5
    // T_rec must stop after all recording producers: T0 (market), T1 (user), T_rtds

    stop_market_ws();

    // Stop RTDS (producer for rtds_queue) before T2 (consumer)
    if (cfg.rtds_enabled && rtds_client) {
        rtds_client->request_shutdown();
        if (rtds_thread.joinable()) rtds_thread.join();
    }

    // Stop Binance MD (producer for binance_md_queue) before T2 (consumer)
    if (cfg.binance_md_enabled && binance_md_client) {
        binance_md_client->request_shutdown();
        if (binance_md_thread.joinable()) binance_md_thread.join();
    }

    if (cfg.ui_bridge_enabled && ipc_bridge) {
        ipc_bridge->request_shutdown();
        if (ipc_bridge_thread.joinable()) ipc_bridge_thread.join();
    }

    stop_user_ws();

    if (cfg.scheduler_enabled) {
        scheduler.request_shutdown();
        if (scheduler_thread.joinable()) scheduler_thread.join();
    }

    // Stop T3 (gateway) after T2 — drain remaining intents
    if (cfg.gateway_enabled && gateway) {
        gateway->request_shutdown();
        if (gateway_thread.joinable()) gateway_thread.join();
    }

    if (cfg.inventory_service_enabled && inventory_service) {
        inventory_service->request_shutdown();
        if (inventory_service_thread.joinable()) inventory_service_thread.join();
    }

    // Stop recorder after producers (RTDS) have stopped
    if ((cfg.recording_enabled || cfg.journal_enabled) && data_recorder) {
        data_recorder->request_shutdown();
        if (recorder_thread.joinable()) recorder_thread.join();
    }

    g_shutdown.store(true);
    if (shutdown_thread.joinable()) shutdown_thread.join();
    if (metrics_thread.joinable()) metrics_thread.join();

    // Final metrics dump
    std::cout << metrics.dump();
    metrics.dump_to_file(cfg.metrics_file);

    logger.stop();

    std::cout << "Engine stopped.\n";
    return 0;
}
