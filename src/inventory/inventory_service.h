#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "inventory/inventory_sink.h"
#include "queue/spsc_queue.h"

namespace lt {

class Metrics;
class AsyncLogger;
class TokenInventory;

struct InventoryServiceConfig {
    bool enabled = false;
    bool dry_run = true;
    int64_t poll_sleep_ms = 10;

    // Relayer
    std::string relayer_host = "relayer-v2.polymarket.com";
    std::string relayer_port = "443";
    int64_t relayer_poll_interval_ms = 5000;
    int relayer_max_poll_attempts = 60;
    int relayer_timeout_ms = 10000;

    // Builder auth
    std::string builder_api_key;
    std::string builder_api_secret_b64;
    std::string builder_api_passphrase;

    // Signing (private key bytes, copied before secrets are cleared)
    uint8_t private_key[32]{};
    bool has_private_key = false;

    // Direct RPC (alternative to relayer)
    bool use_direct_rpc = false;
    std::string polygon_rpc_url;
    int64_t max_gas_price_gwei = 100;
    uint64_t gas_limit = 500000;
    int rpc_timeout_ms = 10000;
};

// Dedicated non-hot-path worker for split/merge/redeem operations.
class InventoryService {
public:
    InventoryService(SpscQueue<InventoryOpRequest>& queue,
                     Metrics& metrics,
                     AsyncLogger& logger,
                     const InventoryServiceConfig& config,
                     TokenInventory* inventory = nullptr);
    ~InventoryService();

    InventoryService(const InventoryService&) = delete;
    InventoryService& operator=(const InventoryService&) = delete;

    void run();
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
