#include <doctest/doctest.h>
#include "inventory/safe_tx_builder.h"
#include "inventory/abi_encoder.h"
#include "crypto/keccak.h"
#include "crypto/eip712.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"

#include <cstring>
#include <vector>

static std::vector<uint8_t> hex_to_bytes(const char* hex) {
    std::string_view sv(hex);
    if (sv.size() >= 2 && sv[0] == '0' && sv[1] == 'x') sv.remove_prefix(2);
    std::vector<uint8_t> out(sv.size() / 2);
    lt::hex_decode(sv, out.data(), out.size());
    return out;
}

TEST_SUITE("SafeTxBuilder") {

TEST_CASE("SAFE_DOMAIN_TYPEHASH matches keccak256 of type string") {
    const char* type_str = "EIP712Domain(uint256 chainId,address verifyingContract)";
    lt::Bytes32 expected = lt::keccak256(
        reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
    CHECK(lt::SAFE_DOMAIN_TYPEHASH == expected);
}

TEST_CASE("SAFE_TX_TYPEHASH matches keccak256 of type string") {
    const char* type_str =
        "SafeTx(address to,uint256 value,bytes data,uint8 operation,"
        "uint256 safeTxGas,uint256 baseGas,uint256 gasPrice,"
        "address gasToken,address refundReceiver,uint256 nonce)";
    lt::Bytes32 expected = lt::keccak256(
        reinterpret_cast<const uint8_t*>(type_str), std::strlen(type_str));
    CHECK(lt::SAFE_TX_TYPEHASH == expected);
}

TEST_CASE("domain separator is deterministic") {
    uint8_t safe_addr[20]{};
    std::memset(safe_addr, 0x42, 20);
    auto sep1 = lt::compute_safe_domain_separator(safe_addr);
    auto sep2 = lt::compute_safe_domain_separator(safe_addr);
    CHECK(sep1 == sep2);
}

TEST_CASE("different safe addresses produce different domain separators") {
    uint8_t addr1[20]{};
    uint8_t addr2[20]{};
    std::memset(addr1, 0x11, 20);
    std::memset(addr2, 0x22, 20);
    auto sep1 = lt::compute_safe_domain_separator(addr1);
    auto sep2 = lt::compute_safe_domain_separator(addr2);
    CHECK(sep1 != sep2);
}

TEST_CASE("different chain IDs produce different domain separators") {
    uint8_t addr[20]{};
    std::memset(addr, 0x42, 20);
    auto sep137 = lt::compute_safe_domain_separator(addr, 137);
    auto sep1 = lt::compute_safe_domain_separator(addr, 1);
    CHECK(sep137 != sep1);
}

TEST_CASE("Safe domain separator differs from CTF Exchange domain separator") {
    // Safe uses a simpler domain (chainId + verifyingContract only)
    // while CTF Exchange uses (name, version, chainId, verifyingContract)
    uint8_t addr[20]{};
    std::memcpy(addr, lt::CTF_EXCHANGE_ADDRESS, 20);
    auto safe_sep = lt::compute_safe_domain_separator(addr);
    auto ctf_sep = lt::compute_domain_separator(lt::CTF_EXCHANGE_ADDRESS);
    CHECK(safe_sep != ctf_sep);
}

TEST_CASE("hash_safe_tx_struct is deterministic") {
    lt::SafeTxFields fields;
    std::memcpy(fields.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields.data = {0x01, 0x02, 0x03};
    fields.nonce = 42;

    auto hash1 = lt::hash_safe_tx_struct(fields);
    auto hash2 = lt::hash_safe_tx_struct(fields);
    CHECK(hash1 == hash2);
}

TEST_CASE("different nonces produce different struct hashes") {
    lt::SafeTxFields fields1;
    std::memcpy(fields1.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields1.data = {0x01};
    fields1.nonce = 1;

    lt::SafeTxFields fields2 = fields1;
    fields2.nonce = 2;

    CHECK(lt::hash_safe_tx_struct(fields1) != lt::hash_safe_tx_struct(fields2));
}

TEST_CASE("different data produces different struct hashes") {
    lt::SafeTxFields fields1;
    std::memcpy(fields1.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields1.data = {0x01, 0x02};
    fields1.nonce = 0;

    lt::SafeTxFields fields2 = fields1;
    fields2.data = {0x03, 0x04};

    CHECK(lt::hash_safe_tx_struct(fields1) != lt::hash_safe_tx_struct(fields2));
}

TEST_CASE("different to addresses produce different struct hashes") {
    lt::SafeTxFields fields1;
    std::memset(fields1.to, 0x11, 20);
    fields1.data = {0x01};
    fields1.nonce = 0;

    lt::SafeTxFields fields2 = fields1;
    std::memset(fields2.to, 0x22, 20);

    CHECK(lt::hash_safe_tx_struct(fields1) != lt::hash_safe_tx_struct(fields2));
}

TEST_CASE("safe_tx_signing_hash uses EIP-712 prefix") {
    uint8_t safe_addr[20]{};
    std::memset(safe_addr, 0x42, 20);
    auto domain_sep = lt::compute_safe_domain_separator(safe_addr);

    lt::SafeTxFields fields;
    std::memcpy(fields.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields.data = {0xDE, 0xAD};
    fields.nonce = 1;

    auto signing_hash = lt::safe_tx_signing_hash(domain_sep, fields);

    // Manually compute: keccak256(0x19 || 0x01 || domain_sep || struct_hash)
    auto struct_hash = lt::hash_safe_tx_struct(fields);
    auto expected = lt::eip712_signing_hash(domain_sep, struct_hash);
    CHECK(signing_hash == expected);
}

TEST_CASE("end-to-end: split position signing hash") {
    // Build a realistic split call and compute its signing hash
    lt::Bytes32 condition_id{};
    std::memset(condition_id.data(), 0xAB, 32);
    auto call = lt::encode_split_position(condition_id, 5000000);

    uint8_t safe_addr[20]{};
    lt::parse_eth_address("0x18C18e0c8c3154360B4F730eC8B9D8013Ceb2183", safe_addr);
    auto domain_sep = lt::compute_safe_domain_separator(safe_addr);

    lt::SafeTxFields fields;
    std::memcpy(fields.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields.data = call.data;
    fields.nonce = 0;

    auto signing_hash = lt::safe_tx_signing_hash(domain_sep, fields);
    CHECK_FALSE(signing_hash == lt::Bytes32{});  // Should not be zero
}

TEST_CASE("eth_sign_hash applies personal message prefix") {
    // eth_sign_hash = keccak256("\x19Ethereum Signed Message:\n32" + hash)
    lt::Bytes32 test_hash{};
    std::memset(test_hash.data(), 0xAA, 32);

    auto result = lt::eth_sign_hash(test_hash);

    // Must not be zero
    CHECK_FALSE(result == lt::Bytes32{});

    // Must differ from the raw hash
    CHECK(result != test_hash);

    // Must be deterministic
    CHECK(result == lt::eth_sign_hash(test_hash));

    // Different inputs -> different outputs
    lt::Bytes32 test_hash2{};
    std::memset(test_hash2.data(), 0xBB, 32);
    CHECK(lt::eth_sign_hash(test_hash2) != result);

    // Manually verify: prefix is exactly 28 bytes + 32 bytes = 60 bytes
    uint8_t buf[60];
    const char* prefix = "\x19" "Ethereum Signed Message:\n32";
    std::memcpy(buf, prefix, 28);
    std::memcpy(buf + 28, test_hash.data(), 32);
    auto expected = lt::keccak256(buf, 60);
    CHECK(result == expected);
}

TEST_CASE("struct hash includes keccak256 of data (not raw data)") {
    // Per EIP-712: for bytes type, encode as keccak256(value)
    lt::SafeTxFields fields;
    std::memcpy(fields.to, lt::CTF_CONTRACT_ADDRESS, 20);
    fields.data = {0x01, 0x02, 0x03, 0x04, 0x05};
    fields.nonce = 0;

    // The struct hash should be deterministic regardless of data length
    auto hash = lt::hash_safe_tx_struct(fields);
    CHECK_FALSE(hash == lt::Bytes32{});

    // Verify data hash is embedded (indirectly: changing 1 byte should change hash)
    lt::SafeTxFields fields2 = fields;
    fields2.data[0] = 0xFF;
    CHECK(lt::hash_safe_tx_struct(fields2) != hash);
}

// ===================================================================
// SDK Reference Test: verify against Python SDK's known test vectors
// From: py-builder-relayer-client/tests/builder/test_safe.py
// ===================================================================

TEST_CASE("SDK reference: struct hash matches Python SDK expected value") {
    // Inputs from test_create_struct_hash in the Python SDK:
    // Chain ID: 137, Safe: 0xd93B25cb943D14d0d34FBaF01Fc93a0f8b5F6E47
    // To: 0xA238CBeb142c10Ef7Ad8442C6D1f9E89e07e7761 (MultiSend)
    // Operation: 1 (DelegateCall), Nonce: 8
    // Data: MultiSend encoded approve calls (277 bytes)

    uint8_t safe_addr[20]{};
    lt::parse_eth_address("0xd93B25cb943D14d0d34FBaF01Fc93a0f8b5F6E47", safe_addr);
    auto domain_sep = lt::compute_safe_domain_separator(safe_addr, 137);

    uint8_t to_addr[20]{};
    lt::parse_eth_address("0xA238CBeb142c10Ef7Ad8442C6D1f9E89e07e7761", to_addr);

    auto data_bytes = hex_to_bytes(
        "0x8d80ff0a"
        "0000000000000000000000000000000000000000000000000000000000000020"
        "0000000000000000000000000000000000000000000000000000000000000132"
        "002791bca1f2de4661ed88a30c99a7a9449aa84174"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000044"
        "095ea7b3"
        "0000000000000000000000004d97dcd97ec945f40cf65f87097ace5ea0476045"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        "002791bca1f2de4661ed88a30c99a7a9449aa84174"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000044"
        "095ea7b3"
        "0000000000000000000000004d97dcd97ec945f40cf65f87097ace5ea0476045"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"
        "0000000000000000000000000000"
    );

    lt::SafeTxFields fields;
    std::memcpy(fields.to, to_addr, 20);
    fields.data = data_bytes;
    fields.nonce = 8;
    fields.operation = 1;  // DelegateCall

    // Expected EIP-712 signing hash from the Python SDK
    // (SDK's "struct hash" is actually keccak256(\x19\x01 + domainSep + structHash))
    auto signing_hash = lt::safe_tx_signing_hash(domain_sep, fields);
    std::string signing_hash_hex = "0x" + lt::hex_encode(signing_hash);

    INFO("signing_hash = " << signing_hash_hex);
    CHECK(signing_hash_hex == "0x06d5102c3e356b62a75f8203cd5ce7ab1fa8fdab33875ef621eee102220d90b8");
}

TEST_CASE("SDK reference: eth_sign signature matches Python SDK") {
    // The Python SDK's sign_eip712_struct_hash applies encode_defunct()
    // (personal sign prefix) before signing. We verify the full chain:
    // 1. Take the known EIP-712 hash
    // 2. Apply eth_sign prefix
    // 3. Sign with the test private key
    // 4. Compare against expected signature

    // Known EIP-712 signing hash from test above
    auto eip712_hash_bytes = hex_to_bytes(
        "0x06d5102c3e356b62a75f8203cd5ce7ab1fa8fdab33875ef621eee102220d90b8");
    lt::Bytes32 eip712_hash{};
    std::memcpy(eip712_hash.data(), eip712_hash_bytes.data(), 32);

    // Apply eth_sign prefix (what encode_defunct does)
    lt::Bytes32 prefixed_hash = lt::eth_sign_hash(eip712_hash);

    // Sign with Hardhat account 0 private key
    auto pk_bytes = hex_to_bytes(
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");
    lt::Secp256k1OrderSigner signer(pk_bytes.data());

    uint8_t signature[65]{};
    REQUIRE(signer.sign_hash(prefixed_hash, signature));

    std::string sig_hex = "0x" + lt::hex_encode(signature, 65);
    INFO("signature = " << sig_hex);

    // Expected from Python SDK test_create_safe_signature
    CHECK(sig_hex == "0xad62657208a0d885f91bba7490de238741bf7c51eb792f00856171aafc9e0123"
                     "73156fb672e55d840733c8bf723ec458545fcd5749aa5e547f808c222e7e1170"
                     "1c");
}

TEST_CASE("SDK reference: packed signature v-adjustment") {
    // Verify v-value adjustment: v=28 (0x1c) -> v=32 (0x20) for Safe eth_sign
    auto raw_sig = hex_to_bytes(
        "0xad62657208a0d885f91bba7490de238741bf7c51eb792f00856171aafc9e0123"
        "73156fb672e55d840733c8bf723ec458545fcd5749aa5e547f808c222e7e1170"
        "1c");

    uint8_t adjusted[65];
    std::memcpy(adjusted, raw_sig.data(), 65);

    // Apply Safe v-adjustment (same logic as build_submit_body)
    uint8_t v = adjusted[64];
    if (v == 27 || v == 28) {
        adjusted[64] = v + 4;
    } else if (v == 0 || v == 1) {
        adjusted[64] = v + 31;
    }

    std::string packed_hex = "0x" + lt::hex_encode(adjusted, 65);
    INFO("packed_sig = " << packed_hex);

    CHECK(packed_hex == "0xad62657208a0d885f91bba7490de238741bf7c51eb792f00856171aafc9e0123"
                        "73156fb672e55d840733c8bf723ec458545fcd5749aa5e547f808c222e7e1170"
                        "20");
}

TEST_CASE("SDK reference: Safe address derivation") {
    // From test_derive_safe: EOA 0x6e0c80c...4B5b5 should derive Safe 0x6d8c4e...B4fa
    // CREATE2: keccak256(0xff + factory + keccak256(abi.encode(eoa)) + initCodeHash)

    auto eoa_bytes_vec = hex_to_bytes("0x6e0c80c90ea6c15917308F820Eac91Ce2724B5b5");
    uint8_t eoa_bytes[20]{};
    std::memcpy(eoa_bytes, eoa_bytes_vec.data(), 20);

    // Factory: 0xaacFeEa03eb1561C4e67d661e40682Bd20E3541b
    auto factory_vec = hex_to_bytes("0xaacFeEa03eb1561C4e67d661e40682Bd20E3541b");
    uint8_t factory[20]{};
    std::memcpy(factory, factory_vec.data(), 20);

    // SAFE_INIT_CODE_HASH
    auto init_code_hash_vec = hex_to_bytes(
        "0x2bce2127ff07fb632d16c8347c4ebf501f4841168bed00d9e6ef715ddb6fcecf");

    // salt = keccak256(abi.encode(["address"], [eoa]))
    // abi.encode address = left-padded to 32 bytes
    lt::Bytes32 eoa_padded = lt::address_to_bytes32(eoa_bytes);
    lt::Bytes32 salt = lt::keccak256(eoa_padded);

    // CREATE2: keccak256(0xff + factory(20) + salt(32) + initCodeHash(32))
    uint8_t create2_input[1 + 20 + 32 + 32];
    create2_input[0] = 0xff;
    std::memcpy(create2_input + 1, factory, 20);
    std::memcpy(create2_input + 21, salt.data(), 32);
    std::memcpy(create2_input + 53, init_code_hash_vec.data(), 32);

    lt::Bytes32 addr_hash = lt::keccak256(create2_input, sizeof(create2_input));

    // Take last 20 bytes as address
    uint8_t derived[20]{};
    std::memcpy(derived, addr_hash.data() + 12, 20);

    std::string derived_hex = "0x" + lt::hex_encode(derived, 20);
    INFO("derived safe = " << derived_hex);

    // Expected: 0x6d8c4e9aDF5748Af82Dabe2C6225207770d6B4fa (case-insensitive)
    std::string expected = "0x6d8c4e9adf5748af82dabe2c6225207770d6b4fa";
    // Lowercase compare
    std::string derived_lower = derived_hex;
    for (auto& c : derived_lower) c = std::tolower(c);
    CHECK(derived_lower == expected);
}

TEST_CASE("Check if user wallet is PROXY type") {
    // User EOA: 0xfd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2
    // User configured wallet: 0x18C18e0c8c3154360B4F730eC8B9D8013Ceb2183
    // PROXY factory: 0xaB45c5A4B0c941a2F231C04C3f49182e1A254052
    // PROXY_INIT_CODE_HASH: 0xd21df8dc65880a8606f09fe0ce3df9b8869287ab0b058be05aa9e8af6330a00b
    // Proxy derivation uses encode_packed (raw 20 bytes) NOT abi.encode (32 bytes)

    auto eoa_vec = hex_to_bytes("0xfd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2");
    auto factory_vec = hex_to_bytes("0xaB45c5A4B0c941a2F231C04C3f49182e1A254052");
    auto init_hash_vec = hex_to_bytes("0xd21df8dc65880a8606f09fe0ce3df9b8869287ab0b058be05aa9e8af6330a00b");

    // salt = keccak256(encode_packed(["address"], [eoa])) = keccak256(eoa_20_bytes)
    lt::Bytes32 salt = lt::keccak256(eoa_vec.data(), 20);

    // CREATE2: keccak256(0xff + factory + salt + initCodeHash)
    uint8_t buf[85];
    buf[0] = 0xff;
    std::memcpy(buf + 1, factory_vec.data(), 20);
    std::memcpy(buf + 21, salt.data(), 32);
    std::memcpy(buf + 53, init_hash_vec.data(), 32);

    lt::Bytes32 addr_hash = lt::keccak256(buf, sizeof(buf));
    uint8_t derived[20]{};
    std::memcpy(derived, addr_hash.data() + 12, 20);

    std::string derived_hex = lt::hex_encode(derived, 20);
    INFO("Derived PROXY from EOA = 0x" << derived_hex);
    INFO("User configured wallet = 18c18e0c8c3154360b4f730ec8b9d8013ceb2183");

    std::string expected = "18c18e0c8c3154360b4f730ec8b9d8013ceb2183";
    CHECK(derived_hex == expected);
}

TEST_CASE("derive_safe_address matches CREATE2 reference") {
    // Verify our derive_safe_address function matches the SDK test vector
    auto eoa_vec = hex_to_bytes("0x6e0c80c90ea6c15917308F820Eac91Ce2724B5b5");
    uint8_t eoa[20]{};
    std::memcpy(eoa, eoa_vec.data(), 20);

    std::string derived = lt::derive_safe_address(eoa);
    INFO("derived = " << derived);

    // Expected from Python SDK test_derive_safe
    // Lowercase compare
    std::string lower = derived;
    for (auto& c : lower) c = std::tolower(c);
    CHECK(lower == "0x6d8c4e9adf5748af82dabe2c6225207770d6b4fa");
}

}  // TEST_SUITE
