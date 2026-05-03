#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common/error.h"

namespace lt {

struct RelayerConfig {
    std::string host = "relayer-v2.polymarket.com";
    std::string port = "443";
    int64_t poll_interval_ms = 5000;
    int max_poll_attempts = 60;
    int timeout_ms = 10000;
};

struct RelayerSubmitResult {
    std::string transaction_id;
    std::string transaction_hash;
    std::string state;  // immediate state from submit response (may be STATE_FAILED)
};

// Response from GET /relay-payload (used for PROXY transactions)
struct RelayPayload {
    uint64_t nonce = 0;
    std::string relay_address;  // relay node address for signing
};

// Synchronous blocking HTTPS client for Polymarket Relayer v2 API.
// Uses Boost.Beast directly (same pattern as sync_https_get in discovery.cpp).
// Designed for non-hot-path worker threads only.
class RelayerClient {
public:
    RelayerClient(const RelayerConfig& config,
                  const std::string& builder_api_key,
                  const std::string& builder_api_secret_b64,
                  const std::string& builder_passphrase);
    ~RelayerClient();

    RelayerClient(const RelayerClient&) = delete;
    RelayerClient& operator=(const RelayerClient&) = delete;

    // GET /relay-payload?address=<eoa>&type=PROXY
    // Returns nonce and relay address for proxy transactions.
    Expected<RelayPayload> get_relay_payload(const std::string& eoa_address);

    // POST /submit with transaction body
    Expected<RelayerSubmitResult> submit(const std::string& body);

    // GET /transaction?id=<id> — returns state string
    Expected<std::string> get_transaction_state(const std::string& tx_id);

    // GET /deployed?address=<eoa> — returns safe/proxy address if deployed
    Expected<std::string> query_safe_address(const std::string& eoa_address);

    // Detailed error string from the most recent failed call (for logging)
    const std::string& last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
