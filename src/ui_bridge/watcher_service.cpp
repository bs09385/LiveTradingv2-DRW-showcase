// Boost.Asio includes Windows headers that #define ERROR.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>

#ifdef ERROR
#undef ERROR
#endif

#include "ui_bridge/watcher_service.h"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/discovery.h"
#include "common/price.h"
#include "events/market_events.h"
#include "logger/async_logger.h"
#include "parser/market_message_parser.h"
#include "ws/market_ws_client.h"
#include "simdjson.h"

namespace lt {

namespace {

// Parse the initial array-wrapped book snapshot from Polymarket WS.
// Format: [{"market":"...","asset_id":"...","bids":[...],"asks":[...]}, ...]
// These lack "event_type" and come as an array. Not used on hot path.
std::vector<MarketEvent> parse_initial_book_array(std::string_view payload,
                                                   Timestamp_ns recv_ts,
                                                   SeqNum_t& seq) {
    std::vector<MarketEvent> events;
    try {
        simdjson::dom::parser dom;
        auto doc = dom.parse(payload.data(), payload.size());
        simdjson::dom::array arr;
        if (doc.get_array().get(arr)) return events;

        for (auto elem : arr) {
            simdjson::dom::object obj;
            if (elem.get_object().get(obj)) continue;

            auto ev = std::make_unique<MarketEvent>();
            ev->recv_ts = recv_ts;
            ev->seq = ++seq;

            BookSnapshot snap;

            // asset_id
            std::string_view aid;
            if (obj["asset_id"].get_string().get(aid)) continue;
            snap.asset_id = AssetId(aid);

            // timestamp
            std::string_view ts_str;
            if (!obj["timestamp"].get_string().get(ts_str)) {
                int64_t ts_val = 0;
                auto r = std::from_chars(ts_str.data(), ts_str.data() + ts_str.size(), ts_val);
                if (r.ec == std::errc{}) snap.exchange_ts = ts_val;
            }

            // bids
            simdjson::dom::array bids;
            if (!obj["bids"].get_array().get(bids)) {
                for (auto bid : bids) {
                    if (snap.bid_count >= snap.bids.size()) break;
                    simdjson::dom::object bid_obj;
                    if (bid.get_object().get(bid_obj)) continue;

                    std::string_view ps, ss;
                    if (bid_obj["price"].get_string().get(ps)) continue;
                    if (bid_obj["size"].get_string().get(ss)) continue;

                    auto price = parse_price(ps);
                    auto size = parse_qty(ss);
                    if (!price.ok() || !size.ok()) continue;

                    snap.bids[snap.bid_count].price = price.value;
                    snap.bids[snap.bid_count].size = size.value;
                    ++snap.bid_count;
                }
            }

            // asks
            simdjson::dom::array asks;
            if (!obj["asks"].get_array().get(asks)) {
                for (auto ask : asks) {
                    if (snap.ask_count >= snap.asks.size()) break;
                    simdjson::dom::object ask_obj;
                    if (ask.get_object().get(ask_obj)) continue;

                    std::string_view ps, ss;
                    if (ask_obj["price"].get_string().get(ps)) continue;
                    if (ask_obj["size"].get_string().get(ss)) continue;

                    auto price = parse_price(ps);
                    auto size = parse_qty(ss);
                    if (!price.ok() || !size.ok()) continue;

                    snap.asks[snap.ask_count].price = price.value;
                    snap.asks[snap.ask_count].size = size.value;
                    ++snap.ask_count;
                }
            }

            ev->payload = std::move(snap);
            events.push_back(std::move(*ev));
        }
    } catch (...) {
        // Parse failure — return whatever we have so far
    }
    return events;
}

}  // anonymous namespace

// --- WatcherService implementation ---

WatcherService::WatcherService(AsyncLogger& logger, const EngineConfig& config)
    : logger_(logger), log_handle_(logger.create_producer("watcher_svc")), config_(config) {}

WatcherService::~WatcherService() {
    stop();
}

void WatcherService::set_on_book_data(OnBookData cb) { on_book_data_ = std::move(cb); }
void WatcherService::set_on_ws_connected(OnWsConnected cb) { on_ws_connected_ = std::move(cb); }
void WatcherService::set_on_ws_disconnected(OnWsDisconnected cb) { on_ws_disconnected_ = std::move(cb); }
void WatcherService::set_on_discovery(OnDiscovery cb) { on_discovery_ = std::move(cb); }

void WatcherService::start() {
    if (running_.load(std::memory_order_relaxed)) return;
    stop_requested_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread([this]() { thread_main(); });
}

void WatcherService::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    running_.store(false, std::memory_order_relaxed);
}

void WatcherService::update_subscriptions(std::vector<std::string> asset_ids) {
    std::lock_guard<std::mutex> lock(sub_mutex_);
    pending_asset_ids_ = std::move(asset_ids);
    sub_changed_.store(true, std::memory_order_release);
}

void WatcherService::thread_main() {
    AsyncLogger::log(log_handle_, LogLevel::INFO, "Watcher service started");

    std::vector<std::string> current_ids;
    std::unique_ptr<MarketWsClient> ws_client;
    std::thread ws_thread;  // joinable thread for WS client (not detached)
    MarketMessageParser parser;
    SeqNum_t seq = 0;       // persistent across WS reconnects within same subscription
    int msg_count = 0;      // debug counter — lives in outer scope to avoid use-after-free
    int parse_ok_count = 0;
    int parse_fail_count = 0;
    int last_fail_ec = 0;

    auto last_discovery = std::chrono::steady_clock::now();
    // Do an initial discovery poll immediately
    do_discovery_poll();

    // Helper to cleanly shut down the current WS client + thread
    auto shutdown_ws = [&]() {
        if (ws_client) {
            ws_client->request_shutdown();
            if (ws_thread.joinable()) ws_thread.join();
            ws_client.reset();
        }
    };

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        // Check for subscription changes
        if (sub_changed_.load(std::memory_order_acquire)) {
            std::vector<std::string> new_ids;
            {
                std::lock_guard<std::mutex> lock(sub_mutex_);
                new_ids = pending_asset_ids_;
                sub_changed_.store(false, std::memory_order_relaxed);
            }

            if (new_ids != current_ids) {
                auto old_ids = current_ids;
                current_ids = new_ids;

                if (!ws_client && !current_ids.empty()) {
                    // No existing client — create one from scratch
                    seq = 0;
                    msg_count = 0;
                    parse_ok_count = 0;
                    parse_fail_count = 0;
                    last_fail_ec = 0;

                    WsClientConfig ws_cfg;
                    ws_cfg.endpoint = config_.ws_endpoint;
                    ws_cfg.asset_ids = current_ids;
                    ws_cfg.ping_interval_ms = config_.ping_interval_ms;
                    ws_cfg.pong_timeout_ms = config_.pong_timeout_ms;
                    ws_cfg.reconnect_base_ms = config_.reconnect_base_ms;
                    ws_cfg.reconnect_max_ms = config_.reconnect_max_ms;

                    ws_client = std::make_unique<MarketWsClient>(ws_cfg);

                    ws_client->set_on_message([this, &parser, &seq, &msg_count,
                                               &parse_ok_count, &parse_fail_count, &last_fail_ec]
                                              (std::string_view payload, Timestamp_ns recv_ts) {
                        ++msg_count;
                        if (msg_count <= 5) {
                            char dbg[LogEntry::kMaxMsg];
                            auto preview_len = std::min(payload.size(), static_cast<size_t>(200));
                            std::snprintf(dbg, sizeof(dbg), "Watcher WS msg #%d (len=%zu): %.200s",
                                          msg_count, payload.size(), std::string(payload.substr(0, preview_len)).c_str());
                            AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);
                        }

                        if (!payload.empty() && payload[0] == '[') {
                            auto events = parse_initial_book_array(payload, recv_ts, seq);
                            if (!events.empty() && on_book_data_) {
                                char dbg[LogEntry::kMaxMsg];
                                std::snprintf(dbg, sizeof(dbg),
                                              "Watcher: parsed initial book snapshot, %zu assets",
                                              events.size());
                                AsyncLogger::log(log_handle_, LogLevel::INFO, dbg);
                                for (auto& ev : events) {
                                    on_book_data_(ev);
                                }
                            }
                            return;
                        }

                        auto event = std::make_unique<MarketEvent>();
                        auto ec = parser.parse(payload, recv_ts, ++seq, *event);
                        if (ec == ErrorCode::OK && on_book_data_) {
                            ++parse_ok_count;
                            on_book_data_(*event);
                        } else {
                            ++parse_fail_count;
                            last_fail_ec = static_cast<int>(ec);
                            if (parse_fail_count <= 10) {
                                char dbg[LogEntry::kMaxMsg];
                                auto preview_len = std::min(payload.size(), static_cast<size_t>(150));
                                std::snprintf(dbg, sizeof(dbg),
                                              "Watcher parse FAIL #%d ec=%d len=%zu: %.150s",
                                              parse_fail_count, static_cast<int>(ec),
                                              payload.size(),
                                              std::string(payload.substr(0, preview_len)).c_str());
                                AsyncLogger::log(log_handle_, LogLevel::WARN, dbg);
                            } else if (parse_fail_count % 1000 == 0) {
                                char dbg[LogEntry::kMaxMsg];
                                std::snprintf(dbg, sizeof(dbg),
                                              "Watcher parse stats: ok=%d fail=%d last_ec=%d",
                                              parse_ok_count, parse_fail_count, last_fail_ec);
                                AsyncLogger::log(log_handle_, LogLevel::WARN, dbg);
                            }
                        }
                    });

                    ws_client->set_on_connected([this]() {
                        if (on_ws_connected_) on_ws_connected_();
                    });

                    ws_client->set_on_disconnected([this](const std::string& reason) {
                        if (on_ws_disconnected_) on_ws_disconnected_(reason);
                    });

                    auto* client_ptr = ws_client.get();
                    ws_thread = std::thread([client_ptr]() {
                        client_ptr->run();
                    });

                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "Watcher WS client started with %zu asset_ids",
                                  current_ids.size());
                    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

                } else if (ws_client && current_ids.empty()) {
                    // All tokens removed — shut down client
                    shutdown_ws();

                } else if (ws_client) {
                    // Client exists and tokens changed — use dynamic sub/unsub
                    // to avoid tearing down the connection (preserves other feeds)
                    std::vector<std::string> to_remove;
                    std::vector<std::string> to_add;

                    // Find tokens removed (in old but not in new)
                    for (const auto& id : old_ids) {
                        bool found = false;
                        for (const auto& nid : current_ids) {
                            if (nid == id) { found = true; break; }
                        }
                        if (!found) to_remove.push_back(id);
                    }

                    // Find tokens added (in new but not in old)
                    for (const auto& id : current_ids) {
                        bool found = false;
                        for (const auto& oid : old_ids) {
                            if (oid == id) { found = true; break; }
                        }
                        if (!found) to_add.push_back(id);
                    }

                    if (!to_remove.empty()) {
                        ws_client->send_unsubscribe(to_remove);
                        char buf[LogEntry::kMaxMsg];
                        std::snprintf(buf, sizeof(buf),
                                      "Watcher: unsubscribed %zu tokens (dynamic)",
                                      to_remove.size());
                        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                    }

                    if (!to_add.empty()) {
                        ws_client->send_subscribe_add(to_add);
                        char buf[LogEntry::kMaxMsg];
                        std::snprintf(buf, sizeof(buf),
                                      "Watcher: subscribed %zu new tokens (dynamic)",
                                      to_add.size());
                        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                    }
                }
            }
        }

        // Periodic discovery polling
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discovery).count();
        if (elapsed >= config_.watcher_discovery_poll_ms) {
            do_discovery_poll();
            last_discovery = now;
        }

        // Sleep briefly to avoid spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Clean shutdown
    shutdown_ws();

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Watcher service stopped");
}

void WatcherService::do_discovery_poll() {
    auto host = extract_host(config_.discovery_api_url);
    if (host.empty()) {
        AsyncLogger::log(log_handle_, LogLevel::WARN, "Discovery: empty host from URL");
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto unix_ts = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf), "Discovery poll starting (host=%s, time=%lld)",
                  host.c_str(), static_cast<long long>(unix_ts));
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

    for (int i = 0; i < kBtcTimeframeCount; ++i) {
        if (stop_requested_.load(std::memory_order_relaxed)) return;

        auto tf = static_cast<BtcTimeframe>(i);
        int64_t period = timeframe_period_seconds(tf);
        auto current_candidates = discovery_window_candidates(tf, unix_ts);
        if (current_candidates.empty()) {
            current_candidates.push_back((unix_ts / period) * period);
        }

        std::optional<DiscoveredMarket> market;
        std::optional<DiscoveredMarket> closed_market;
        int64_t matched_ts = current_candidates.front();

        for (int64_t ts : current_candidates) {
            auto found = discover_btc_market_for_window(host, tf, ts);
            if (!found) continue;
            if (!found->is_closed) {
                market = found;
                matched_ts = ts;
                break;
            }
            if (!closed_market) {
                closed_market = found;
                matched_ts = ts;
            }
        }
        if (!market && closed_market) {
            market = closed_market;
        }

        SeriesMarketInfo current_info;
        std::optional<SeriesMarketInfo> next_info;
        bool found = false;

        if (market && !market->is_closed) {
            // Active market found at current timestamp
            current_info.set_condition_id(market->condition_id);
            current_info.set_token_id_up(market->token_id_up);
            current_info.set_token_id_down(market->token_id_down);
            current_info.is_closed = false;
            found = true;

            // Also try to discover next market
            int64_t next_ts = matched_ts + period;
            auto next_market = discover_btc_market_for_window(host, tf, next_ts);
            if (next_market) {
                SeriesMarketInfo ni;
                ni.set_condition_id(next_market->condition_id);
                ni.set_token_id_up(next_market->token_id_up);
                ni.set_token_id_down(next_market->token_id_down);
                ni.is_closed = next_market->is_closed;
                next_info = ni;
            }
        } else if (market && market->is_closed) {
            // Market exists but is closed — report closure with token IDs preserved
            current_info.set_condition_id(market->condition_id);
            current_info.set_token_id_up(market->token_id_up);
            current_info.set_token_id_down(market->token_id_down);
            current_info.is_closed = true;
            found = true;

            // Discover next window's market for rollover
            int64_t next_ts = matched_ts + period;
            auto next_market = discover_btc_market_for_window(host, tf, next_ts);
            if (next_market && !next_market->is_closed) {
                SeriesMarketInfo ni;
                ni.set_condition_id(next_market->condition_id);
                ni.set_token_id_up(next_market->token_id_up);
                ni.set_token_id_down(next_market->token_id_down);
                ni.is_closed = false;
                next_info = ni;
            }
        } else {
            // Not found in candidate current windows — try candidate next windows.
            for (int64_t base_ts : current_candidates) {
                int64_t next_ts = base_ts + period;
                auto next_market = discover_btc_market_for_window(host, tf, next_ts);
                if (next_market && !next_market->is_closed) {
                    current_info.set_condition_id(next_market->condition_id);
                    current_info.set_token_id_up(next_market->token_id_up);
                    current_info.set_token_id_down(next_market->token_id_down);
                    current_info.is_closed = false;
                    found = true;
                    matched_ts = next_ts;
                    break;
                }
            }
        }

        if (found && on_discovery_) {
            std::snprintf(buf, sizeof(buf), "Discovered %s: window=%lld cond=%s up=%.20s... down=%.20s...",
                          timeframe_name(tf), static_cast<long long>(matched_ts),
                          current_info.condition_id,
                          current_info.token_id_up, current_info.token_id_down);
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            on_discovery_(tf, current_info, next_info);
        } else if (!found) {
            std::snprintf(buf, sizeof(buf), "Discovery: no active market found for %s (now=%lld)",
                          timeframe_name(tf), static_cast<long long>(unix_ts));
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        }
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Discovery poll complete");
}

}  // namespace lt
