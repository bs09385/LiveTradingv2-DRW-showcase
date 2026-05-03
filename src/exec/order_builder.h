#pragma once

#include <array>
#include <string>
#include <string_view>
#include "common/error.h"
#include "common/types.h"
#include "crypto/order_signer.h"
#include "crypto/eip712.h"
#include "exec/exec_intent.h"

namespace lt {

struct OrderBuilderConfig {
    bool defer_exec = false;
    bool post_only = false;
    uint8_t signature_type = 0;  // 0=EOA
};

struct OrderPayload {
    std::string json_body;
    OrderId client_order_id;
    OrderId predicted_exchange_oid;   // "0x" + hex(signing_hash) — known before REST send
    std::string debug_signing_hash;   // hex of EIP-712 signing hash for debugging
};

static constexpr int kBatchMaxOrders = 15;

struct BatchOrderPayload {
    std::string json_body;
    std::array<OrderId, kBatchMaxOrders> client_order_ids;
    std::array<OrderId, kBatchMaxOrders> predicted_exchange_oids;
    int count = 0;
};

class OrderBuilder {
public:
    // owner_uuid: the POST /order "owner" field
    // maker_address: the maker Ethereum address (hex, 0x-prefixed)
    // signer_address: the signer Ethereum address (hex, 0x-prefixed)
    OrderBuilder(OrderSigner& signer, std::string_view owner_uuid,
                 std::string_view maker_address, std::string_view signer_address,
                 const OrderBuilderConfig& cfg = {});

    // Build a signed POST /order JSON payload from an ExecIntent
    Expected<OrderPayload> build(const ExecIntent& intent);

    // Build a signed POST /orders batch JSON payload from multiple ExecIntents
    Expected<BatchOrderPayload> build_batch(const ExecIntent* intents, int count);

private:
    OrderSigner& signer_;
    std::string owner_uuid_;
    std::string maker_address_;
    std::string signer_address_;
    OrderBuilderConfig config_;
    uint8_t maker_addr_bytes_[20]{};
    uint8_t signer_addr_bytes_[20]{};

    // Cached domain separators (one per exchange contract)
    Bytes32 domain_sep_normal_;
    Bytes32 domain_sep_neg_risk_;

    // Generate a salt (nanosecond timestamp, matching official clients)
    uint64_t generate_salt() const;
};

}  // namespace lt
