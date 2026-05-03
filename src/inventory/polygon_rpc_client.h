#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/error.h"

namespace lt {

struct TransactionReceipt {
    std::string transaction_hash;
    uint64_t status = 0;       // 1 = success, 0 = revert
    uint64_t gas_used = 0;
    uint64_t block_number = 0;
};

// Synchronous Polygon JSON-RPC client for inventory operations.
// Uses sync_https_post() (Boost.Beast) for all calls.
// Non-hot-path only (inventory worker thread).
class PolygonRpcClient {
public:
    explicit PolygonRpcClient(const std::string& rpc_url, int timeout_ms = 10000);

    // eth_gasPrice -> gas price in wei
    Expected<uint64_t> get_gas_price();

    // eth_getTransactionCount(address, "pending") -> nonce
    Expected<uint64_t> get_transaction_count(const std::string& address);

    // eth_sendRawTransaction("0x" + hex(signed_tx)) -> tx hash
    Expected<std::string> send_raw_transaction(const std::vector<uint8_t>& signed_tx);

    // eth_getTransactionReceipt(txHash) -> receipt
    // Returns NETWORK_ERROR on RPC failure, JSON_MISSING_FIELD if receipt is null (pending).
    Expected<TransactionReceipt> get_transaction_receipt(const std::string& tx_hash);

    const std::string& last_error() const { return last_error_; }

private:
    std::string host_;
    std::string path_;
    int timeout_ms_;
    int rpc_id_ = 1;
    std::string last_error_;

    // Execute a JSON-RPC call and return raw response body.
    std::string do_rpc(const std::string& body);

    // Parse hex string (with 0x prefix) to uint64_t
    static uint64_t parse_hex_uint64(const char* hex, size_t len);
};

}  // namespace lt
