// Boost.Asio includes Windows headers that #define ERROR.
// Must include Boost first, then #undef ERROR before our headers that use LogLevel::ERROR.
#include <boost/asio.hpp>
#include <boost/beast.hpp>

#ifdef ERROR
#undef ERROR
#endif

#include "ui_bridge/ipc_bridge.h"

#include <cstdio>
#include <optional>
#include <thread>
#include <unordered_map>

#include "common/clock.h"
#include "common/pnl_tracker.h"
#include "common/token_inventory.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "ui_bridge/account_poll_service.h"
#include "ui_bridge/btc_series_registry.h"
#include "ui_bridge/ui_command_parser.h"
#include "ui_bridge/ui_serializer.h"
#include "ui_bridge/watch_manager.h"
#include "ui_bridge/watcher_book_store.h"
#include "ui_bridge/watcher_service.h"

namespace lt {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// Detect tick size from book snapshot prices via GCD.
// Returns a standard tick (100, 500, 1000) or 100 (0.01) as default.
static TickSize_t detect_tick_from_prices(const Price_t* prices, int count) {
    if (count < 2) return 100;
    auto gcd = [](Price_t a, Price_t b) -> Price_t {
        while (b) { Price_t t = b; b = a % b; a = t; }
        return a;
    };
    Price_t g = 0;
    for (int i = 0; i < count; ++i) {
        if (prices[i] > 0) g = g == 0 ? prices[i] : gcd(g, prices[i]);
    }
    // Accept standard Polymarket tick sizes: 0.01 (100), 0.05 (500), 0.10 (1000)
    if (g >= 1000 && g % 1000 == 0) return 1000;
    if (g >= 500 && g % 500 == 0) return 500;
    return 100;
}

struct IpcBridge::Impl {
    Impl(SpscQueue<UiBookUpdate>& book_queue,
         SpscQueue<UiStateSnapshot>& state_queue,
         SpscQueue<SchedulerEvent>& control_queue,
         Metrics& metrics,
         AsyncLogger& logger,
         const IpcBridgeConfig& config,
         const MarketPairRegistry* market_pairs,
         const std::vector<MarketPairConfig>* market_pair_configs,
         std::atomic<bool>* fatal_flag)
        : book_queue_(book_queue),
          state_queue_(state_queue),
          control_queue_(control_queue),
          metrics_(metrics),
          logger_(logger),
          log_handle_(logger.create_producer("ipc_bridge")),
          config_(config),
          market_pairs_(market_pairs),
          market_pair_configs_(market_pair_configs),
          fatal_flag_(fatal_flag),
          ioc_(),
          acceptor_(ioc_),
          snapshot_timer_(ioc_) {}

    void run();
    void request_shutdown();
    void set_engine_config(const EngineConfig& engine_config);
    void set_account_info(const std::string& name, const std::string& address);
    void set_account_credentials(const std::string& api_key,
                                  const std::string& api_secret,
                                  const std::string& api_passphrase,
                                  const std::string& address);
    void set_rotation_info(const std::string& condition_id,
                           int64_t window_start, int64_t window_end,
                           int rotation_count, bool in_no_trade);
    void set_token_inventory(TokenInventory* inv) { token_inventory_ = inv; }
    void set_pnl_tracker(const PnlTracker* t) { pnl_tracker_ = t; }

private:
    void start_accept();
    void on_accept(beast::error_code ec, tcp::socket socket);
    void start_read();
    void on_read(beast::error_code ec, std::size_t bytes);
    void start_snapshot_timer();
    void on_snapshot_tick(beast::error_code ec);
    void drain_queues();
    void assemble_and_send();
    void send_snapshot(const std::string& json);
    void send_message(const std::string& json);
    void on_write(beast::error_code ec, std::size_t bytes);
    void close_session();

    // Watcher methods
    void handle_watch_command(const WatchCommand& cmd);
    void start_watcher_timer();
    void on_watcher_tick(beast::error_code ec);
    void on_watcher_book_data(const MarketEvent& event);
    void on_watcher_ws_connected();
    void on_watcher_ws_disconnected(const std::string& reason);
    void on_discovery_result(BtcTimeframe tf, SeriesMarketInfo current,
                             std::optional<SeriesMarketInfo> next);
    void execute_rollover(BtcTimeframe tf);
    void send_watcher_snapshots();
    void send_series_list();
    void send_watcher_status(BtcTimeframe tf);
    void refresh_watcher_subscriptions();

    SpscQueue<UiBookUpdate>& book_queue_;
    SpscQueue<UiStateSnapshot>& state_queue_;
    SpscQueue<SchedulerEvent>& control_queue_;
    Metrics& metrics_;
    AsyncLogger& logger_;
    ProducerHandle log_handle_;
    IpcBridgeConfig config_;
    const MarketPairRegistry* market_pairs_;
    const std::vector<MarketPairConfig>* market_pair_configs_;
    std::atomic<bool>* fatal_flag_;

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    net::steady_timer snapshot_timer_;

    // Single WS session (only one UI client at a time)
    std::unique_ptr<websocket::stream<beast::tcp_stream>> ws_;
    bool connected_ = false;
    bool write_in_progress_ = false;
    bool snapshot_pending_ = false;  // set when a snapshot arrived during write_in_progress_
    std::shared_ptr<std::string> pending_snapshot_buf_;  // latest-only snapshot buffer
    beast::flat_buffer read_buf_;
    std::vector<std::shared_ptr<std::string>> write_queue_;  // pending non-snapshot messages

    // Latest data from queues (latest-only semantics)
    std::unordered_map<AssetId, UiBookUpdate, AssetIdHash> latest_books_;
    UiStateSnapshot latest_state_{};
    bool has_state_ = false;

    // Watcher state (all T6-owned)
    WatchManager watch_manager_;
    BtcSeriesRegistry series_registry_;
    WatcherBookStore watcher_books_;
    std::unique_ptr<WatcherService> watcher_service_;
    net::steady_timer watcher_timer_{ioc_};
    std::optional<EngineConfig> engine_config_;

    // Trade accumulation between watcher ticks (per timeframe)
    std::vector<WatcherBookLevel> watcher_pending_trades_[kBtcTimeframeCount];
    // Current tick size per timeframe (default 100 = 0.01)
    TickSize_t watcher_tick_size_[kBtcTimeframeCount] = {100, 100};

    std::atomic<bool> stop_requested_{false};

    // Account identity (stable strings, set before run())
    std::string account_name_;
    std::string account_address_;

    // Rotation info (updated by T7 via post to T6's io_context)
    std::string rotation_condition_;
    int64_t rotation_window_start_ = 0;
    int64_t rotation_window_end_ = 0;
    int rotation_count_ = 0;
    bool rotation_in_no_trade_ = false;

    // Account positions value polling
    std::unique_ptr<AccountPollService> account_poll_service_;
    std::string acct_address_;

    // Latest poll results (T6-owned, updated via post from poll thread)
    double position_value_ = 0.0;
    double pol_balance_ = 0.0;
    bool has_account_data_ = false;
    std::string account_poll_error_;

    // Token inventory (shared atomic read/write for USDC balance)
    TokenInventory* token_inventory_ = nullptr;
    const PnlTracker* pnl_tracker_ = nullptr;
};

void IpcBridge::Impl::run() {
    try {
        auto addr = net::ip::make_address(config_.bind_address);
        auto endpoint = tcp::endpoint(addr, static_cast<unsigned short>(config_.ws_port));
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(net::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(1);

        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "IPC Bridge listening on port %d", config_.ws_port);
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        start_accept();
        start_snapshot_timer();

        // Start watcher service if enabled
        if (config_.watcher_enabled && engine_config_) {
            watcher_service_ = std::make_unique<WatcherService>(logger_, *engine_config_);

            // Set callbacks — post results to T6's io_context
            watcher_service_->set_on_book_data([this](const MarketEvent& event) {
                net::post(ioc_, [this, event]() { on_watcher_book_data(event); });
            });
            watcher_service_->set_on_ws_connected([this]() {
                net::post(ioc_, [this]() { on_watcher_ws_connected(); });
            });
            watcher_service_->set_on_ws_disconnected([this](const std::string& reason) {
                net::post(ioc_, [this, reason]() { on_watcher_ws_disconnected(reason); });
            });
            watcher_service_->set_on_discovery([this](BtcTimeframe tf,
                                                       SeriesMarketInfo current,
                                                       std::optional<SeriesMarketInfo> next) {
                net::post(ioc_, [this, tf, current, next]() {
                    on_discovery_result(tf, current, next);
                });
            });

            watcher_service_->start();
            start_watcher_timer();
            AsyncLogger::log(log_handle_, LogLevel::INFO, "Watcher service started");
        }

        // Start positions value + on-chain balance polling if account address is available
        std::string poll_address = !acct_address_.empty() ? acct_address_ : account_address_;
        if (!poll_address.empty()) {
            try {
                account_poll_service_ = std::make_unique<AccountPollService>(
                    "data-api.polymarket.com",
                    poll_address,
                    config_.account_poll_interval_ms,
                    config_.polygon_rpc_url,
                    poll_address,
                    config_.balance_poll_interval_ms,
                    token_inventory_,
                    config_.eoa_address);

                account_poll_service_->set_on_result(
                    [this](const AccountPollResult& result) {
                        net::post(ioc_, [this, result]() {
                            if (result.value_ok) {
                                position_value_ = result.position_value;
                            }
                            if (result.pol_ok) {
                                pol_balance_ = result.pol_balance;
                            }
                            has_account_data_ = result.value_ok || result.balance_ok;
                            account_poll_error_ = result.error;
                        });
                    });

                account_poll_service_->start();
                AsyncLogger::log(log_handle_, LogLevel::INFO,
                                 "Account polling started (value + on-chain balance)");
            } catch (const std::exception& ex) {
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                              "Account poll service init failed: %s", ex.what());
                AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            }
        }

        ioc_.run();
    } catch (const std::exception& ex) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "IPC Bridge fatal error: %s", ex.what());
        AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
    }

    // Stop account poll service
    if (account_poll_service_) {
        account_poll_service_->stop();
        account_poll_service_.reset();
    }

    // Stop watcher service
    if (watcher_service_) {
        watcher_service_->stop();
        watcher_service_.reset();
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "IPC Bridge stopped");
}

void IpcBridge::Impl::set_engine_config(const EngineConfig& engine_config) {
    engine_config_ = engine_config;
}

void IpcBridge::Impl::set_account_info(const std::string& name, const std::string& address) {
    account_name_ = name;
    account_address_ = address;
}

void IpcBridge::Impl::set_account_credentials(const std::string& api_key,
                                               const std::string& api_secret,
                                               const std::string& api_passphrase,
                                               const std::string& address) {
    (void)api_key;
    (void)api_secret;
    (void)api_passphrase;
    acct_address_ = address;
}

void IpcBridge::Impl::set_rotation_info(const std::string& condition_id,
                                         int64_t window_start, int64_t window_end,
                                         int rotation_count, bool in_no_trade) {
    net::post(ioc_, [this, condition_id, window_start, window_end, rotation_count, in_no_trade]() {
        rotation_condition_ = condition_id;
        rotation_window_start_ = window_start;
        rotation_window_end_ = window_end;
        rotation_count_ = rotation_count;
        rotation_in_no_trade_ = in_no_trade;
    });
}

void IpcBridge::Impl::request_shutdown() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (account_poll_service_) {
        account_poll_service_->stop();
    }
    net::post(ioc_, [this]() {
        beast::error_code ec;
        acceptor_.close(ec);
        snapshot_timer_.cancel();
        watcher_timer_.cancel();
        if (watcher_service_) {
            watcher_service_->stop();
        }
        close_session();
        ioc_.stop();
    });
}

void IpcBridge::Impl::start_accept() {
    acceptor_.async_accept(
        [this](beast::error_code ec, tcp::socket socket) {
            on_accept(ec, std::move(socket));
        });
}

void IpcBridge::Impl::on_accept(beast::error_code ec, tcp::socket socket) {
    if (ec) {
        if (stop_requested_.load(std::memory_order_relaxed)) return;
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "IPC accept error: %s", ec.message().c_str());
        AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        start_accept();
        return;
    }

    // Close existing session if any
    if (connected_) {
        close_session();
    }

    ws_ = std::make_unique<websocket::stream<beast::tcp_stream>>(std::move(socket));

    // Disable Nagle's algorithm on accepted connection
    beast::get_lowest_layer(*ws_).socket().set_option(tcp::no_delay(true));

    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));

    // If auth token is configured, validate it from the upgrade request
    if (!config_.auth_token.empty()) {
        // Read the HTTP upgrade request first
        auto req = std::make_shared<http::request<http::string_body>>();
        auto buf = std::make_shared<beast::flat_buffer>();
        http::async_read(beast::get_lowest_layer(*ws_), *buf, *req,
            [this, req, buf](beast::error_code ec2, std::size_t) {
                if (ec2) {
                    ws_.reset();
                    start_accept();
                    return;
                }

                // Check auth token from query string: ws://host:port/?token=XXX
                bool auth_ok = false;
                auto target = std::string(req->target());
                auto qpos = target.find("token=");
                if (qpos != std::string::npos) {
                    auto token_start = qpos + 6;
                    auto token_end = target.find('&', token_start);
                    auto token = (token_end != std::string::npos)
                        ? target.substr(token_start, token_end - token_start)
                        : target.substr(token_start);
                    auth_ok = (token == config_.auth_token);
                }

                if (!auth_ok) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "IPC: rejected connection (invalid auth token)");
                    // Send 4001 close and reset
                    ws_->async_accept(*req, [this](beast::error_code) {
                        if (ws_) {
                            ws_->async_close(websocket::close_reason(static_cast<websocket::close_code>(4001), "unauthorized"),
                                [this](beast::error_code) {
                                    ws_.reset();
                                    start_accept();
                                });
                        }
                    });
                    return;
                }

                // Auth passed — accept the WebSocket upgrade
                ws_->async_accept(*req, [this](beast::error_code ec3) {
                    if (ec3) {
                        char buf[LogEntry::kMaxMsg];
                        std::snprintf(buf, sizeof(buf), "IPC WS accept error: %s",
                                      ec3.message().c_str());
                        AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
                        ws_.reset();
                        start_accept();
                        return;
                    }
                    connected_ = true;
                    write_in_progress_ = false;
                    metrics_.inc(MetricId::UI_WS_CONNECTED);
                    AsyncLogger::log(log_handle_, LogLevel::INFO,
                                     "UI client connected (authenticated)");
                    start_read();
                });
            });
    } else {
        // No auth — accept directly (backward compatible for local dev)
        ws_->async_accept([this](beast::error_code ec2) {
            if (ec2) {
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf), "IPC WS accept error: %s",
                              ec2.message().c_str());
                AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
                ws_.reset();
                start_accept();
                return;
            }
            connected_ = true;
            write_in_progress_ = false;
            metrics_.inc(MetricId::UI_WS_CONNECTED);
            AsyncLogger::log(log_handle_, LogLevel::INFO, "UI client connected");
            start_read();
        });
    }
}

void IpcBridge::Impl::start_read() {
    if (!ws_ || !connected_) return;
    read_buf_.clear();
    ws_->async_read(read_buf_, [this](beast::error_code ec, std::size_t bytes) {
        on_read(ec, bytes);
    });
}

void IpcBridge::Impl::on_read(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        if (ec == websocket::error::closed || ec == net::error::eof ||
            ec == net::error::connection_reset) {
            AsyncLogger::log(log_handle_, LogLevel::INFO, "UI client disconnected");
        } else {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "IPC read error: %s", ec.message().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        }
        close_session();
        start_accept();
        return;
    }

    // Parse command
    auto data = read_buf_.data();
    std::string_view json(static_cast<const char*>(data.data()), data.size());

    // Check for shutdown command: {"cmd":"shutdown"}
    if (json.find("\"shutdown\"") != std::string_view::npos &&
        json.find("\"cmd\"") != std::string_view::npos) {
        AsyncLogger::log(log_handle_, LogLevel::INFO, "Shutdown command received from UI");
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
        start_read();
        return;
    }

    // Check if this is a watcher command first
    if (is_watch_command(json)) {
        auto wcmd = parse_watch_command(json);
        if (wcmd.valid) {
            handle_watch_command(wcmd);
        } else {
            metrics_.inc(MetricId::UI_COMMANDS_INVALID);
        }
        start_read();
        return;
    }

    auto cmd = parse_ui_command(json);
    if (cmd.valid) {
        metrics_.inc(MetricId::UI_COMMANDS_RECEIVED);
        bool pushed = false;
        // Bounded retry: try a few times, then drop rather than blocking
        // the entire io_context (which freezes WS reads, snapshot timer,
        // and watcher callbacks on T6's single-threaded event loop).
        constexpr int kMaxPushRetries = 8;
        for (int attempt = 0; attempt < kMaxPushRetries; ++attempt) {
            if (stop_requested_.load(std::memory_order_relaxed)) break;
            if (control_queue_.try_push(cmd.event)) {
                pushed = true;
                break;
            }
            // Brief yield between retries — bounded, not infinite
            std::this_thread::yield();
        }
        if (!pushed) {
            metrics_.inc(MetricId::UI_COMMANDS_DROPPED);
            if (stop_requested_.load(std::memory_order_relaxed)) {
                AsyncLogger::log(log_handle_, LogLevel::ERROR,
                                 "Control queue unavailable during shutdown, command dropped");
            } else {
                AsyncLogger::log(log_handle_, LogLevel::WARN,
                                 "Control queue full after bounded retry, command dropped");
            }
        }
    } else {
        metrics_.inc(MetricId::UI_COMMANDS_INVALID);
        char buf[LogEntry::kMaxMsg];
        auto cmd_len = json.size() < 200 ? json.size() : 200;
        std::snprintf(buf, sizeof(buf), "Invalid UI command: %.*s",
                      static_cast<int>(cmd_len), json.data());
        AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
    }

    start_read();
}

void IpcBridge::Impl::start_snapshot_timer() {
    int interval_ms = 1000 / config_.snapshot_rate_hz;
    if (interval_ms < 1) interval_ms = 1;

    snapshot_timer_.expires_after(std::chrono::milliseconds(interval_ms));
    snapshot_timer_.async_wait([this](beast::error_code ec) {
        on_snapshot_tick(ec);
    });
}

void IpcBridge::Impl::on_snapshot_tick(beast::error_code ec) {
    if (ec || stop_requested_.load(std::memory_order_relaxed)) return;

    drain_queues();
    assemble_and_send();
    start_snapshot_timer();
}

void IpcBridge::Impl::drain_queues() {
    // Drain book queue — keep latest per asset_id
    while (auto* item = book_queue_.front()) {
        latest_books_[item->asset_id] = *item;
        book_queue_.pop();
    }

    // Drain state queue — keep latest only
    while (auto* item = state_queue_.front()) {
        latest_state_ = *item;
        has_state_ = true;
        state_queue_.pop();
    }
}

void IpcBridge::Impl::assemble_and_send() {
    if (!connected_ || write_in_progress_) {
        if (connected_) {
            snapshot_pending_ = true;  // recover on next on_write() completion
            metrics_.inc(MetricId::UI_SNAPSHOTS_DROPPED);
        }
        return;
    }
    snapshot_pending_ = false;

    EngineSnapshot snap;
    snap.timestamp_ns = SteadyClock::now();

    // Build market snapshots from static config plus dynamic watcher discovery.
    // This keeps UI market/position panels populated in both fixed and rotation modes.
    struct MarketSeed {
        AssetId condition_id;
        AssetId token_id_up;
        AssetId token_id_down;
        const char* series_label = "";
    };

    auto timeframe_label = [](BtcTimeframe tf) -> const char* {
        switch (tf) {
            case BtcTimeframe::BTC_5M: return "BTC 5M";
            case BtcTimeframe::BTC_15M: return "BTC 15M";
        }
        return "";
    };

    std::vector<MarketSeed> seeds;
    seeds.reserve((market_pair_configs_ ? market_pair_configs_->size() : 0) +
                  (kBtcTimeframeCount * 2));

    auto add_seed = [&](const AssetId& condition_id,
                        const AssetId& token_id_up,
                        const AssetId& token_id_down,
                        const char* series_label) {
        if (condition_id.len == 0 || token_id_up.len == 0 || token_id_down.len == 0) return;

        for (auto& seed : seeds) {
            if (seed.condition_id == condition_id) {
                if ((seed.series_label == nullptr || seed.series_label[0] == '\0') &&
                    series_label && series_label[0] != '\0') {
                    seed.series_label = series_label;
                }
                return;
            }
        }

        MarketSeed seed;
        seed.condition_id = condition_id;
        seed.token_id_up = token_id_up;
        seed.token_id_down = token_id_down;
        seed.series_label = (series_label && series_label[0] != '\0') ? series_label : "";
        seeds.push_back(seed);
    };

    if (market_pair_configs_) {
        for (const auto& pair_cfg : *market_pair_configs_) {
            add_seed(AssetId(pair_cfg.condition_id),
                     AssetId(pair_cfg.token_id_up),
                     AssetId(pair_cfg.token_id_down),
                     "");
        }
    }

    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        auto tf = static_cast<BtcTimeframe>(i);
        const char* label = timeframe_label(tf);

        const auto* cur = series_registry_.current(tf);
        if (cur && !cur->empty()) {
            add_seed(AssetId(cur->condition_id),
                     AssetId(cur->token_id_up),
                     AssetId(cur->token_id_down),
                     label);
        }
        // Note: next market is intentionally NOT added to the engine snapshot.
        // It's a pre-registration for smooth rolling, not a market the user
        // is actively trading. Including it causes duplicate rows in positions.
    }

    for (const auto& seed : seeds) {
        UiMarketSnapshot mkt;
        mkt.condition_id = seed.condition_id;
        mkt.token_id_up = seed.token_id_up;
        mkt.token_id_down = seed.token_id_down;
        mkt.series_label = seed.series_label;

        auto it_up = latest_books_.find(seed.token_id_up);
        if (it_up != latest_books_.end()) {
            mkt.book_up = &it_up->second;
        }
        // book_down intentionally left nullptr — DOWN book events are
        // filtered at T0; only the UP book is maintained.

        // Positions routed through T2 state snapshot (no direct T1 reads)
        if (has_state_) {
            for (int pi = 0; pi < latest_state_.position_count; ++pi) {
                if (latest_state_.positions[pi].token_id == seed.token_id_up) {
                    mkt.position_up = latest_state_.positions[pi].position;
                } else if (latest_state_.positions[pi].token_id == seed.token_id_down) {
                    mkt.position_down = latest_state_.positions[pi].position;
                }
            }
        }

        snap.markets.push_back(std::move(mkt));
    }

    // State
    if (has_state_) {
        snap.state = &latest_state_;
    }

    // Metrics
    snap.metrics.ws_frames = metrics_.get(MetricId::WS_FRAMES_RECEIVED);
    snap.metrics.parse_ok = metrics_.get(MetricId::PARSE_OK);
    snap.metrics.sched_cycles = metrics_.get(MetricId::SCHED_CYCLES);
    snap.metrics.sched_events = metrics_.get(MetricId::SCHED_EVENTS);
    snap.metrics.rest_requests = metrics_.get(MetricId::EXEC_REST_REQUESTS_ORDER) +
                                 metrics_.get(MetricId::EXEC_REST_REQUESTS_CANCEL);
    snap.metrics.rest_errors = metrics_.get(MetricId::EXEC_REST_ERRORS);
    snap.metrics.ui_snapshots_dropped = metrics_.get(MetricId::UI_SNAPSHOTS_DROPPED);
    snap.metrics.ui_book_drops = metrics_.get(MetricId::UI_BOOK_DROPS);
    snap.metrics.ui_state_drops = metrics_.get(MetricId::UI_STATE_DROPS);

    // Gateway health
    snap.gateway.degraded = (metrics_.get(MetricId::EXEC_GATEWAY_DEGRADED_COUNT) > 0);
    snap.gateway.heartbeat_ok = metrics_.get(MetricId::EXEC_HEARTBEAT_OK);
    snap.gateway.heartbeat_fail = metrics_.get(MetricId::EXEC_HEARTBEAT_FAIL);

    // Latency percentiles (computed from ring buffers)
    {
        int64_t lbuf[kLatencyRingSize];

        auto fill = [&](LatencyTrackerId id) -> LatencyPercentiles {
            int n = metrics_.tracker(id).snapshot(lbuf, kLatencyRingSize);
            return compute_percentiles(lbuf, n);
        };

        auto order = fill(LatencyTrackerId::ORDER_RTT);
        snap.latency.order_rtt_avg_ns = order.avg_ns;
        snap.latency.order_rtt_p95_ns = order.p95_ns;

        auto cancel = fill(LatencyTrackerId::CANCEL_RTT);
        snap.latency.cancel_rtt_avg_ns = cancel.avg_ns;
        snap.latency.cancel_rtt_p95_ns = cancel.p95_ns;

        auto engine = fill(LatencyTrackerId::ENGINE_SPEED);
        snap.latency.engine_avg_ns = engine.avg_ns;
        snap.latency.engine_p95_ns = engine.p95_ns;

        auto pipeline = fill(LatencyTrackerId::FULL_PIPELINE);
        snap.latency.pipeline_avg_ns = pipeline.avg_ns;
        snap.latency.pipeline_p95_ns = pipeline.p95_ns;

        auto ws_book = fill(LatencyTrackerId::WS_TO_PROCESS);
        snap.latency.ws_book_avg_ns = ws_book.avg_ns;
        snap.latency.ws_book_p95_ns = ws_book.p95_ns;

        auto exch_recv = fill(LatencyTrackerId::EXCHANGE_TO_RECV);
        snap.latency.exchange_to_recv_avg_ns = exch_recv.avg_ns;
        snap.latency.exchange_to_recv_p95_ns = exch_recv.p95_ns;

        auto bmd = fill(LatencyTrackerId::BINANCE_MD_EXCH_TO_RECV);
        snap.latency.binance_md_avg_ns = bmd.avg_ns;
        snap.latency.binance_md_p95_ns = bmd.p95_ns;

        // Probe result (T2 writes atomics, T6 reads here)
        snap.latency.probe_order_rtt_ns = metrics_.probe_result.order_rtt_ns.load(std::memory_order_relaxed);
        snap.latency.probe_cancel_rtt_ns = metrics_.probe_result.cancel_rtt_ns.load(std::memory_order_relaxed);
        snap.latency.probe_roundtrip_ns = metrics_.probe_result.roundtrip_ns.load(std::memory_order_relaxed);
        snap.latency.probe_status = metrics_.probe_result.status.load(std::memory_order_relaxed);
    }

    // Account identity (stable c_str pointers — strings set before run())
    snap.account_name = account_name_.c_str();
    snap.account_address = account_address_.c_str();

    // Rotation info
    snap.rotation.market_condition = rotation_condition_.c_str();
    snap.rotation.window_start = rotation_window_start_;
    snap.rotation.window_end = rotation_window_end_;
    snap.rotation.rotation_count = rotation_count_;
    snap.rotation.in_no_trade = rotation_in_no_trade_;

    // Account positions value
    if (has_account_data_) {
        snap.account_balance.position_value = position_value_;
        snap.account_balance.available = true;
    }
    if (token_inventory_) {
        snap.account_balance.usdc_balance = token_inventory_->usdc_balance();
    }
    snap.account_balance.pol_balance = pol_balance_;
    if (pnl_tracker_) {
        snap.account_balance.realized_pnl = pnl_tracker_->realized_pnl();
    }
    if (!account_poll_error_.empty()) {
        snap.account_balance.error = account_poll_error_.c_str();
    }

    std::string json = serialize_engine_snapshot(snap);
    send_snapshot(json);
}

void IpcBridge::Impl::send_snapshot(const std::string& json) {
    if (!ws_ || !connected_) return;

    auto buf = std::make_shared<std::string>(json);

    if (write_in_progress_) {
        // Latest-only: replace any pending snapshot so the client
        // always gets the freshest state, never a stale one.
        snapshot_pending_ = true;
        pending_snapshot_buf_ = std::move(buf);
        return;
    }

    write_in_progress_ = true;
    ws_->text(true);
    ws_->async_write(net::buffer(*buf),
        [this, buf](beast::error_code ec, std::size_t bytes) {
            on_write(ec, bytes);
        });
}

void IpcBridge::Impl::send_message(const std::string& json) {
    if (!ws_ || !connected_) return;

    auto buf = std::make_shared<std::string>(json);

    if (write_in_progress_) {
        // Queue the message for later send. Cap at 1024 to prevent
        // unbounded memory growth from slow/stuck UI clients.
        static constexpr std::size_t kMaxWriteQueue = 1024;
        if (write_queue_.size() >= kMaxWriteQueue) {
            write_queue_.erase(write_queue_.begin());  // drop oldest
            metrics_.inc(MetricId::UI_SNAPSHOTS_DROPPED);
        }
        write_queue_.push_back(std::move(buf));
        return;
    }

    write_in_progress_ = true;
    ws_->text(true);
    ws_->async_write(net::buffer(*buf),
        [this, buf](beast::error_code ec, std::size_t bytes) {
            on_write(ec, bytes);
        });
}

void IpcBridge::Impl::on_write(beast::error_code ec, std::size_t /*bytes*/) {
    write_in_progress_ = false;
    if (ec) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "IPC write error: %s", ec.message().c_str());
        AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        write_queue_.clear();
        snapshot_pending_ = false;
        pending_snapshot_buf_.reset();
        close_session();
        start_accept();
        return;
    }
    metrics_.inc(MetricId::UI_SNAPSHOTS_SENT);

    // Send next queued message if any
    if (!write_queue_.empty() && ws_ && connected_) {
        auto next_buf = std::move(write_queue_.front());
        write_queue_.erase(write_queue_.begin());
        write_in_progress_ = true;
        ws_->text(true);
        ws_->async_write(net::buffer(*next_buf),
            [this, next_buf](beast::error_code ec2, std::size_t bytes2) {
                on_write(ec2, bytes2);
            });
        return;
    }

    // Flush pending snapshot — always the latest, never stale.
    if (snapshot_pending_ && pending_snapshot_buf_ && ws_ && connected_) {
        snapshot_pending_ = false;
        auto buf = std::move(pending_snapshot_buf_);
        write_in_progress_ = true;
        ws_->text(true);
        ws_->async_write(net::buffer(*buf),
            [this, buf](beast::error_code ec2, std::size_t bytes2) {
                on_write(ec2, bytes2);
            });
        return;
    }
    snapshot_pending_ = false;
}

void IpcBridge::Impl::close_session() {
    if (connected_) {
        metrics_.inc(MetricId::UI_WS_DISCONNECTED);
    }
    connected_ = false;
    write_in_progress_ = false;
    snapshot_pending_ = false;
    pending_snapshot_buf_.reset();
    if (ws_) {
        beast::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
}

// --- Watcher methods ---

void IpcBridge::Impl::handle_watch_command(const WatchCommand& cmd) {
    switch (cmd.type) {
        case WatchCommand::Type::SUBSCRIBE: {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "Watch subscribe: %s", timeframe_name(cmd.timeframe));
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

            watch_manager_.subscribe(cmd.timeframe);
            refresh_watcher_subscriptions();
            send_watcher_status(cmd.timeframe);
            break;
        }
        case WatchCommand::Type::UNSUBSCRIBE: {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "Watch unsubscribe: %s", timeframe_name(cmd.timeframe));
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

            watch_manager_.unsubscribe(cmd.timeframe);
            refresh_watcher_subscriptions();
            send_watcher_status(cmd.timeframe);
            break;
        }
        case WatchCommand::Type::REQUEST_LIST: {
            send_series_list();
            break;
        }
    }
}

void IpcBridge::Impl::start_watcher_timer() {
    int interval_ms = config_.ladder_update_interval_ms;
    if (interval_ms < 10) interval_ms = 10;

    watcher_timer_.expires_after(std::chrono::milliseconds(interval_ms));
    watcher_timer_.async_wait([this](beast::error_code ec) {
        on_watcher_tick(ec);
    });
}

void IpcBridge::Impl::on_watcher_tick(beast::error_code ec) {
    if (ec || stop_requested_.load(std::memory_order_relaxed)) return;

    try {
        auto now = SteadyClock::now();
        auto stale_ns = config_.watcher_stale_threshold_ms * 1000000LL;
        watch_manager_.tick(now, stale_ns);

        send_watcher_snapshots();
    } catch (const std::exception& ex) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "Watcher tick error: %s", ex.what());
        AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
    } catch (...) {
        AsyncLogger::log(log_handle_, LogLevel::ERROR, "Watcher tick unknown error");
    }

    start_watcher_timer();
}

void IpcBridge::Impl::on_watcher_book_data(const MarketEvent& event) {
    // Process book data from T_watch (already on T6's io_context via post)
    auto now = SteadyClock::now();
    static int watcher_event_count_ = 0;
    static int watcher_snap_count_ = 0;
    static int watcher_pc_count_ = 0;
    static int watcher_trade_count_ = 0;
    static int watcher_bbo_count_ = 0;
    static int watcher_other_count_ = 0;
    ++watcher_event_count_;

    // Apply to watcher book store based on event type
    std::visit([&](auto&& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, BookSnapshot>) {
            ++watcher_snap_count_;
            std::string token_id(payload.asset_id.data, payload.asset_id.len);

            char dbg[LogEntry::kMaxMsg];
            std::snprintf(dbg, sizeof(dbg),
                          "WatcherEvt SNAPSHOT tok=%.20s... bids=%d asks=%d",
                          token_id.c_str(), payload.bid_count, payload.ask_count);
            AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);

            WatcherBookLevel bids[kMaxWatcherBookDepth];
            WatcherBookLevel asks[kMaxWatcherBookDepth];
            int bid_count = std::min(static_cast<int>(payload.bid_count), kMaxWatcherBookDepth);
            int ask_count = std::min(static_cast<int>(payload.ask_count), kMaxWatcherBookDepth);

            for (int i = 0; i < bid_count; ++i) {
                bids[i] = {payload.bids[i].price, payload.bids[i].size};
            }
            for (int i = 0; i < ask_count; ++i) {
                asks[i] = {payload.asks[i].price, payload.asks[i].size};
            }

            watcher_books_.apply_book_snapshot(token_id, bids, bid_count, asks, ask_count);

            // Detect tick size from snapshot prices (initial snapshot carries no tick_size field)
            {
                Price_t all_prices[kMaxWatcherBookDepth * 2];
                int n = 0;
                for (int i = 0; i < bid_count && n < kMaxWatcherBookDepth * 2; ++i)
                    all_prices[n++] = bids[i].price;
                for (int i = 0; i < ask_count && n < kMaxWatcherBookDepth * 2; ++i)
                    all_prices[n++] = asks[i].price;
                TickSize_t detected = detect_tick_from_prices(all_prices, n);
                for (int i = 0; i < kBtcTimeframeCount; ++i) {
                    auto tf = static_cast<BtcTimeframe>(i);
                    const auto* info = series_registry_.current(tf);
                    if (info && (token_id == info->token_id_up || token_id == info->token_id_down)) {
                        if (watcher_tick_size_[i] != detected) {
                            char tbuf[LogEntry::kMaxMsg];
                            std::snprintf(tbuf, sizeof(tbuf),
                                          "Tick size detected from snapshot: %d -> %d for %s",
                                          static_cast<int>(watcher_tick_size_[i]),
                                          static_cast<int>(detected), timeframe_name(tf));
                            AsyncLogger::log(log_handle_, LogLevel::INFO, tbuf);
                            watcher_tick_size_[i] = detected;
                        }
                        break;
                    }
                }
            }

            // Find which timeframe this token belongs to and record data time
            for (int i = 0; i < kBtcTimeframeCount; ++i) {
                auto tf = static_cast<BtcTimeframe>(i);
                const auto* info = series_registry_.current(tf);
                if (info && (token_id == info->token_id_up || token_id == info->token_id_down)) {
                    watch_manager_.on_book_data_received(tf, now);

                    // If FSM is in CONNECTING, mark as connected
                    if (watch_manager_.fsm(tf).state() == WatcherState::CONNECTING) {
                        watch_manager_.fsm(tf).on_ws_connected();
                        send_watcher_status(tf);
                    }
                    break;
                }
            }
        } else if constexpr (std::is_same_v<T, PriceChangeEvent>) {
            ++watcher_pc_count_;
            for (int a = 0; a < payload.asset_count; ++a) {
                const auto& ac = payload.asset_changes[a];
                std::string token_id(ac.asset_id.data, ac.asset_id.len);

                if (watcher_pc_count_ <= 5) {
                    char dbg[LogEntry::kMaxMsg];
                    std::snprintf(dbg, sizeof(dbg),
                                  "WatcherEvt PRICE_CHANGE #%d tok=%.20s... changes=%d",
                                  watcher_pc_count_, token_id.c_str(), ac.change_count);
                    AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);
                }

                for (int c = 0; c < ac.change_count; ++c) {
                    bool is_bid = (ac.changes[c].side == Side::BID);
                    watcher_books_.apply_price_change(token_id,
                                                       ac.changes[c].price,
                                                       ac.changes[c].size,
                                                       is_bid);
                }

                for (int i = 0; i < kBtcTimeframeCount; ++i) {
                    auto tf = static_cast<BtcTimeframe>(i);
                    const auto* info = series_registry_.current(tf);
                    if (info && (token_id == info->token_id_up || token_id == info->token_id_down)) {
                        watch_manager_.on_book_data_received(tf, now);
                        break;
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, LastTradePriceEvent>) {
            ++watcher_trade_count_;
            std::string token_id(payload.asset_id.data, payload.asset_id.len);
            for (int i = 0; i < kBtcTimeframeCount; ++i) {
                auto tf = static_cast<BtcTimeframe>(i);
                const auto* info = series_registry_.current(tf);
                if (!info) continue;
                if (token_id == info->token_id_up) {
                    watcher_pending_trades_[i].push_back({payload.price, payload.size});
                    break;
                } else if (token_id == info->token_id_down) {
                    // Complement the price for Down token in up-centric view
                    Price_t comp = kPriceMax - payload.price;
                    watcher_pending_trades_[i].push_back({comp, payload.size});
                    break;
                }
            }
        } else if constexpr (std::is_same_v<T, TickSizeChangeEvent>) {
            std::string token_id(payload.asset_id.data, payload.asset_id.len);
            for (int i = 0; i < kBtcTimeframeCount; ++i) {
                auto tf = static_cast<BtcTimeframe>(i);
                const auto* info = series_registry_.current(tf);
                if (!info) continue;
                if (token_id == info->token_id_up || token_id == info->token_id_down) {
                    watcher_tick_size_[i] = payload.new_tick_size;
                    break;
                }
            }
        } else if constexpr (std::is_same_v<T, BestBidAskEvent>) {
            ++watcher_bbo_count_;
        } else {
            ++watcher_other_count_;
        }
        // BBO events ignored for watcher books (computed from levels)
    }, event.payload);

    // Periodic summary every 100 events
    if (watcher_event_count_ % 100 == 0) {
        char dbg[LogEntry::kMaxMsg];
        std::snprintf(dbg, sizeof(dbg),
                      "WatcherEvt summary: total=%d snap=%d pc=%d trade=%d bbo=%d other=%d",
                      watcher_event_count_, watcher_snap_count_, watcher_pc_count_,
                      watcher_trade_count_, watcher_bbo_count_, watcher_other_count_);
        AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);
    }
}

void IpcBridge::Impl::on_watcher_ws_connected() {
    AsyncLogger::log(log_handle_, LogLevel::INFO, "Watcher WS connected");
}

void IpcBridge::Impl::on_watcher_ws_disconnected(const std::string& reason) {
    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf), "Watcher WS disconnected: %s", reason.c_str());
    AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
}

void IpcBridge::Impl::on_discovery_result(BtcTimeframe tf, SeriesMarketInfo current,
                                           std::optional<SeriesMarketInfo> next) {
    const SeriesMarketInfo* next_ptr = next.has_value() ? &next.value() : nullptr;
    auto& fsm = watch_manager_.fsm(tf);

    // Detect market closure: current is closed, FSM is active but not already rolling
    if (current.is_closed && fsm.is_active() && !fsm.is_rolling() && !fsm.is_waiting_for_next()) {
        // Update registry with the closed market + next info
        series_registry_.update_from_discovery(tf, current, next_ptr);

        bool has_next = next_ptr && !next_ptr->is_closed && !next_ptr->empty();
        fsm.on_market_closed(has_next);

        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "Market closed for %s (cond=%s), has_next=%s, fsm=%s",
                      timeframe_name(tf), current.condition_id,
                      has_next ? "true" : "false",
                      watcher_state_name(fsm.state()));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        if (fsm.is_rolling()) {
            execute_rollover(tf);
        } else {
            send_watcher_status(tf);
        }
        return;
    }

    // Detect condition_id change: discovery found a NEW active market while FSM
    // is still CONNECTED to the old one. This happens when the window boundary
    // passes between discovery polls — the old market's is_closed is never seen
    // because the next poll already queries the new window.
    const auto* reg_current = series_registry_.current(tf);
    if (!current.is_closed && !current.empty() && reg_current && !reg_current->empty() &&
        fsm.is_active() && !fsm.is_rolling() && !fsm.is_waiting_for_next() &&
        std::strcmp(current.condition_id, reg_current->condition_id) != 0) {

        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf),
                      "Condition changed for %s: old=%s -> new=%s, executing direct rollover",
                      timeframe_name(tf), reg_current->condition_id, current.condition_id);
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        // Store the new market as "next" in the registry, then execute rollover
        // which promotes next→current, clears old books, and reconnects WS.
        series_registry_.update_from_discovery(tf, *reg_current, &current);
        fsm.on_market_closed(true);  // force CONNECTED → ROLLING

        if (fsm.is_rolling()) {
            execute_rollover(tf);
        }

        // After rollover, also store the next market if discovered
        if (next_ptr && !next_ptr->is_closed && !next_ptr->empty()) {
            // Update the registry's "next" slot with the actual next market
            const auto* new_current = series_registry_.current(tf);
            if (new_current) {
                series_registry_.update_from_discovery(tf, *new_current, next_ptr);
            }
        }
        return;
    }

    // Normal update (not a closure event)
    series_registry_.update_from_discovery(tf, current, next_ptr);

    // Check if we were waiting for next and it arrived
    if (fsm.is_waiting_for_next() && series_registry_.has_next(tf)) {
        fsm.on_next_discovered();

        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "Next market discovered for %s while ROLL_PENDING, fsm=%s",
                      timeframe_name(tf), watcher_state_name(fsm.state()));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        if (fsm.is_rolling()) {
            execute_rollover(tf);
        } else {
            send_watcher_status(tf);
        }
        return;
    }

    // Refresh WS subscriptions since token_ids may have changed
    refresh_watcher_subscriptions();

    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf), "Discovery update for %s: cond=%s has_next=%s",
                  timeframe_name(tf), current.condition_id,
                  next_ptr ? "true" : "false");
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
}

void IpcBridge::Impl::execute_rollover(BtcTimeframe tf) {
    int ti = static_cast<int>(tf);

    // Capture old token IDs before promotion
    const auto* old_info = series_registry_.current(tf);
    std::string old_up, old_down;
    if (old_info) {
        old_up = old_info->token_id_up;
        old_down = old_info->token_id_down;
    }

    // Promote next to current
    series_registry_.promote_next(tf);

    // Clear old books
    if (!old_up.empty()) watcher_books_.clear_book(old_up);
    if (!old_down.empty()) watcher_books_.clear_book(old_down);

    // Clear pending trades and reset tick size to default
    watcher_pending_trades_[ti].clear();
    watcher_tick_size_[ti] = 100;

    // Reset staleness tracking
    watch_manager_.on_book_data_received(tf, 0);

    // Refresh WS subscriptions with new token IDs
    refresh_watcher_subscriptions();

    // Complete the roll: ROLLING -> CONNECTING
    watch_manager_.fsm(tf).on_switch_complete();

    // Notify UI
    send_watcher_status(tf);

    const auto* new_info = series_registry_.current(tf);
    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf),
                  "Rollover complete for %s: old_up=%.20s... -> new_cond=%s, fsm=%s",
                  timeframe_name(tf), old_up.c_str(),
                  new_info ? new_info->condition_id : "(none)",
                  watcher_state_name(watch_manager_.fsm(tf).state()));
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
}

void IpcBridge::Impl::send_watcher_snapshots() {
    if (!connected_) return;
    static int watcher_send_count_ = 0;

    auto timeframes = watch_manager_.subscribed_timeframes();
    for (auto tf : timeframes) {
        int ti = static_cast<int>(tf);
        const auto* info = series_registry_.current(tf);
        if (!info || info->empty()) continue;

        auto ladder = watcher_books_.build_merged_ladder(
            info->token_id_up, info->token_id_down, config_.ladder_max_depth);

        ++watcher_send_count_;
        // Log ladder contents every 50 sends
        if (watcher_send_count_ <= 3 || watcher_send_count_ % 50 == 0) {
            Price_t top_bid = 0, top_ask = 0;
            Qty_t top_bid_sz = 0, top_ask_sz = 0;
            if (!ladder.buy_levels.empty()) {
                top_bid = ladder.buy_levels[0].price;
                top_bid_sz = ladder.buy_levels[0].size;
            }
            if (!ladder.sell_levels.empty()) {
                top_ask = ladder.sell_levels[0].price;
                top_ask_sz = ladder.sell_levels[0].size;
            }
            char dbg[LogEntry::kMaxMsg];
            std::snprintf(dbg, sizeof(dbg),
                          "WatcherSend #%d %s: buys=%zu sells=%zu topBid=%d@%lld topAsk=%d@%lld trades=%zu",
                          watcher_send_count_, timeframe_name(tf),
                          ladder.buy_levels.size(), ladder.sell_levels.size(),
                          top_bid, static_cast<long long>(top_bid_sz),
                          top_ask, static_cast<long long>(top_ask_sz),
                          watcher_pending_trades_[ti].size());
            AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);
        }

        auto json = serialize_watcher_books(tf, info->condition_id, ladder,
                                             watcher_pending_trades_[ti],
                                             watcher_tick_size_[ti]);
        send_message(json);

        // Clear trades after sending
        watcher_pending_trades_[ti].clear();
    }
}

void IpcBridge::Impl::send_series_list() {
    std::vector<SeriesListEntry> entries;
    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        auto tf = static_cast<BtcTimeframe>(i);
        SeriesListEntry entry;
        entry.timeframe = tf;
        const auto* info = series_registry_.current(tf);
        entry.condition_id = info ? info->condition_id : "";
        entry.status = watch_manager_.fsm(tf).state();
        entry.has_next = series_registry_.has_next(tf);
        entries.push_back(entry);
    }

    auto json = serialize_series_list(entries);
    send_message(json);
}

void IpcBridge::Impl::send_watcher_status(BtcTimeframe tf) {
    auto state = watch_manager_.fsm(tf).state();
    auto json = serialize_watcher_status(tf, state);
    send_message(json);
}

void IpcBridge::Impl::refresh_watcher_subscriptions() {
    if (!watcher_service_) return;
    auto ids = watch_manager_.active_asset_ids(series_registry_);
    watcher_service_->update_subscriptions(std::move(ids));
}

// --- IpcBridge outer class (Pimpl forwarding) ---

IpcBridge::IpcBridge(SpscQueue<UiBookUpdate>& book_queue,
                     SpscQueue<UiStateSnapshot>& state_queue,
                     SpscQueue<SchedulerEvent>& control_queue,
                     Metrics& metrics,
                     AsyncLogger& logger,
                     const IpcBridgeConfig& config,
                     const MarketPairRegistry* market_pairs,
                     const std::vector<MarketPairConfig>* market_pair_configs,
                     std::atomic<bool>* fatal_flag)
    : impl_(std::make_unique<Impl>(book_queue, state_queue, control_queue,
                                   metrics, logger, config,
                                   market_pairs, market_pair_configs, fatal_flag)) {}

IpcBridge::~IpcBridge() = default;

void IpcBridge::run() { impl_->run(); }
void IpcBridge::request_shutdown() { impl_->request_shutdown(); }
void IpcBridge::set_engine_config(const EngineConfig& config) { impl_->set_engine_config(config); }
void IpcBridge::set_account_info(const std::string& name, const std::string& address) {
    impl_->set_account_info(name, address);
}

void IpcBridge::set_token_inventory(TokenInventory* inventory) {
    impl_->set_token_inventory(inventory);
}

void IpcBridge::set_pnl_tracker(const PnlTracker* tracker) {
    impl_->set_pnl_tracker(tracker);
}

void IpcBridge::set_account_credentials(const std::string& api_key,
                                         const std::string& api_secret,
                                         const std::string& api_passphrase,
                                         const std::string& address) {
    impl_->set_account_credentials(api_key, api_secret, api_passphrase, address);
}

void IpcBridge::set_rotation_info(const std::string& condition_id,
                                   int64_t window_start, int64_t window_end,
                                   int rotation_count, bool in_no_trade) {
    impl_->set_rotation_info(condition_id, window_start, window_end, rotation_count, in_no_trade);
}

}  // namespace lt
