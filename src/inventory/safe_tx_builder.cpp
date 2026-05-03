#include "inventory/safe_tx_builder.h"
#include "crypto/keccak.h"
#include "crypto/eip712.h"

#include <cstring>

namespace lt {

// SAFE_DOMAIN_TYPEHASH = keccak256("EIP712Domain(uint256 chainId,address verifyingContract)")
static Bytes32 compute_safe_domain_typehash() {
    const char* type_str = "EIP712Domain(uint256 chainId,address verifyingContract)";
    return keccak256(reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
}

// SAFE_TX_TYPEHASH = keccak256("SafeTx(address to,uint256 value,bytes data,uint8 operation,uint256 safeTxGas,uint256 baseGas,uint256 gasPrice,address gasToken,address refundReceiver,uint256 nonce)")
static Bytes32 compute_safe_tx_typehash() {
    const char* type_str =
        "SafeTx(address to,uint256 value,bytes data,uint8 operation,"
        "uint256 safeTxGas,uint256 baseGas,uint256 gasPrice,"
        "address gasToken,address refundReceiver,uint256 nonce)";
    return keccak256(reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
}

const Bytes32 SAFE_DOMAIN_TYPEHASH = compute_safe_domain_typehash();
const Bytes32 SAFE_TX_TYPEHASH = compute_safe_tx_typehash();

Bytes32 compute_safe_domain_separator(const uint8_t safe_address[20]) {
    return compute_safe_domain_separator(safe_address, POLYGON_CHAIN_ID);
}

Bytes32 compute_safe_domain_separator(const uint8_t safe_address[20], uint64_t chain_id) {
    // domainSeparator = keccak256(abi.encode(
    //   SAFE_DOMAIN_TYPEHASH,
    //   chainId,
    //   verifyingContract
    // ))
    // = 3 * 32 = 96 bytes
    uint8_t buf[96];

    std::memcpy(buf, SAFE_DOMAIN_TYPEHASH.data(), 32);

    Bytes32 chain_id_bytes{};
    uint64_to_uint256_be(chain_id, chain_id_bytes.data());
    std::memcpy(buf + 32, chain_id_bytes.data(), 32);

    Bytes32 addr_bytes = address_to_bytes32(safe_address);
    std::memcpy(buf + 64, addr_bytes.data(), 32);

    return keccak256(buf, 96);
}

Bytes32 hash_safe_tx_struct(const SafeTxFields& fields) {
    // structHash = keccak256(abi.encode(
    //   SAFE_TX_TYPEHASH,
    //   to,
    //   value (0),
    //   keccak256(data),
    //   operation (0),
    //   safeTxGas (0),
    //   baseGas (0),
    //   gasPrice (0),
    //   gasToken (address(0)),
    //   refundReceiver (address(0)),
    //   nonce
    // ))
    // = 11 * 32 = 352 bytes
    uint8_t buf[352];
    std::memset(buf, 0, sizeof(buf));
    size_t off = 0;

    auto write32 = [&](const uint8_t* data) {
        std::memcpy(buf + off, data, 32);
        off += 32;
    };

    // SAFE_TX_TYPEHASH
    write32(SAFE_TX_TYPEHASH.data());

    // to (address, left-padded)
    Bytes32 to_bytes = address_to_bytes32(fields.to);
    write32(to_bytes.data());

    // value = 0 (already zeroed)
    off += 32;

    // keccak256(data) — for bytes type, EIP-712 hashes the value
    Bytes32 data_hash = keccak256(fields.data.data(), fields.data.size());
    write32(data_hash.data());

    // operation (uint8 stored in uint256 slot)
    {
        Bytes32 op_bytes{};
        op_bytes[31] = fields.operation;
        write32(op_bytes.data());
    }

    // safeTxGas = 0 (already zeroed)
    off += 32;

    // baseGas = 0 (already zeroed)
    off += 32;

    // gasPrice = 0 (already zeroed)
    off += 32;

    // gasToken = address(0) (already zeroed)
    off += 32;

    // refundReceiver = address(0) (already zeroed)
    off += 32;

    // nonce
    Bytes32 nonce_bytes{};
    uint64_to_uint256_be(fields.nonce, nonce_bytes.data());
    write32(nonce_bytes.data());

    return keccak256(buf, 352);
}

Bytes32 safe_tx_signing_hash(const Bytes32& domain_sep, const SafeTxFields& fields) {
    Bytes32 struct_hash = hash_safe_tx_struct(fields);
    return eip712_signing_hash(domain_sep, struct_hash);
}

std::string derive_safe_address(const uint8_t eoa[20]) {
    // CREATE2: address = keccak256(0xff + factory + salt + initCodeHash)[12:]
    // salt = keccak256(abi.encode(["address"], [eoa]))  (32-byte left-padded)
    // factory = 0xaacFeEa03eb1561C4e67d661e40682Bd20E3541b
    // initCodeHash = 0x2bce2127ff07fb632d16c8347c4ebf501f4841168bed00d9e6ef715ddb6fcecf

    static const uint8_t SAFE_FACTORY[] = {
        0xaa, 0xcf, 0xee, 0xa0, 0x3e, 0xb1, 0x56, 0x1c, 0x4e, 0x67,
        0xd6, 0x61, 0xe4, 0x06, 0x82, 0xbd, 0x20, 0xe3, 0x54, 0x1b
    };
    static const uint8_t SAFE_INIT_CODE_HASH[] = {
        0x2b, 0xce, 0x21, 0x27, 0xff, 0x07, 0xfb, 0x63, 0x2d, 0x16, 0xc8, 0x34, 0x7c, 0x4e, 0xbf, 0x50,
        0x1f, 0x48, 0x41, 0x16, 0x8b, 0xed, 0x00, 0xd9, 0xe6, 0xef, 0x71, 0x5d, 0xdb, 0x6f, 0xce, 0xcf
    };

    // salt = keccak256(abi.encode(address)) = keccak256(address_to_bytes32(eoa))
    Bytes32 eoa_padded = address_to_bytes32(eoa);
    Bytes32 salt = keccak256(eoa_padded);

    // CREATE2 input: 0xff(1) + factory(20) + salt(32) + initCodeHash(32) = 85 bytes
    uint8_t buf[85];
    buf[0] = 0xff;
    std::memcpy(buf + 1, SAFE_FACTORY, 20);
    std::memcpy(buf + 21, salt.data(), 32);
    std::memcpy(buf + 53, SAFE_INIT_CODE_HASH, 32);

    Bytes32 addr_hash = keccak256(buf, sizeof(buf));

    // Last 20 bytes = derived address
    return "0x" + hex_encode(addr_hash.data() + 12, 20);
}

Bytes32 eth_sign_hash(const Bytes32& hash) {
    // Ethereum personal sign prefix for 32-byte data:
    // "\x19Ethereum Signed Message:\n32" (28 bytes) + hash (32 bytes) = 60 bytes
    static constexpr uint8_t ETH_SIGN_PREFIX[] =
        "\x19" "Ethereum Signed Message:\n32";
    static constexpr size_t PREFIX_LEN = 28;

    uint8_t buf[PREFIX_LEN + 32];
    std::memcpy(buf, ETH_SIGN_PREFIX, PREFIX_LEN);
    std::memcpy(buf + PREFIX_LEN, hash.data(), 32);
    return keccak256(buf, sizeof(buf));
}

}  // namespace lt
