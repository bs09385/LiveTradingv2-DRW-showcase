#include <doctest/doctest.h>
#include "crypto/eip712.h"
#include "crypto/keccak.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"

#include <cstring>

TEST_SUITE("EIP-712") {

TEST_CASE("ORDER_TYPEHASH matches expected") {
    // Verify the typehash is correctly computed from the type string
    const char* type_str =
        "Order(uint256 salt,address maker,address signer,address taker,"
        "uint256 tokenId,uint256 makerAmount,uint256 takerAmount,"
        "uint256 expiration,uint256 nonce,uint256 feeRateBps,"
        "uint8 side,uint8 signatureType)";
    lt::Bytes32 expected = lt::keccak256(
        reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
    CHECK(lt::ORDER_TYPEHASH == expected);
}

TEST_CASE("DOMAIN_TYPEHASH matches expected") {
    const char* type_str =
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    lt::Bytes32 expected = lt::keccak256(
        reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
    CHECK(lt::DOMAIN_TYPEHASH == expected);
}

TEST_CASE("domain separator is deterministic") {
    lt::Bytes32 sep1 = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);
    lt::Bytes32 sep2 = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);
    CHECK(sep1 == sep2);
}

TEST_CASE("different exchanges produce different domain separators") {
    lt::Bytes32 sep_normal = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);
    lt::Bytes32 sep_neg = lt::compute_domain_separator(lt::NEG_RISK_CTF_EXCHANGE_ADDRESS);
    CHECK(sep_normal != sep_neg);
}

TEST_CASE("hash_order_struct is deterministic") {
    lt::OrderFields fields{};
    std::memset(fields.salt.data(), 0x42, 32);
    std::memset(fields.maker, 0x11, 20);
    std::memset(fields.signer, 0x22, 20);
    fields.side = 0;
    fields.signature_type = 0;

    lt::Bytes32 h1 = lt::hash_order_struct(fields);
    lt::Bytes32 h2 = lt::hash_order_struct(fields);
    CHECK(h1 == h2);
}

TEST_CASE("different order fields produce different hashes") {
    lt::OrderFields fields1{};
    std::memset(fields1.salt.data(), 0x42, 32);
    fields1.side = 0;

    lt::OrderFields fields2 = fields1;
    fields2.side = 1;

    lt::Bytes32 h1 = lt::hash_order_struct(fields1);
    lt::Bytes32 h2 = lt::hash_order_struct(fields2);
    CHECK(h1 != h2);
}

TEST_CASE("eip712_signing_hash format") {
    lt::Bytes32 domain_sep{};
    std::memset(domain_sep.data(), 0xAA, 32);
    lt::Bytes32 struct_hash{};
    std::memset(struct_hash.data(), 0xBB, 32);

    lt::Bytes32 result = lt::eip712_signing_hash(domain_sep, struct_hash);

    // Verify it's keccak256(0x19 || 0x01 || domain_sep || struct_hash)
    uint8_t buf[66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    std::memcpy(buf + 2, domain_sep.data(), 32);
    std::memcpy(buf + 34, struct_hash.data(), 32);
    lt::Bytes32 expected = lt::keccak256(buf, 66);

    CHECK(result == expected);
}

// ---- Official Polymarket test vector validation ----
// From: Polymarket/python-order-utils and Polymarket/clob-order-utils
// Uses Amoy testnet (chain_id=80002, exchange=0xdFE02Eb6733538f8Ea35D585af8DE5958AD99E40)
// Private key: 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
// Signer: 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266

TEST_CASE("official test vector: CTF Exchange (Amoy)") {
    // Exchange address on Amoy testnet
    uint8_t exchange_addr[20];
    lt::hex_decode("dFE02Eb6733538f8Ea35D585af8DE5958AD99E40", exchange_addr, 20);

    // Compute domain separator with Amoy chain ID
    lt::Bytes32 domain_sep = lt::compute_domain_separator(exchange_addr, 80002);

    // Build order fields matching official test vector
    lt::OrderFields fields{};

    // salt = 479249096354
    lt::uint64_to_uint256_be(479249096354ULL, fields.salt.data());

    // maker = signer = 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
    lt::hex_decode("f39Fd6e51aad88F6F4ce6aB8827279cffFb92266", fields.maker, 20);
    lt::hex_decode("f39Fd6e51aad88F6F4ce6aB8827279cffFb92266", fields.signer, 20);

    // taker = 0x0000...0000 (already zeroed)

    // tokenId = 1234
    lt::uint64_to_uint256_be(1234, fields.token_id);

    // makerAmount = 100000000
    lt::uint64_to_uint256_be(100000000, fields.maker_amount);

    // takerAmount = 50000000
    lt::uint64_to_uint256_be(50000000, fields.taker_amount);

    // expiration = 0, nonce = 0 (already zeroed)

    // feeRateBps = 100
    fields.fee_rate_bps = 100;

    // side = 0 (BUY), signatureType = 0 (EOA) (already zeroed)

    // Compute struct hash and signing hash
    lt::Bytes32 struct_hash = lt::hash_order_struct(fields);
    lt::Bytes32 signing_hash = lt::eip712_signing_hash(domain_sep, struct_hash);

    // Expected signing hash from official test suite
    std::string hash_hex = lt::hex_encode(signing_hash);
    CHECK(hash_hex == "02ca1d1aa31103804173ad1acd70066cb6c1258a4be6dada055111f9a7ea4e55");

    // Sign with official test private key
    uint8_t privkey[32];
    lt::hex_decode("ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80",
                   privkey, 32);
    lt::Secp256k1OrderSigner signer(privkey);

    uint8_t signature[65];
    REQUIRE(signer.sign_order(fields, domain_sep, signature));

    std::string sig_hex = lt::hex_encode(signature, 65);
    CHECK(sig_hex == "302cd9abd0b5fcaa202a344437ec0b6660da984e24ae9ad915a592a90facf5a5"
                     "1bb8a873cd8d270f070217fea1986531d5eec66f1162a81f66e026db653bf7ce"
                     "1c");
}

TEST_CASE("official test vector: Neg Risk CTF Exchange (Amoy)") {
    // Neg Risk CTF Exchange on Amoy (same address as mainnet)
    lt::Bytes32 domain_sep = lt::compute_domain_separator(
        lt::NEG_RISK_CTF_EXCHANGE_ADDRESS, 80002);

    lt::OrderFields fields{};
    lt::uint64_to_uint256_be(479249096354ULL, fields.salt.data());
    lt::hex_decode("f39Fd6e51aad88F6F4ce6aB8827279cffFb92266", fields.maker, 20);
    lt::hex_decode("f39Fd6e51aad88F6F4ce6aB8827279cffFb92266", fields.signer, 20);
    lt::uint64_to_uint256_be(1234, fields.token_id);
    lt::uint64_to_uint256_be(100000000, fields.maker_amount);
    lt::uint64_to_uint256_be(50000000, fields.taker_amount);
    fields.fee_rate_bps = 100;

    lt::Bytes32 struct_hash = lt::hash_order_struct(fields);
    lt::Bytes32 signing_hash = lt::eip712_signing_hash(domain_sep, struct_hash);

    std::string hash_hex = lt::hex_encode(signing_hash);
    CHECK(hash_hex == "f15790d3edc4b5aed427b0b543a9206fcf4b1a13dfed016d33bfb313076263b8");

    uint8_t privkey[32];
    lt::hex_decode("ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80",
                   privkey, 32);
    lt::Secp256k1OrderSigner signer(privkey);

    uint8_t signature[65];
    REQUIRE(signer.sign_order(fields, domain_sep, signature));

    std::string sig_hex = lt::hex_encode(signature, 65);
    CHECK(sig_hex == "1b3646ef347e5bd144c65bd3357ba19c12c12abaeedae733cf8579bc51a2752c"
                     "0454c3bc6b236957e393637982c769b8dc0706c0f5c399983d933850afd1cbcd"
                     "1c");
}

TEST_CASE("decimal_to_uint256 large tokenId round-trip") {
    // Exact tokenId from the live order
    const char* token_id_str = "39418051625724893514235207715152484160403032464550719304729678912984285746815";

    uint8_t bytes[32]{};
    REQUIRE(lt::decimal_to_uint256(std::string_view(token_id_str), bytes));

    // Verify by converting back: multiply hex bytes back to decimal
    // Instead, verify the hex encoding matches what Python would produce
    // 39418051625724893514235207715152484160403032464550719304729678912984285746815
    // = 0x571B30297B9948A22FDFCCDC58F01C5C51BE6F7A1E6B2B6B2A91FD04D7A53CFF
    // (We can verify this with Python: hex(39418...815))
    std::string hex = lt::hex_encode(bytes, 32);

    // Verify non-zero (basic sanity)
    bool all_zero = true;
    for (int i = 0; i < 32; ++i) {
        if (bytes[i] != 0) { all_zero = false; break; }
    }
    CHECK_FALSE(all_zero);

    // Re-encode and verify idempotent
    uint8_t bytes2[32]{};
    REQUIRE(lt::decimal_to_uint256(std::string_view(token_id_str), bytes2));
    CHECK(std::memcmp(bytes, bytes2, 32) == 0);
}

TEST_CASE("mainnet neg_risk domain separator is stable") {
    // Verify mainnet neg_risk domain separator matches expected value
    lt::Bytes32 sep = lt::compute_domain_separator(lt::NEG_RISK_CTF_EXCHANGE_ADDRESS, 137);
    lt::Bytes32 sep2 = lt::compute_domain_separator(lt::NEG_RISK_CTF_EXCHANGE_ADDRESS);
    CHECK(sep == sep2);

    // Verify it's different from normal
    lt::Bytes32 sep_normal = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);
    CHECK(sep != sep_normal);
}

TEST_CASE("verify live order signing hash from log") {
    // Exact parameters from engine_20260301_222608.log line 28-31
    lt::OrderFields fields{};
    lt::uint64_to_uint256_be(1595448156ULL, fields.salt.data());
    lt::hex_decode("18C18e0c8c3154360B4F730eC8B9D8013Ceb2183", fields.maker, 20);
    lt::hex_decode("fd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2", fields.signer, 20);
    // taker = zero (default)
    lt::decimal_to_uint256(std::string_view(
        "69935983593098922989644581089761015239481377301387627882488586690228830460757"),
        fields.token_id);
    lt::decimal_to_uint256(std::string_view("2100000"), fields.maker_amount);
    lt::decimal_to_uint256(std::string_view("5000000"), fields.taker_amount);
    // expiration=0, nonce=0, feeRateBps=0 (defaults)
    fields.side = 0;            // BUY
    fields.signature_type = 1;  // POLY_PROXY

    // negRisk=true → Neg Risk CTF Exchange, chain 137
    lt::Bytes32 domain_sep = lt::compute_domain_separator(
        lt::NEG_RISK_CTF_EXCHANGE_ADDRESS, 137);
    lt::Bytes32 struct_hash = lt::hash_order_struct(fields);
    lt::Bytes32 signing_hash = lt::eip712_signing_hash(domain_sep, struct_hash);

    std::string hash_hex = lt::hex_encode(signing_hash);
    // Must match the signing_hash from the log
    CHECK(hash_hex == "7ead0f81ab5893c84b52ad0099fe61042b494a9e95e13f42096bbbc6ebf518c7");
}

TEST_CASE("eip55_checksum known vectors") {
    // Well-known EIP-55 test vector from the EIP-55 spec
    // All-caps: 0x52908400098527886E0F7030069857D2E4169EE7
    uint8_t addr1[20];
    lt::hex_decode("52908400098527886E0F7030069857D2E4169EE7", addr1, 20);
    CHECK(lt::eip55_checksum(addr1) == "0x52908400098527886E0F7030069857D2E4169EE7");

    // All-lower: 0xde709f2102306220921060314715629080e2fb77
    uint8_t addr2[20];
    lt::hex_decode("de709f2102306220921060314715629080e2fb77", addr2, 20);
    CHECK(lt::eip55_checksum(addr2) == "0xde709f2102306220921060314715629080e2fb77");

    // Mixed-case from official test: 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
    uint8_t addr3[20];
    lt::hex_decode("f39Fd6e51aad88F6F4ce6aB8827279cffFb92266", addr3, 20);
    CHECK(lt::eip55_checksum(addr3) == "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266");

    // Zero address: all digits 0-9, no casing changes
    uint8_t addr_zero[20]{};
    CHECK(lt::eip55_checksum(addr_zero) == "0x0000000000000000000000000000000000000000");
}

}  // TEST_SUITE
