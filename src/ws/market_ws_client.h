#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

namespace lt {

using OnMessageCb = std::function<void(std::string_view payload, Timestamp_ns recv_ts)>;
using OnConnectedCb = std::function<void()>;
using OnDisconnectedCb = std::function<void(const std::string& reason)>;

struct WsClientConfig {
    std::string endpoint;
    std::vector<std::string> asset_ids;
    int64_t ping_interval_ms = 10000;
    int64_t pong_timeout_ms = 5000;
    int64_t reconnect_base_ms = 1000;
    int64_t reconnect_max_ms = 30000;
    int64_t stale_threshold_ms = 30000;
    int redundancy = 1;              // number of parallel WS connections (1-20)
    int64_t redundancy_stagger_ms = 200;  // delay between connection starts
};

class MarketWsClient {
public:
    explicit MarketWsClient(const WsClientConfig& config);
    ~MarketWsClient();

    // Non-copyable
    MarketWsClient(const MarketWsClient&) = delete;
    MarketWsClient& operator=(const MarketWsClient&) = delete;

    void set_on_message(OnMessageCb cb);
    void set_on_connected(OnConnectedCb cb);
    void set_on_disconnected(OnDisconnectedCb cb);

    // Run the client (blocks on io_context.run())
    void run();

    // Thread-safe shutdown request
    void request_shutdown();

    // Dynamic subscription: add token_ids to existing connection.
    // Thread-safe: posts to io_context for serialized async_write.
    void send_subscribe_add(const std::vector<std::string>& token_ids);

    // Dynamic unsubscribe: remove token_ids from existing connection.
    // Thread-safe: posts to io_context for serialized async_write.
    void send_unsubscribe(const std::vector<std::string>& token_ids);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
