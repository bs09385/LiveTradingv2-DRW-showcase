#include "crypto/eip712.h"
#include "crypto/keccak.h"

#include <cstring>

namespace lt {

// Pre-compute typehashes at startup
// ORDER_TYPEHASH = keccak256("Order(uint256 salt,address maker,address signer,address taker,uint256 tokenId,uint256 makerAmount,uint256 takerAmount,uint256 expiration,uint256 nonce,uint256 feeRateBps,uint8 side,uint8 signatureType)")
static Bytes32 compute_order_typehash() {
    const char* type_str =
        "Order(uint256 salt,address maker,address signer,address taker,"
        "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
        "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
        "uint8 side,uint8 signatureType)";
    return keccak256(reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
}

// DOMAIN_TYPEHASH = keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
static Bytes32 compute_domain_typehash() {
    const char* type_str =
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    return keccak256(reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
}

const Bytes32 ORDER_TYPEHASH = compute_order_typehash();
const Bytes32 DOMAIN_TYPEHASH = compute_domain_typehash();

// CTF Exchange: 0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E
const uint8_t CTF_EXCHANGE_ADDRESS[20] = {
    0x4b, 0xFb, 0x41, 0xd5, 0xB3, 0x57, 0x0D, 0xeF, 0xd0, 0x3C,
    0x39, 0xa9, 0xA4, 0xD8, 0xdE, 0x6B, 0xd8, 0xB8, 0x98, 0x2E
};

// Neg Risk CTF Exchange: 0xC5d563A36AE78145C45a50134d48A1215220f80a
const uint8_t NEG_RISK_CTF_EXCHANGE_ADDRESS[20] = {
    0xC5, 0xd5, 0x63, 0xA3, 0x6A, 0xE7, 0x81, 0x45, 0xC4, 0x5a,
    0x50, 0x13, 0x4d, 0x48, 0xA1, 0x21, 0x52, 0x20, 0xf8, 0x0a
};

Bytes32 compute_domain_separator(const uint8_t exchange_address[20]) {
    return compute_domain_separator(exchange_address, POLYGON_CHAIN_ID);
}

Bytes32 compute_domain_separator(const uint8_t exchange_address[20], uint64_t chain_id) {
    // domainSeparator = keccak256(
    //   abi.encode(
    //     DOMAIN_TYPEHASH,
    //     keccak256("Polymarket CTF Exchange"),
    //     keccak256("1"),
    //     chainId (137),
    //     verifyingContract
    //   )
    // )
    const char* name = "Polymarket CTF Exchange";
    Bytes32 name_hash = keccak256(reinterpret_cast<const uint8_t*>(name), std::strlen(name));

    const char* version = "1";
    Bytes32 version_hash = keccak256(reinterpret_cast<const uint8_t*>(version), std::strlen(version));

    // chain_id as bytes32 (big-endian)
    Bytes32 chain_id_bytes{};
    uint64_to_uint256_be(chain_id, chain_id_bytes.data());

    // address as bytes32 (left-padded)
    Bytes32 addr_bytes = address_to_bytes32(exchange_address);

    // Concatenate: DOMAIN_TYPEHASH || nameHash || versionHash || chainId || address
    // = 5 * 32 = 160 bytes
    uint8_t buf[160];
    std::memcpy(buf + 0,   DOMAIN_TYPEHASH.data(), 32);
    std::memcpy(buf + 32,  name_hash.data(), 32);
    std::memcpy(buf + 64,  version_hash.data(), 32);
    std::memcpy(buf + 96,  chain_id_bytes.data(), 32);
    std::memcpy(buf + 128, addr_bytes.data(), 32);

    return keccak256(buf, 160);
}

Bytes32 hash_order_struct(const OrderFields& fields) {
    // structHash = keccak256(abi.encode(
    //   ORDER_TYPEHASH,
    //   salt, maker, signer, taker, tokenId, makerAmount, takerAmount,
    //   expiration, nonce, feeRateBps, side, signatureType
    // ))
    // = 13 * 32 = 416 bytes
    uint8_t buf[416];
    size_t off = 0;

    auto write32 = [&](const uint8_t* data) {
        std::memcpy(buf + off, data, 32);
        off += 32;
    };

    auto write_uint64 = [&](uint64_t val) {
        Bytes32 b{};
        uint64_to_uint256_be(val, b.data());
        write32(b.data());
    };

    auto write_address = [&](const uint8_t addr[20]) {
        Bytes32 b = address_to_bytes32(addr);
        write32(b.data());
    };

    auto write_uint8 = [&](uint8_t val) {
        Bytes32 b{};
        b[31] = val;
        write32(b.data());
    };

    // ORDER_TYPEHASH
    write32(ORDER_TYPEHASH.data());

    // salt (already bytes32)
    write32(fields.salt.data());

    // maker address
    write_address(fields.maker);

    // signer address
    write_address(fields.signer);

    // taker address
    write_address(fields.taker);

    // tokenId (already uint256)
    write32(fields.token_id);

    // makerAmount (already uint256)
    write32(fields.maker_amount);

    // takerAmount (already uint256)
    write32(fields.taker_amount);

    // expiration
    write_uint64(fields.expiration);

    // nonce
    write_uint64(fields.nonce);

    // feeRateBps
    write_uint64(fields.fee_rate_bps);

    // side (uint8)
    write_uint8(fields.side);

    // signatureType (uint8)
    write_uint8(fields.signature_type);

    return keccak256(buf, 416);
}

Bytes32 eip712_signing_hash(const Bytes32& domain_sep, const Bytes32& struct_hash) {
    // keccak256(0x19 || 0x01 || domainSeparator || structHash)
    // = 2 + 32 + 32 = 66 bytes
    uint8_t buf[66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    std::memcpy(buf + 2,  domain_sep.data(), 32);
    std::memcpy(buf + 34, struct_hash.data(), 32);

    return keccak256(buf, 66);
}

}  // namespace lt
