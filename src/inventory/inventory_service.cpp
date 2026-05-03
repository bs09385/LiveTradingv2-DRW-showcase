#include "inventory/inventory_service.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "common/clock.h"
#include "common/token_inventory.h"
#include "inventory/abi_encoder.h"
#include "inventory/proxy_tx_builder.h"
#include "inventory/polygon_rpc_client.h"
#include "inventory/relayer_client.h"
#include "inventory/safe_tx_builder.h"  // for eth_sign_hash
#include "inventory/tx_builder.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"

namespace lt {

namespace {

// Build the JSON submit body for a PROXY transaction.
std::string build_proxy_submit_body(const std::string& eoa_address,
                                     const std::string& proxy_address,
                                     const std::vector<uint8_t>& proxy_data,
                                     uint64_t nonce,
                                     const uint8_t signature[65],
                                     uint64_t gas_limit,
                                     const std::string& relay_address) {
    // Hex-encode the proxy call data
    std::string data_hex = "0x" + hex_encode(proxy_data.data(), proxy_data.size());

    // Signature: r(32) + s(32) + v(1), standard v=27/28, no adjustment for proxy
    std::string sig_hex = "0x" + hex_encode(signature, 65);

    std::string nonce_str = std::to_string(nonce);
    std::string gas_limit_str = std::to_string(gas_limit);

    // EIP-55 checksum addresses
    uint8_t eoa_bytes[20]{}, proxy_bytes[20]{};
    parse_eth_address(eoa_address, eoa_bytes);
    std::string eoa_cs = eip55_checksum(eoa_bytes);
    parse_eth_address(proxy_address, proxy_bytes);
    std::string proxy_cs = eip55_checksum(proxy_bytes);
    std::string factory_cs = eip55_checksum(PROXY_FACTORY_ADDRESS);
    std::string relay_hub_cs = eip55_checksum(RELAY_HUB_ADDRESS);

    // Build JSON (field names match Python SDK TransactionRequest.to_dict())
    std::string body;
    body.reserve(4096);
    body += "{";
    body += "\"type\":\"PROXY\",";
    body += "\"from\":\"" + eoa_cs + "\",";
    body += "\"to\":\"" + factory_cs + "\",";
    body += "\"proxyWallet\":\"" + proxy_cs + "\",";
    body += "\"data\":\"" + data_hex + "\",";
    body += "\"nonce\":\"" + nonce_str + "\",";
    body += "\"signature\":\"" + sig_hex + "\",";
    body += "\"signatureParams\":{";
    body += "\"gasPrice\":\"0\",";
    body += "\"gasLimit\":\"" + gas_limit_str + "\",";
    body += "\"relayerFee\":\"0\",";
    body += "\"relayHub\":\"" + relay_hub_cs + "\",";
    body += "\"relay\":\"" + relay_address + "\"";
    body += "},";
    body += "\"metadata\":\"\"";
    body += "}";

    return body;
}

}  // namespace

struct InventoryService::Impl {
    SpscQueue<InventoryOpRequest>& queue_;
    Metrics& metrics_;
    AsyncLogger& logger_;
    InventoryServiceConfig config_;
    ProducerHandle log_handle_;
    std::atomic<bool> stop_requested_{false};
    TokenInventory* inventory_ = nullptr;

    // Relayer components (initialized once at startup)
    std::unique_ptr<RelayerClient> relayer_;
    std::unique_ptr<Secp256k1OrderSigner> signer_;
    std::string eoa_address_;
    uint8_t eoa_bytes_[20]{};     // parsed once at init, reused per request
    std::string proxy_address_;   // CREATE2-derived proxy wallet
    bool relayer_ready_ = false;

    // Direct RPC components (alternative to relayer)
    std::unique_ptr<PolygonRpcClient> rpc_client_;
    bool rpc_ready_ = false;

    Impl(SpscQueue<InventoryOpRequest>& queue,
         Metrics& metrics,
         AsyncLogger& logger,
         const InventoryServiceConfig& config,
         TokenInventory* inventory)
        : queue_(queue),
          metrics_(metrics),
          logger_(logger),
          config_(config),
          log_handle_(logger.create_producer("inventory")),
          inventory_(inventory) {

        // Initialize signing + address derivation (shared by both paths)
        if (config_.has_private_key) {
            signer_ = std::make_unique<Secp256k1OrderSigner>(config_.private_key);

            if (signer_->get_signer_address(eoa_bytes_)) {
                eoa_address_ = "0x" + hex_encode(eoa_bytes_, 20);
                proxy_address_ = derive_proxy_address(eoa_bytes_);
            }

            // Zero private key from config after copying to signer
            volatile uint8_t* pk = const_cast<volatile uint8_t*>(config_.private_key);
            for (int i = 0; i < 32; ++i) pk[i] = 0;
        }

        // Initialize transport: direct RPC or relayer
        if (config_.use_direct_rpc && config_.has_private_key &&
            !config_.polygon_rpc_url.empty()) {
            rpc_client_ = std::make_unique<PolygonRpcClient>(
                config_.polygon_rpc_url, config_.rpc_timeout_ms);
            rpc_ready_ = true;
        } else if (!config_.use_direct_rpc && config_.has_private_key &&
                   !config_.builder_api_key.empty()) {
            RelayerConfig rcfg;
            rcfg.host = config_.relayer_host;
            rcfg.port = config_.relayer_port;
            rcfg.poll_interval_ms = config_.relayer_poll_interval_ms;
            rcfg.max_poll_attempts = config_.relayer_max_poll_attempts;
            rcfg.timeout_ms = config_.relayer_timeout_ms;

            relayer_ = std::make_unique<RelayerClient>(
                rcfg, config_.builder_api_key,
                config_.builder_api_secret_b64,
                config_.builder_api_passphrase);
            relayer_ready_ = true;
        }
    }

    void run() {
        AsyncLogger::log(log_handle_, LogLevel::INFO, "InventoryService started");

        if (rpc_ready_) {
            char msg[LogEntry::kMaxMsg];
            std::snprintf(msg, sizeof(msg),
                          "Direct RPC: url=%s proxy=%s eoa=%s max_gas=%lld gwei dry_run=%d",
                          config_.polygon_rpc_url.c_str(),
                          proxy_address_.c_str(),
                          eoa_address_.c_str(),
                          static_cast<long long>(config_.max_gas_price_gwei),
                          config_.dry_run ? 1 : 0);
            AsyncLogger::log(log_handle_, LogLevel::INFO, msg);
        } else if (relayer_ready_) {
            char msg[LogEntry::kMaxMsg];
            std::snprintf(msg, sizeof(msg),
                          "PROXY relayer: host=%s proxy=%s eoa=%s dry_run=%d",
                          config_.relayer_host.c_str(),
                          proxy_address_.c_str(),
                          eoa_address_.c_str(),
                          config_.dry_run ? 1 : 0);
            AsyncLogger::log(log_handle_, LogLevel::INFO, msg);
        } else {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Inventory transport NOT initialized (missing credentials/config)");
        }

        while (!stop_requested_.load(std::memory_order_relaxed)) {
            bool did_work = false;
            while (auto* req = queue_.front()) {
                did_work = true;
                InventoryOpRequest r = *req;
                queue_.pop();
                handle_request(r);
            }

            if (!did_work) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::max<int64_t>(1, config_.poll_sleep_ms)));
            }
        }

        AsyncLogger::log(log_handle_, LogLevel::INFO, "InventoryService stopped");
    }

    void request_shutdown() {
        stop_requested_.store(true, std::memory_order_relaxed);
    }

    // Submit a signed PROXY transaction to the relayer and poll for result.
    // target_contract: contract being called (e.g., CTF or USDC.e)
    // inner_call: the ABI-encoded call data (before proxy wrapping)
    // Returns the terminal state ("STATE_CONFIRMED", "STATE_FAILED", etc.) or empty on error.
    std::string submit_proxy_tx(const char* label,
                                 const uint8_t target_contract[20],
                                 const AbiEncodedCall& inner_call) {
        // 1. Get relay payload (nonce + relay address)
        AsyncLogger::log(log_handle_, LogLevel::INFO, "Requesting relay payload...");
        auto payload_result = relayer_->get_relay_payload(eoa_address_);
        if (!payload_result.ok()) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err), "Failed to get relay payload: %s | %s",
                          error_name(payload_result.error),
                          relayer_->last_error().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }
        const auto& payload = payload_result.value;
        {
            char info[LogEntry::kMaxMsg];
            std::snprintf(info, sizeof(info),
                          "Relay payload: nonce=%llu relay=%s",
                          static_cast<unsigned long long>(payload.nonce),
                          payload.relay_address.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, info);
        }

        // 2. Encode the inner call in proxy wrapper
        std::vector<uint8_t> proxy_data = encode_proxy_call_data(
            target_contract, inner_call.data);

        // 3. Build proxy struct hash
        uint8_t relay_bytes[20]{};
        if (!parse_eth_address(payload.relay_address, relay_bytes)) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Failed to parse relay address");
            return "";
        }

        ProxyTxParams params;
        std::memcpy(params.from, eoa_bytes_, 20);
        std::memcpy(params.to, PROXY_FACTORY_ADDRESS, 20);
        params.data = proxy_data;
        params.tx_fee = 0;
        params.gas_price = 0;
        params.gas_limit = PROXY_DEFAULT_GAS_LIMIT;
        params.nonce = payload.nonce;
        std::memcpy(params.relay_hub, RELAY_HUB_ADDRESS, 20);
        std::memcpy(params.relay, relay_bytes, 20);

        Bytes32 struct_hash = create_proxy_struct_hash(params);

        // 4. Apply eth_sign prefix and sign
        Bytes32 signing_hash = eth_sign_hash(struct_hash);

        uint8_t signature[65]{};
        if (!signer_->sign_hash(signing_hash, signature)) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Failed to sign proxy transaction");
            return "";
        }

        // 5. Build submit body
        std::string body = build_proxy_submit_body(
            eoa_address_, proxy_address_, proxy_data,
            payload.nonce, signature,
            PROXY_DEFAULT_GAS_LIMIT, payload.relay_address);

        {
            char submit_msg[LogEntry::kMaxMsg];
            std::snprintf(submit_msg, sizeof(submit_msg),
                          "Submitting PROXY %s: nonce=%llu data_len=%zu",
                          label,
                          static_cast<unsigned long long>(payload.nonce),
                          proxy_data.size());
            AsyncLogger::log(log_handle_, LogLevel::INFO, submit_msg);
        }
        {
            char body_msg[LogEntry::kMaxMsg];
            std::snprintf(body_msg, sizeof(body_msg),
                          "Submit body: %.240s", body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, body_msg);
        }

        // 6. POST /submit
        auto submit_result = relayer_->submit(body);
        if (!submit_result.ok()) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err), "Submit failed: %s | %s",
                          error_name(submit_result.error),
                          relayer_->last_error().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }

        const auto& sr = submit_result.value;
        {
            char sub_msg[LogEntry::kMaxMsg];
            std::snprintf(sub_msg, sizeof(sub_msg),
                          "Submitted: tx_id=%s tx_hash=%s state=%s",
                          sr.transaction_id.c_str(), sr.transaction_hash.c_str(),
                          sr.state.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, sub_msg);
        }

        // Check immediate failure
        if (sr.state == "STATE_FAILED" || sr.state == "STATE_INVALID") {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err),
                          "%s immediately %s: tx_id=%s",
                          label, sr.state.c_str(), sr.transaction_id.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return sr.state;
        }

        // 7. Poll until terminal
        int max_attempts = config_.relayer_max_poll_attempts;
        int64_t poll_ms = config_.relayer_poll_interval_ms;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (stop_requested_.load(std::memory_order_relaxed)) return "";

            auto state_result = relayer_->get_transaction_state(sr.transaction_id);
            if (state_result.ok()) {
                char poll_msg[LogEntry::kMaxMsg];
                std::snprintf(poll_msg, sizeof(poll_msg),
                              "Poll %d/%d: state=%s tx_id=%s",
                              attempt + 1, max_attempts,
                              state_result.value.c_str(),
                              sr.transaction_id.c_str());
                AsyncLogger::log(log_handle_, LogLevel::INFO, poll_msg);

                if (state_result.value == "STATE_CONFIRMED" ||
                    state_result.value == "STATE_FAILED" ||
                    state_result.value == "STATE_INVALID") {
                    return state_result.value;
                }
            } else {
                char poll_err[LogEntry::kMaxMsg];
                std::snprintf(poll_err, sizeof(poll_err),
                              "Poll %d/%d FAILED: %s | %s",
                              attempt + 1, max_attempts,
                              error_name(state_result.error),
                              relayer_->last_error().c_str());
                AsyncLogger::log(log_handle_, LogLevel::WARN, poll_err);
            }
            // Sleep in short chunks to allow quick shutdown
            for (int64_t slept = 0; slept < poll_ms; slept += 100) {
                if (stop_requested_.load(std::memory_order_relaxed)) return "";
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min<int64_t>(100, poll_ms - slept)));
            }
        }
        {
            char timeout_msg[LogEntry::kMaxMsg];
            std::snprintf(timeout_msg, sizeof(timeout_msg),
                          "Poll timed out after %d attempts for tx_id=%s",
                          max_attempts, sr.transaction_id.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, timeout_msg);
        }
        return "";  // timed out
    }

    // Submit via direct Polygon RPC (replaces relayer path).
    std::string submit_direct_rpc_tx(const char* label,
                                      const uint8_t target_contract[20],
                                      const AbiEncodedCall& inner_call) {
        // 1. Encode proxy call data (same as relayer path)
        std::vector<uint8_t> proxy_data = encode_proxy_call_data(
            target_contract, inner_call.data);

        // 2. Query gas price and check against max
        auto gas_result = rpc_client_->get_gas_price();
        if (!gas_result.ok()) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err), "eth_gasPrice failed: %s",
                          rpc_client_->last_error().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }
        uint64_t gas_price_wei = gas_result.value;
        uint64_t gas_price_gwei = gas_price_wei / 1'000'000'000ULL;
        if (static_cast<int64_t>(gas_price_gwei) > config_.max_gas_price_gwei) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err),
                          "Gas too high: %llu gwei > max %lld gwei, skipping %s",
                          static_cast<unsigned long long>(gas_price_gwei),
                          static_cast<long long>(config_.max_gas_price_gwei), label);
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }

        // 3. Query nonce
        auto nonce_result = rpc_client_->get_transaction_count(eoa_address_);
        if (!nonce_result.ok()) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err), "eth_getTransactionCount failed: %s",
                          rpc_client_->last_error().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }

        // 4. Build and sign raw transaction
        RawTxParams tx_params;
        tx_params.nonce = nonce_result.value;
        tx_params.gas_price = gas_price_wei;
        tx_params.gas_limit = config_.gas_limit;
        std::memcpy(tx_params.to, PROXY_FACTORY_ADDRESS, 20);
        tx_params.value = 0;
        tx_params.data = std::move(proxy_data);
        tx_params.chain_id = 137;  // Polygon

        {
            char info[LogEntry::kMaxMsg];
            std::snprintf(info, sizeof(info),
                          "RPC %s: nonce=%llu gas=%llu gwei data_len=%zu",
                          label,
                          static_cast<unsigned long long>(tx_params.nonce),
                          static_cast<unsigned long long>(gas_price_gwei),
                          tx_params.data.size());
            AsyncLogger::log(log_handle_, LogLevel::INFO, info);
        }

        auto signed_tx = build_signed_transaction(tx_params, *signer_);
        if (signed_tx.empty()) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Failed to sign raw transaction");
            return "";
        }

        // 5. Compute tx hash for logging
        Bytes32 tx_hash_bytes = compute_tx_hash(signed_tx);
        std::string tx_hash = "0x" + hex_encode(tx_hash_bytes.data(), 32);

        // 6. Submit via eth_sendRawTransaction
        auto send_result = rpc_client_->send_raw_transaction(signed_tx);
        if (!send_result.ok()) {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err),
                          "eth_sendRawTransaction failed: %s",
                          rpc_client_->last_error().c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            return "";
        }

        {
            char sub_msg[LogEntry::kMaxMsg];
            std::snprintf(sub_msg, sizeof(sub_msg),
                          "Submitted: tx_hash=%s", tx_hash.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, sub_msg);
        }

        // 7. Poll for receipt
        int max_attempts = config_.relayer_max_poll_attempts;
        int64_t poll_ms = config_.relayer_poll_interval_ms;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            if (stop_requested_.load(std::memory_order_relaxed)) return "";

            auto receipt_result = rpc_client_->get_transaction_receipt(tx_hash);
            if (receipt_result.ok()) {
                const auto& receipt = receipt_result.value;
                char poll_msg[LogEntry::kMaxMsg];
                std::snprintf(poll_msg, sizeof(poll_msg),
                              "Receipt: status=%llu gas_used=%llu block=%llu",
                              static_cast<unsigned long long>(receipt.status),
                              static_cast<unsigned long long>(receipt.gas_used),
                              static_cast<unsigned long long>(receipt.block_number));
                AsyncLogger::log(log_handle_, LogLevel::INFO, poll_msg);

                return receipt.status == 1 ? "STATE_CONFIRMED" : "STATE_FAILED";
            }
            // JSON_MISSING_FIELD = null receipt (still pending)

            for (int64_t slept = 0; slept < poll_ms; slept += 100) {
                if (stop_requested_.load(std::memory_order_relaxed)) return "";
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    std::min<int64_t>(100, poll_ms - slept)));
            }
        }
        {
            char timeout_msg[LogEntry::kMaxMsg];
            std::snprintf(timeout_msg, sizeof(timeout_msg),
                          "Receipt poll timed out after %d attempts for %s",
                          max_attempts, tx_hash.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, timeout_msg);
        }
        return "";
    }

    // Unified dispatch: direct RPC or relayer
    std::string submit_inventory_tx(const char* label,
                                     const uint8_t target_contract[20],
                                     const AbiEncodedCall& inner_call) {
        if (rpc_ready_)
            return submit_direct_rpc_tx(label, target_contract, inner_call);
        return submit_proxy_tx(label, target_contract, inner_call);
    }

    // Ensure USDC.e is approved for CTF contract spending (one-time)
    bool usdc_approved_ = false;
    void ensure_usdc_approved() {
        if (usdc_approved_ || config_.dry_run) return;

        AsyncLogger::log(log_handle_, LogLevel::INFO,
                         "Approving USDC.e for CTF contract (one-time)...");

        AbiEncodedCall approve_call = encode_usdc_approve_ctf();
        std::string result = submit_inventory_tx("USDC_approve", USDC_E_ADDRESS, approve_call);

        if (result == "STATE_CONFIRMED") {
            AsyncLogger::log(log_handle_, LogLevel::INFO, "USDC.e approval CONFIRMED");
            usdc_approved_ = true;
        } else {
            char err[LogEntry::kMaxMsg];
            std::snprintf(err, sizeof(err),
                          "USDC.e approval failed (state=%s). Split will likely fail.",
                          result.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, err);
            // Don't block — try the split anyway; it may already be approved
            usdc_approved_ = true;  // don't retry every request
        }
    }

    void handle_request(const InventoryOpRequest& req) {
        char msg[LogEntry::kMaxMsg];
        std::snprintf(msg, sizeof(msg),
                      "Inventory request: id=%u op=%s cond=%.*s qty=%lld",
                      req.request_id,
                      inventory_op_name(req.type),
                      static_cast<int>(req.condition_id.len), req.condition_id.data,
                      static_cast<long long>(req.quantity));
        AsyncLogger::log(log_handle_, LogLevel::INFO, msg);

        if (!relayer_ready_ && !rpc_ready_) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "No inventory transport ready; dropping request");
            return;
        }

        // 1. ABI-encode the CTF call
        Bytes32 condition_bytes{};
        if (!hex_decode_to_bytes32(
                std::string_view(req.condition_id.data, req.condition_id.len),
                condition_bytes)) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Failed to decode condition_id hex; dropping request");
            return;
        }

        AbiEncodedCall call;
        uint64_t amount = static_cast<uint64_t>(req.quantity);
        switch (req.type) {
            case InventoryOpType::SPLIT:
                call = encode_split_position(condition_bytes, amount);
                break;
            case InventoryOpType::MERGE:
                call = encode_merge_positions(condition_bytes, amount);
                break;
            case InventoryOpType::REDEEM:
                call = encode_redeem_positions(condition_bytes);
                break;
        }

        // Determine if this operation succeeds (real tx or dry_run simulation)
        bool confirmed = false;

        if (config_.dry_run) {
            std::string data_hex = hex_encode(call.data.data(), call.data.size());
            char dry[LogEntry::kMaxMsg];
            std::snprintf(dry, sizeof(dry),
                          "DRY_RUN: op=%s data_len=%zu data=0x%.100s...",
                          inventory_op_name(req.type),
                          call.data.size(),
                          data_hex.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, dry);
            confirmed = true;  // Simulate success for position tracking
        } else {
            // Ensure USDC is approved for CTF before split
            if (req.type == InventoryOpType::SPLIT) {
                ensure_usdc_approved();
            }

            // Submit the CTF call via direct RPC or relayer
            std::string final_state = submit_inventory_tx(
                inventory_op_name(req.type), CTF_CONTRACT_ADDRESS, call);

            {
                char done_msg[LogEntry::kMaxMsg];
                std::snprintf(done_msg, sizeof(done_msg),
                              "Inventory %s %s: state=%s",
                              inventory_op_name(req.type),
                              final_state == "STATE_CONFIRMED" ? "CONFIRMED" : "FAILED",
                              final_state.c_str());
                LogLevel level = (final_state == "STATE_CONFIRMED")
                    ? LogLevel::INFO : LogLevel::WARN;
                AsyncLogger::log(log_handle_, level, done_msg);
            }
            confirmed = (final_state == "STATE_CONFIRMED");
        }

        // Update TokenInventory after confirmed (or simulated) split/merge/redeem
        if (confirmed && inventory_) {
            if (req.token_id_up.len == 0 || req.token_id_down.len == 0) {
                char warn[LogEntry::kMaxMsg];
                std::snprintf(warn, sizeof(warn),
                              "TokenInventory skip: missing token_ids up_len=%u down_len=%u",
                              req.token_id_up.len, req.token_id_down.len);
                AsyncLogger::log(log_handle_, LogLevel::WARN, warn);
            } else {
                Qty_t qty = req.quantity;
                // Read before values for diagnostic logging
                Qty_t before_up = inventory_->position_for(req.token_id_up);
                Qty_t before_down = inventory_->position_for(req.token_id_down);

                switch (req.type) {
                    case InventoryOpType::SPLIT:
                        inventory_->adjust_position(req.token_id_up, qty);
                        inventory_->adjust_position(req.token_id_down, qty);
                        // Split: 1 USDC → 1 YES + 1 NO. qty is fp6 = micro-USDC cost.
                        inventory_->adjust_usdc_balance(-qty);
                        break;
                    case InventoryOpType::MERGE:
                        inventory_->adjust_position(req.token_id_up, -qty);
                        inventory_->adjust_position(req.token_id_down, -qty);
                        // Merge: 1 YES + 1 NO → 1 USDC. qty is fp6 = micro-USDC received.
                        inventory_->adjust_usdc_balance(qty);
                        break;
                    case InventoryOpType::REDEEM:
                        // Redeem: winning tokens → USDC. Credit the redeemed amount.
                        // Both sides go to 0; the larger position is the payout.
                        inventory_->adjust_usdc_balance(
                            std::max(before_up, before_down));
                        inventory_->set_position(req.token_id_up, 0);
                        inventory_->set_position(req.token_id_down, 0);
                        break;
                }

                Qty_t after_up = inventory_->position_for(req.token_id_up);
                Qty_t after_down = inventory_->position_for(req.token_id_down);

                char inv_msg[LogEntry::kMaxMsg];
                std::snprintf(inv_msg, sizeof(inv_msg),
                              "TokenInventory updated: op=%s qty=%lld "
                              "up=%.*s [%lld->%lld] down=%.*s [%lld->%lld]",
                              inventory_op_name(req.type),
                              static_cast<long long>(qty),
                              static_cast<int>(req.token_id_up.len), req.token_id_up.data,
                              static_cast<long long>(before_up),
                              static_cast<long long>(after_up),
                              static_cast<int>(req.token_id_down.len), req.token_id_down.data,
                              static_cast<long long>(before_down),
                              static_cast<long long>(after_down));
                AsyncLogger::log(log_handle_, LogLevel::INFO, inv_msg);

                // Warn if positions didn't change (token not registered in TokenInventory)
                if (before_up == after_up && before_down == after_down &&
                    req.type != InventoryOpType::REDEEM) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "TokenInventory positions unchanged! "
                                     "Tokens may not be registered.");
                }
            }
        } else if (confirmed && !inventory_) {
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "TokenInventory ptr is null; cannot update positions");
        }
    }
};

InventoryService::InventoryService(SpscQueue<InventoryOpRequest>& queue,
                                   Metrics& metrics,
                                   AsyncLogger& logger,
                                   const InventoryServiceConfig& config,
                                   TokenInventory* inventory)
    : impl_(std::make_unique<Impl>(queue, metrics, logger, config, inventory)) {}

InventoryService::~InventoryService() = default;

void InventoryService::run() {
    impl_->run();
}

void InventoryService::request_shutdown() {
    impl_->request_shutdown();
}

}  // namespace lt
