#pragma once

#include <cstdint>
#include <array>
#include "crypto/hex_utils.h"

namespace lt {

// EIP-712 order fields used by Polymarket CTF Exchange
struct OrderFields {
    Bytes32 salt;
    uint8_t maker[20]{};          // maker address
    uint8_t signer[20]{};         // signer address
    uint8_t taker[20]{};          // taker address (0x0 = any)
    uint8_t token_id[32]{};       // tokenId as uint256
    uint8_t maker_amount[32]{};   // makerAmount as uint256
    uint8_t taker_amount[32]{};   // takerAmount as uint256
    uint64_t expiration = 0;
    uint64_t nonce = 0;
    uint64_t fee_rate_bps = 0;
    uint8_t side = 0;             // 0 = BUY, 1 = SELL
    uint8_t signature_type = 0;   // 0 = EOA, 1 = POLY_PROXY, 2 = POLY_GNOSIS_SAFE
};

// Pre-computed EIP-712 type hashes (Polymarket CTF Exchange)
// ORDER_TYPEHASH = keccak256("Order(uint256 salt,address maker,address signer,address taker,uint256 tokenId,uint256 makerAmount,uint256 takerAmount,uint256 expiration,uint256 nonce,uint256 feeRateBps,uint8 side,uint8 signatureType)")
extern const Bytes32 ORDER_TYPEHASH;

// DOMAIN_TYPEHASH = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
extern const Bytes32 DOMAIN_TYPEHASH;

// Polygon chain ID
inline constexpr uint64_t POLYGON_CHAIN_ID = 137;

// Exchange contract addresses
extern const uint8_t CTF_EXCHANGE_ADDRESS[20];
extern const uint8_t NEG_RISK_CTF_EXCHANGE_ADDRESS[20];

// Compute domain separator for the given exchange contract address
Bytes32 compute_domain_separator(const uint8_t exchange_address[20]);

// Overload with explicit chain ID (for testing against other networks)
Bytes32 compute_domain_separator(const uint8_t exchange_address[20], uint64_t chain_id);

// Hash the order struct per EIP-712 encoding rules
Bytes32 hash_order_struct(const OrderFields& fields);

// Compute the final EIP-712 signing hash:
// keccak256(0x19 || 0x01 || domainSeparator || structHash)
Bytes32 eip712_signing_hash(const Bytes32& domain_sep, const Bytes32& struct_hash);

}  // namespace lt
