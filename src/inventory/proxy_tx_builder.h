#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "crypto/hex_utils.h"

namespace lt {

// Polygon mainnet: Proxy factory
// 0xaB45c5A4B0c941a2F231C04C3f49182e1A254052
extern const uint8_t PROXY_FACTORY_ADDRESS[20];

// Polygon mainnet: Relay hub
// 0xD216153c06E857cD7f72665E0aF1d7D82172F494
extern const uint8_t RELAY_HUB_ADDRESS[20];

// Default gas limit for proxy transactions (matches Python SDK DEFAULT_GAS_LIMIT)
static constexpr uint64_t PROXY_DEFAULT_GAS_LIMIT = 500000;

// Derive the proxy wallet address for an EOA via CREATE2.
// factory = Proxy factory, salt = keccak256(encode_packed(eoa)) = keccak256(raw 20 bytes)
// Returns "0x..." hex string.
std::string derive_proxy_address(const uint8_t eoa[20]);

// Wrap an inner contract call in the proxy((uint8,address,uint256,bytes)[]) ABI encoding.
// target_contract: contract being called (e.g., CTF for split, USDC.e for approve)
// inner_call_data: ABI-encoded call (e.g., from encode_split_position)
// Returns full calldata including the proxy function selector.
std::vector<uint8_t> encode_proxy_call_data(
    const uint8_t target_contract[20],
    const std::vector<uint8_t>& inner_call_data);

// Parameters for creating the proxy struct hash.
struct ProxyTxParams {
    uint8_t from[20]{};         // EOA address
    uint8_t to[20]{};           // proxy factory address
    std::vector<uint8_t> data;  // proxy-encoded call data (from encode_proxy_call_data)
    uint64_t tx_fee = 0;
    uint64_t gas_price = 0;
    uint64_t gas_limit = PROXY_DEFAULT_GAS_LIMIT;
    uint64_t nonce = 0;
    uint8_t relay_hub[20]{};    // relay hub address
    uint8_t relay[20]{};        // relay address (from /relay-payload response)
};

// Create the proxy struct hash:
// keccak256("rlx:" + from(20) + to(20) + data(var) + txFee(32) + gasPrice(32)
//           + gasLimit(32) + nonce(32) + relayHub(20) + relay(20))
// The result should be signed with eth_sign prefix before secp256k1 signing.
Bytes32 create_proxy_struct_hash(const ProxyTxParams& params);

// Proxy function selector: keccak256("proxy((uint8,address,uint256,bytes)[])")[:4]
extern const uint8_t PROXY_FUNCTION_SELECTOR[4];

}  // namespace lt
