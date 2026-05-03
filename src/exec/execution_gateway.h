#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "common/types.h"
#include "common/token_inventory.h"
#include "exec/exec_intent.h"
#include "exec/exec_feedback.h"
#include "exec/rate_limiter.h"
#include "exec/heartbeat_manager.h"
#include "events/scheduler_events.h"
#include "queue/spsc_queue.h"

namespace lt {

class OrderSigner;
class OrderBuilder;
class Metrics;
class AsyncLogger;

struct GatewayConfig {
    std::string rest_base_url = "https://clob.polymarket.com";
    std::string rest_host = "clob.polymarket.com";
    std::string rest_port = "443";
    int64_t request_timeout_ms = 5000;
    int rest_pipeline_depth = 4;
    bool gateway_enabled = true;
    RateLimitConfig rate_limit;
    HeartbeatConfig heartbeat;
    std::string owner_uuid;
    std::string maker_address;
    std::string signer_address;

    // Optional explicit auth credentials (primarily for tests). When all are
    // provided, gateway uses these instead of environment variables.
    std::string poly_api_key;
    std::string poly_api_secret_b64;
    std::string poly_api_passphrase;
    std::string poly_api_address;

    // Order builder defaults
    bool defer_exec = false;
    bool post_only = false;
    uint8_t signature_type = 0;  // 0=EOA

    // Batch orders
    bool batch_orders_enabled = false;
    int batch_max_size = 15;

    // Redundant cancel connections (separate from order pipeline)
    int cancel_connection_redundancy = 1;

    // HTTP/2 transport
    bool use_http2 = false;
    int max_concurrent_streams = 100;

    // Order pipeline connection pool — round-robin across N connections
    // to isolate TCP packet loss. 1 = single connection (old behavior).
    int order_connection_pool_size = 3;

    // When true, run() skips the initial GET /time warmup. Used by tests so
    // they don't trigger DNS / TLS handshakes against a real endpoint.
    bool skip_warmup = false;
};

class ExecutionGateway {
public:
    ExecutionGateway(SpscQueue<ExecIntent>& intent_queue,
                     SpscQueue<SchedulerEvent>& feedback_queue,
                     Metrics& metrics,
                     AsyncLogger& logger,
                     const GatewayConfig& config,
                     OrderSigner& signer,
                     std::atomic<bool>* fatal_flag = nullptr,
                     TokenInventory* inventory_view = nullptr);
    ~ExecutionGateway();

    ExecutionGateway(const ExecutionGateway&) = delete;
    ExecutionGateway& operator=(const ExecutionGateway&) = delete;

    // Run the gateway event loop (blocks). Called from T3 thread.
    void run();

    // Thread-safe shutdown request
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
