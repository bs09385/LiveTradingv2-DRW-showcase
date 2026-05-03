#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "common/types.h"

namespace lt {

using RtdsOnMessageCb = std::function<void(std::string_view payload, Timestamp_ns recv_ts)>;
using RtdsOnConnectedCb = std::function<void()>;
using RtdsOnDisconnectedCb = std::function<void(const std::string& reason)>;

struct RtdsWsConfig {
    std::string endpoint = "wss://ws-live-data.polymarket.com";
    int64_t ping_interval_ms = 5000;      // RTDS requires PING every 5 seconds
    int64_t reconnect_base_ms = 1000;
    int64_t reconnect_max_ms = 30000;
    int64_t stale_threshold_ms = 15000;
};

// WebSocket client for Polymarket Real-Time Data Socket (RTDS).
//
// Connects to wss://ws-live-data.polymarket.com and subscribes to
// the crypto_prices topic. Sends PING every 5 seconds as required.
//
// Thread ownership: T_rtds (dedicated thread).
class RtdsWsClient {
public:
    explicit RtdsWsClient(const RtdsWsConfig& config);
    ~RtdsWsClient();

    RtdsWsClient(const RtdsWsClient&) = delete;
    RtdsWsClient& operator=(const RtdsWsClient&) = delete;

    void set_on_message(RtdsOnMessageCb cb);
    void set_on_connected(RtdsOnConnectedCb cb);
    void set_on_disconnected(RtdsOnDisconnectedCb cb);

    // Run the client (blocks on io_context.run())
    void run();

    // Thread-safe shutdown request
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
