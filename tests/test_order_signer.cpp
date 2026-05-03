#include <doctest/doctest.h>
#include "crypto/order_signer.h"
#include "crypto/hex_utils.h"
#include "crypto/keccak.h"
#include "crypto/eip712.h"

#include <cstring>

TEST_SUITE("OrderSigner") {

// Well-known test private key (DO NOT use for real funds!)
// Key: 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
// This is Hardhat/Foundry account #0
static const char* TEST_PRIVATE_KEY_HEX =
    "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";

// Expected address for this key: 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
static const char* EXPECTED_ADDRESS_HEX = "f39fd6e51aad88f6f4ce6ab8827279cfffb92266";

TEST_CASE("derive address from known private key") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);

    lt::Secp256k1OrderSigner signer(pk.data());
    uint8_t address[20];
    REQUIRE(signer.get_signer_address(address));

    std::string addr_hex = lt::hex_encode(address, 20);
    CHECK(addr_hex == EXPECTED_ADDRESS_HEX);
}

TEST_CASE("sign produces 65-byte signature") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);

    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderFields fields{};
    std::memset(fields.salt.data(), 0x42, 32);

    uint8_t address[20];
    signer.get_signer_address(address);
    std::memcpy(fields.maker, address, 20);
    std::memcpy(fields.signer, address, 20);

    lt::Bytes32 domain_sep = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);

    uint8_t signature[65];
    REQUIRE(signer.sign_order(fields, domain_sep, signature));

    // v should be 27 or 28
    CHECK((signature[64] == 27 || signature[64] == 28));
}

TEST_CASE("signing is deterministic for same inputs") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);

    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderFields fields{};
    // Use fixed salt for determinism
    std::memset(fields.salt.data(), 0x01, 32);

    uint8_t address[20];
    signer.get_signer_address(address);
    std::memcpy(fields.maker, address, 20);
    std::memcpy(fields.signer, address, 20);

    lt::Bytes32 domain_sep = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);

    uint8_t sig1[65], sig2[65];
    REQUIRE(signer.sign_order(fields, domain_sep, sig1));
    REQUIRE(signer.sign_order(fields, domain_sep, sig2));

    CHECK(std::memcmp(sig1, sig2, 65) == 0);
}

TEST_CASE("different fields produce different signatures") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);

    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderFields fields1{};
    std::memset(fields1.salt.data(), 0x01, 32);

    lt::OrderFields fields2{};
    std::memset(fields2.salt.data(), 0x02, 32);

    uint8_t address[20];
    signer.get_signer_address(address);
    std::memcpy(fields1.maker, address, 20);
    std::memcpy(fields1.signer, address, 20);
    std::memcpy(fields2.maker, address, 20);
    std::memcpy(fields2.signer, address, 20);

    lt::Bytes32 domain_sep = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);

    uint8_t sig1[65], sig2[65];
    REQUIRE(signer.sign_order(fields1, domain_sep, sig1));
    REQUIRE(signer.sign_order(fields2, domain_sep, sig2));

    // r and s should differ (overwhelmingly likely with different hash inputs)
    CHECK(std::memcmp(sig1, sig2, 64) != 0);
}

}  // TEST_SUITE
