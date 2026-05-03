#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include "crypto/hex_utils.h"

namespace lt {

class OrderSigner;

struct RawTxParams {
    uint64_t nonce = 0;
    uint64_t gas_price = 0;      // wei
    uint64_t gas_limit = 500000;
    uint8_t to[20]{};            // destination contract
    uint64_t value = 0;          // ETH/MATIC value in wei (0 for contract calls)
    std::vector<uint8_t> data;   // calldata
    uint64_t chain_id = 137;     // Polygon mainnet
};

// Build a signed legacy (type 0) Ethereum transaction with EIP-155 replay protection.
// Returns RLP-encoded signed transaction bytes. Empty on failure.
std::vector<uint8_t> build_signed_transaction(
    const RawTxParams& params,
    OrderSigner& signer);

// Compute transaction hash from signed RLP bytes: keccak256(signed_rlp).
Bytes32 compute_tx_hash(const std::vector<uint8_t>& signed_tx);

}  // namespace lt
