#pragma once

#include <cstdint>
#include <vector>

#include "crypto/hex_utils.h"

namespace lt {

// EIP-712 domain for Gnosis Safe: EIP712Domain(uint256 chainId, address verifyingContract)
// verifyingContract = Safe proxy address, chainId = 137 (Polygon)
extern const Bytes32 SAFE_DOMAIN_TYPEHASH;

// SafeTx(address to,uint256 value,bytes data,uint8 operation,uint256 safeTxGas,
//        uint256 baseGas,uint256 gasPrice,address gasToken,address refundReceiver,uint256 nonce)
extern const Bytes32 SAFE_TX_TYPEHASH;

// Compute domain separator for a specific Safe address on Polygon (chain_id=137).
Bytes32 compute_safe_domain_separator(const uint8_t safe_address[20]);

// Compute domain separator with explicit chain ID (for testing).
Bytes32 compute_safe_domain_separator(const uint8_t safe_address[20], uint64_t chain_id);

// Fields for a Safe transaction. Only `to`, `data`, and `nonce` vary.
// All other fields default to 0/zero-address (gasless relayer pattern).
struct SafeTxFields {
    uint8_t to[20]{};                    // destination contract (e.g. CTF)
    std::vector<uint8_t> data;           // ABI-encoded call
    uint64_t nonce = 0;
    uint8_t operation = 0;               // 0 = CALL, 1 = DELEGATECALL
    // safeTxGas = 0, baseGas = 0, gasPrice = 0,
    // gasToken = address(0), refundReceiver = address(0), value = 0
};

// Hash a SafeTxFields struct per EIP-712 encoding.
Bytes32 hash_safe_tx_struct(const SafeTxFields& fields);

// Compute the final EIP-712 signing hash for a Safe transaction:
// keccak256(0x19 || 0x01 || domainSeparator || structHash)
// Reuses eip712_signing_hash() from crypto/eip712.h.
Bytes32 safe_tx_signing_hash(const Bytes32& domain_sep, const SafeTxFields& fields);

// Derive the Gnosis Safe proxy address for an EOA via CREATE2.
// Uses Polymarket's Safe factory (0xaacFeEa03eb1561C4e67d661e40682Bd20E3541b)
// and SAFE_INIT_CODE_HASH. Returns the 20-byte address as a "0x..." hex string.
std::string derive_safe_address(const uint8_t eoa[20]);

// Apply Ethereum personal sign prefix to a 32-byte hash:
// keccak256("\x19Ethereum Signed Message:\n32" + hash)
// Used for Safe "eth_sign" mode (v > 30): the Safe contract re-applies this
// prefix during signature verification, so we must sign the prefixed hash.
Bytes32 eth_sign_hash(const Bytes32& hash);

}  // namespace lt
