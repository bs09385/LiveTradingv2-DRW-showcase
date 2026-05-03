#include "inventory/abi_encoder.h"
#include "crypto/keccak.h"

#include <cstring>

namespace lt {

// CTF: 0x4D97DCd97eC945f40cF65F87097ACe5EA0476045
const uint8_t CTF_CONTRACT_ADDRESS[20] = {
    0x4D, 0x97, 0xDC, 0xd9, 0x7e, 0xC9, 0x45, 0xf4, 0x0c, 0xF6,
    0x5F, 0x87, 0x09, 0x7A, 0xCe, 0x5E, 0xA0, 0x47, 0x60, 0x45
};

// USDC.e: 0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174
const uint8_t USDC_E_ADDRESS[20] = {
    0x27, 0x91, 0xBc, 0xa1, 0xf2, 0xde, 0x46, 0x61, 0xED, 0x88,
    0xA3, 0x0C, 0x99, 0xA7, 0xa9, 0x44, 0x9A, 0xa8, 0x41, 0x74
};

static Bytes32 split_hash() {
    return keccak256(
        reinterpret_cast<const uint8_t*>("splitPosition(address,bytes32,bytes32,uint256[],uint256)"),
        std::strlen("splitPosition(address,bytes32,bytes32,uint256[],uint256)"));
}

static Bytes32 merge_hash() {
    return keccak256(
        reinterpret_cast<const uint8_t*>("mergePositions(address,bytes32,bytes32,uint256[],uint256)"),
        std::strlen("mergePositions(address,bytes32,bytes32,uint256[],uint256)"));
}

static Bytes32 redeem_hash() {
    return keccak256(
        reinterpret_cast<const uint8_t*>("redeemPositions(address,bytes32,bytes32,uint256[])"),
        std::strlen("redeemPositions(address,bytes32,bytes32,uint256[])"));
}

static const Bytes32 kSplitHash = split_hash();
static const Bytes32 kMergeHash = merge_hash();
static const Bytes32 kRedeemHash = redeem_hash();

const uint8_t SPLIT_POSITION_SELECTOR[4] = {
    kSplitHash[0], kSplitHash[1], kSplitHash[2], kSplitHash[3]
};
const uint8_t MERGE_POSITIONS_SELECTOR[4] = {
    kMergeHash[0], kMergeHash[1], kMergeHash[2], kMergeHash[3]
};
const uint8_t REDEEM_POSITIONS_SELECTOR[4] = {
    kRedeemHash[0], kRedeemHash[1], kRedeemHash[2], kRedeemHash[3]
};

static Bytes32 approve_hash() {
    return keccak256(
        reinterpret_cast<const uint8_t*>("approve(address,uint256)"),
        std::strlen("approve(address,uint256)"));
}
static const Bytes32 kApproveHash = approve_hash();
const uint8_t APPROVE_SELECTOR[4] = {
    kApproveHash[0], kApproveHash[1], kApproveHash[2], kApproveHash[3]
};

namespace {

// Write 32 zero bytes
void write_zero32(std::vector<uint8_t>& out) {
    out.insert(out.end(), 32, 0);
}

// Write address as left-padded bytes32
void write_address32(std::vector<uint8_t>& out, const uint8_t addr[20]) {
    out.insert(out.end(), 12, 0);
    out.insert(out.end(), addr, addr + 20);
}

// Write Bytes32 directly
void write_bytes32(std::vector<uint8_t>& out, const Bytes32& b) {
    out.insert(out.end(), b.begin(), b.end());
}

// Write uint64 as big-endian uint256
void write_uint256(std::vector<uint8_t>& out, uint64_t val) {
    uint8_t buf[32]{};
    uint64_to_uint256_be(val, buf);
    out.insert(out.end(), buf, buf + 32);
}

}  // namespace

// splitPosition / mergePositions ABI layout (260 bytes):
// [selector:4]
// [collateralToken:32]     = USDC.e address
// [parentCollectionId:32]  = bytes32(0)
// [conditionId:32]
// [arrayOffset:32]         = 160 (5 * 32 = offset to dynamic array)
// [amount:32]
// [arrayLen:32]            = 2
// [indexSet[0]:32]          = 1
// [indexSet[1]:32]          = 2

AbiEncodedCall encode_split_position(const Bytes32& condition_id, uint64_t amount_usdc6) {
    AbiEncodedCall call;
    call.data.reserve(260);

    // Selector
    call.data.insert(call.data.end(), SPLIT_POSITION_SELECTOR, SPLIT_POSITION_SELECTOR + 4);

    // collateralToken (USDC.e address, left-padded to 32 bytes)
    write_address32(call.data, USDC_E_ADDRESS);

    // parentCollectionId = bytes32(0)
    write_zero32(call.data);

    // conditionId
    write_bytes32(call.data, condition_id);

    // array offset = 160 (5 * 32 bytes from start of params to dynamic array data)
    write_uint256(call.data, 160);

    // amount
    write_uint256(call.data, amount_usdc6);

    // dynamic array: length = 2
    write_uint256(call.data, 2);

    // indexSet[0] = 1
    write_uint256(call.data, 1);

    // indexSet[1] = 2
    write_uint256(call.data, 2);

    return call;
}

AbiEncodedCall encode_merge_positions(const Bytes32& condition_id, uint64_t amount_usdc6) {
    AbiEncodedCall call;
    call.data.reserve(260);

    call.data.insert(call.data.end(), MERGE_POSITIONS_SELECTOR, MERGE_POSITIONS_SELECTOR + 4);

    write_address32(call.data, USDC_E_ADDRESS);
    write_zero32(call.data);
    write_bytes32(call.data, condition_id);
    write_uint256(call.data, 160);
    write_uint256(call.data, amount_usdc6);
    write_uint256(call.data, 2);
    write_uint256(call.data, 1);
    write_uint256(call.data, 2);

    return call;
}

// redeemPositions ABI layout (228 bytes):
// [selector:4]
// [collateralToken:32]     = USDC.e address
// [parentCollectionId:32]  = bytes32(0)
// [conditionId:32]
// [arrayOffset:32]         = 128 (4 * 32 = offset to dynamic array)
// [arrayLen:32]            = 2
// [indexSet[0]:32]          = 1
// [indexSet[1]:32]          = 2

AbiEncodedCall encode_redeem_positions(const Bytes32& condition_id) {
    AbiEncodedCall call;
    call.data.reserve(228);

    call.data.insert(call.data.end(), REDEEM_POSITIONS_SELECTOR, REDEEM_POSITIONS_SELECTOR + 4);

    write_address32(call.data, USDC_E_ADDRESS);
    write_zero32(call.data);
    write_bytes32(call.data, condition_id);

    // array offset = 128 (4 * 32 bytes from start of params)
    write_uint256(call.data, 128);

    // dynamic array: length = 2
    write_uint256(call.data, 2);
    write_uint256(call.data, 1);
    write_uint256(call.data, 2);

    return call;
}

AbiEncodedCall encode_usdc_approve_ctf() {
    AbiEncodedCall call;
    call.data.reserve(68);

    // approve(address,uint256) selector
    call.data.insert(call.data.end(), APPROVE_SELECTOR, APPROVE_SELECTOR + 4);

    // spender = CTF contract address
    write_address32(call.data, CTF_CONTRACT_ADDRESS);

    // amount = type(uint256).max
    for (int i = 0; i < 32; ++i) call.data.push_back(0xFF);

    return call;
}

}  // namespace lt
