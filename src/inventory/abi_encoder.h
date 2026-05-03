#pragma once

#include <cstdint>
#include <vector>

#include "crypto/hex_utils.h"

namespace lt {

// ABI-encoded CTF contract call data.
struct AbiEncodedCall {
    std::vector<uint8_t> data;
};

// CTF contract address on Polygon: 0x4D97DCd97eC945f40cF65F87097ACe5EA0476045
extern const uint8_t CTF_CONTRACT_ADDRESS[20];

// USDC.e collateral token on Polygon: 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174
extern const uint8_t USDC_E_ADDRESS[20];

// ABI-encode splitPosition(address,bytes32,bytes32,uint256[],uint256)
// collateralToken = USDC.e, parentCollectionId = 0, indexSets = [1, 2]
AbiEncodedCall encode_split_position(const Bytes32& condition_id, uint64_t amount_usdc6);

// ABI-encode mergePositions(address,bytes32,bytes32,uint256[],uint256)
// collateralToken = USDC.e, parentCollectionId = 0, indexSets = [1, 2]
AbiEncodedCall encode_merge_positions(const Bytes32& condition_id, uint64_t amount_usdc6);

// ABI-encode redeemPositions(address,bytes32,bytes32,uint256[])
// collateralToken = USDC.e, parentCollectionId = 0, indexSets = [1, 2]
AbiEncodedCall encode_redeem_positions(const Bytes32& condition_id);

// ABI-encode ERC20 approve(address spender, uint256 amount) for USDC.e -> CTF
// amount = type(uint256).max (infinite approval)
AbiEncodedCall encode_usdc_approve_ctf();

// Pre-computed function selectors (first 4 bytes of keccak256 of canonical signature)
extern const uint8_t SPLIT_POSITION_SELECTOR[4];
extern const uint8_t MERGE_POSITIONS_SELECTOR[4];
extern const uint8_t REDEEM_POSITIONS_SELECTOR[4];
extern const uint8_t APPROVE_SELECTOR[4];

}  // namespace lt
