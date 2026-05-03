#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "binance/binance_types.h"
#include "common/types.h"

namespace lt {

using BinanceOnMessageCb = std::function<void(std::string_view payload, Timestamp_ns recv_ts)>;
using BinanceOnConnectedCb = std::function<void()>;
using BinanceOnDisconnectedCb = std::function<void(const std::string& reason)>;

// Status sentinel callback fired on connect/disconnect transitions, exactly
// once per UP<->DOWN edge. Runs on T_binance_md.
using BinanceOnStatusCb =
    std::function<void(BinanceUpdateKind kind, const std::string& reason)>;

struct BinanceWsConfig {
    // Full wss:// URL. Caller constructs the combined-stream URL:
    //   wss://stream.binance.com:9443/stream?streams=btcusdt@bookTicker/btcusdt@trade
    std::string endpoint;

    // Reconnect backoff (exponential, capped).
    int64_t reconnect_base_ms = 1000;
    int64_t reconnect_max_ms = 30000;

    // Watchdog: force reconnect if no frame received within this window.
    // bookTicker is very active; 60s is a safe default.
    int64_t stale_threshold_ms = 60000;

    // Proactive rotation: close + reconnect before Binance's 24h server-side
    // cut-off to avoid an unexpected disconnect. 0 disables.
    int64_t rotate_interval_ms = 23 * 60 * 60 * 1000;  // 23h
};

// WebSocket client for Binance Spot market data streams.
//
// Protocol notes:
//   - Combined-stream URL selects subscriptions via path — no SUBSCRIBE
//     frame is required once connected.
//   - Binance sends WebSocket-protocol pings every ~20s; Boost.Beast
//     auto-responds with a pong when using timeout::suggested(client).
//   - A server-initiated disconnect occurs at the 24h mark; this client
//     rotates the connection at `rotate_interval_ms` to preempt that.
//
// Thread ownership: T_binance_md (dedicated thread).
class BinanceWsClient {
public:
    explicit BinanceWsClient(const BinanceWsConfig& config);
    ~BinanceWsClient();

    BinanceWsClient(const BinanceWsClient&) = delete;
    BinanceWsClient& operator=(const BinanceWsClient&) = delete;

    void set_on_message(BinanceOnMessageCb cb);
    void set_on_connected(BinanceOnConnectedCb cb);
    void set_on_disconnected(BinanceOnDisconnectedCb cb);
    void set_on_status(BinanceOnStatusCb cb);

    // Thread-safe status probes (relaxed atomic loads, suitable from any thread).
    // The strategy can use these in evaluate() for fast freshness checks
    // without going through the queue.
    bool is_connected() const noexcept;
    int64_t last_message_ts_ns() const noexcept;

    // Run the client (blocks on io_context.run()).
    void run();

    // Thread-safe shutdown request (posts onto the io_context).
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
