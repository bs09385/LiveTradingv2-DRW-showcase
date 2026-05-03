#include "inventory/proxy_tx_builder.h"
#include "crypto/keccak.h"

#include <cstring>

namespace lt {

// Proxy factory: 0xaB45c5A4B0c941a2F231C04C3f49182e1A254052
const uint8_t PROXY_FACTORY_ADDRESS[20] = {
    0xaB, 0x45, 0xc5, 0xA4, 0xB0, 0xc9, 0x41, 0xa2, 0xF2, 0x31,
    0xC0, 0x4C, 0x3f, 0x49, 0x18, 0x2e, 0x1A, 0x25, 0x40, 0x52
};

// Relay hub: 0xD216153c06E857cD7f72665E0aF1d7D82172F494
const uint8_t RELAY_HUB_ADDRESS[20] = {
    0xD2, 0x16, 0x15, 0x3c, 0x06, 0xE8, 0x57, 0xcD, 0x7f, 0x72,
    0x66, 0x5E, 0x0a, 0xF1, 0xd7, 0xD8, 0x21, 0x72, 0xF4, 0x94
};

// PROXY_INIT_CODE_HASH: 0xd21df8dc65880a8606f09fe0ce3df9b8869287ab0b058be05aa9e8af6330a00b
static const uint8_t PROXY_INIT_CODE_HASH[32] = {
    0xd2, 0x1d, 0xf8, 0xdc, 0x65, 0x88, 0x0a, 0x86, 0x06, 0xf0, 0x9f, 0xe0, 0xce, 0x3d, 0xf9, 0xb8,
    0x86, 0x92, 0x87, 0xab, 0x0b, 0x05, 0x8b, 0xe0, 0x5a, 0xa9, 0xe8, 0xaf, 0x63, 0x30, 0xa0, 0x0b
};

// Compute proxy function selector at startup
static Bytes32 proxy_selector_hash() {
    const char* sig = "proxy((uint8,address,uint256,bytes)[])";
    return keccak256(reinterpret_cast<const uint8_t*>(sig), std::strlen(sig));
}
static const Bytes32 kProxySelectorHash = proxy_selector_hash();

const uint8_t PROXY_FUNCTION_SELECTOR[4] = {
    kProxySelectorHash[0], kProxySelectorHash[1],
    kProxySelectorHash[2], kProxySelectorHash[3]
};

std::string derive_proxy_address(const uint8_t eoa[20]) {
    // CREATE2: address = keccak256(0xff + factory + salt + initCodeHash)[12:]
    // salt = keccak256(encode_packed(eoa)) = keccak256(raw 20 bytes)
    // (NOT 32-byte abi.encode like Safe — proxy uses encode_packed = raw bytes)

    Bytes32 salt = keccak256(eoa, 20);

    // CREATE2 input: 0xff(1) + factory(20) + salt(32) + initCodeHash(32) = 85 bytes
    uint8_t buf[85];
    buf[0] = 0xff;
    std::memcpy(buf + 1, PROXY_FACTORY_ADDRESS, 20);
    std::memcpy(buf + 21, salt.data(), 32);
    std::memcpy(buf + 53, PROXY_INIT_CODE_HASH, 32);

    Bytes32 addr_hash = keccak256(buf, sizeof(buf));

    // Last 20 bytes = derived address
    return "0x" + hex_encode(addr_hash.data() + 12, 20);
}

namespace {

// Write a uint256 big-endian (32 bytes) into a vector
void write_uint256_vec(std::vector<uint8_t>& out, uint64_t val) {
    uint8_t buf[32]{};
    uint64_to_uint256_be(val, buf);
    out.insert(out.end(), buf, buf + 32);
}

// Write an address left-padded to 32 bytes into a vector
void write_address32_vec(std::vector<uint8_t>& out, const uint8_t addr[20]) {
    out.insert(out.end(), 12, 0);
    out.insert(out.end(), addr, addr + 20);
}

}  // namespace

std::vector<uint8_t> encode_proxy_call_data(
    const uint8_t target_contract[20],
    const std::vector<uint8_t>& inner_call_data) {
    // ABI layout for proxy((uint8,address,uint256,bytes)[]) with 1 element:
    //
    // [selector: 4]
    // Byte 0:   offset to array = 0x20 (32)
    // Byte 32:  array length = 1
    // Byte 64:  offset to element[0] = 0x20 (32, relative to this word)
    // Byte 96:  type_code uint8 = 1 (Call), padded to 32
    // Byte 128: to address, left-padded to 32
    // Byte 160: value uint256 = 0
    // Byte 192: offset to bytes data = 0x80 (128, relative to tuple start at byte 96)
    // Byte 224: bytes length
    // Byte 256: bytes data, right-padded to 32-byte boundary

    size_t padded_data_len = ((inner_call_data.size() + 31) / 32) * 32;
    size_t total = 4 + 8 * 32 + padded_data_len;

    std::vector<uint8_t> out;
    out.reserve(total);

    // Function selector
    out.insert(out.end(), PROXY_FUNCTION_SELECTOR, PROXY_FUNCTION_SELECTOR + 4);

    // Offset to array data = 32
    write_uint256_vec(out, 32);

    // Array length = 1
    write_uint256_vec(out, 1);

    // Offset to element[0] = 32 (relative to this word = after all offset words)
    write_uint256_vec(out, 32);

    // Tuple element[0]:
    // type_code = 1 (Call)
    write_uint256_vec(out, 1);

    // to = target contract address
    write_address32_vec(out, target_contract);

    // value = 0
    write_uint256_vec(out, 0);

    // offset to bytes data = 128 (4 * 32, from tuple start)
    write_uint256_vec(out, 128);

    // bytes length
    write_uint256_vec(out, inner_call_data.size());

    // bytes data
    out.insert(out.end(), inner_call_data.begin(), inner_call_data.end());

    // Right-pad to 32-byte boundary
    size_t pad = padded_data_len - inner_call_data.size();
    if (pad > 0) {
        out.insert(out.end(), pad, 0);
    }

    return out;
}

Bytes32 create_proxy_struct_hash(const ProxyTxParams& params) {
    // Hash = keccak256(
    //   "rlx:"           (4 bytes)
    //   + from           (20 bytes, raw address)
    //   + to             (20 bytes, raw address = proxy factory)
    //   + data           (variable, raw bytes of proxy-encoded call)
    //   + txFee          (32 bytes, uint256 big-endian)
    //   + gasPrice       (32 bytes, uint256 big-endian)
    //   + gasLimit       (32 bytes, uint256 big-endian)
    //   + nonce          (32 bytes, uint256 big-endian)
    //   + relayHub       (20 bytes, raw address)
    //   + relay          (20 bytes, raw address)
    // )

    size_t total = 4 + 20 + 20 + params.data.size() + 4 * 32 + 20 + 20;
    std::vector<uint8_t> msg;
    msg.reserve(total);

    // "rlx:" prefix
    static const uint8_t PREFIX[] = {'r', 'l', 'x', ':'};
    msg.insert(msg.end(), PREFIX, PREFIX + 4);

    // from (20 bytes, raw)
    msg.insert(msg.end(), params.from, params.from + 20);

    // to (20 bytes, raw)
    msg.insert(msg.end(), params.to, params.to + 20);

    // data (variable)
    msg.insert(msg.end(), params.data.begin(), params.data.end());

    // txFee (32 bytes big-endian)
    {
        uint8_t buf[32]{};
        uint64_to_uint256_be(params.tx_fee, buf);
        msg.insert(msg.end(), buf, buf + 32);
    }

    // gasPrice (32 bytes big-endian)
    {
        uint8_t buf[32]{};
        uint64_to_uint256_be(params.gas_price, buf);
        msg.insert(msg.end(), buf, buf + 32);
    }

    // gasLimit (32 bytes big-endian)
    {
        uint8_t buf[32]{};
        uint64_to_uint256_be(params.gas_limit, buf);
        msg.insert(msg.end(), buf, buf + 32);
    }

    // nonce (32 bytes big-endian)
    {
        uint8_t buf[32]{};
        uint64_to_uint256_be(params.nonce, buf);
        msg.insert(msg.end(), buf, buf + 32);
    }

    // relayHub (20 bytes, raw)
    msg.insert(msg.end(), params.relay_hub, params.relay_hub + 20);

    // relay (20 bytes, raw)
    msg.insert(msg.end(), params.relay, params.relay + 20);

    return keccak256(msg.data(), msg.size());
}

}  // namespace lt
