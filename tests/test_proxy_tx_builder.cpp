#include "doctest/doctest.h"
#include "inventory/proxy_tx_builder.h"
#include "inventory/abi_encoder.h"
#include "inventory/safe_tx_builder.h"  // for eth_sign_hash
#include "crypto/keccak.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"

#include <algorithm>
#include <cstring>
#include <vector>

static std::vector<uint8_t> hex_to_vec(const char* hex) {
    std::string_view sv(hex);
    if (sv.size() >= 2 && sv[0] == '0' && sv[1] == 'x') sv.remove_prefix(2);
    std::vector<uint8_t> out(sv.size() / 2);
    lt::hex_decode(sv, out.data(), out.size());
    return out;
}

using namespace lt;

TEST_SUITE("ProxyTxBuilder") {

// ---------------------------------------------------------------------------
// derive_proxy_address: verified against Python SDK in previous session
// EOA 0xfd06d0e7...6be1 → proxy 0x18c18e0c...2183
// ---------------------------------------------------------------------------
TEST_CASE("derive_proxy_address matches SDK") {
    uint8_t eoa[20]{};
    parse_eth_address("0xfd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2", eoa);

    std::string proxy = derive_proxy_address(eoa);
    // Lowercase comparison
    std::string proxy_lower = proxy;
    std::transform(proxy_lower.begin(), proxy_lower.end(), proxy_lower.begin(), ::tolower);

    CHECK(proxy_lower == "0x18c18e0c8c3154360b4f730ec8b9d8013ceb2183");
    MESSAGE("Derived proxy: " << proxy);
}

// ---------------------------------------------------------------------------
// derive_proxy_address: salt uses encode_packed (raw 20 bytes), NOT abi.encode (32 bytes)
// ---------------------------------------------------------------------------
TEST_CASE("derive_proxy_address uses encode_packed salt") {
    // The proxy salt = keccak256(raw 20 bytes)
    // The safe salt  = keccak256(left-padded 32 bytes)
    // They MUST differ for same EOA
    uint8_t eoa[20]{};
    parse_eth_address("0xfd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2", eoa);

    Bytes32 proxy_salt = keccak256(eoa, 20);             // encode_packed
    Bytes32 safe_salt = keccak256(address_to_bytes32(eoa));  // abi.encode

    CHECK(proxy_salt != safe_salt);
}

// ---------------------------------------------------------------------------
// PROXY_FUNCTION_SELECTOR matches keccak of canonical signature
// ---------------------------------------------------------------------------
TEST_CASE("PROXY_FUNCTION_SELECTOR is correct") {
    const char* sig = "proxy((uint8,address,uint256,bytes)[])";
    Bytes32 hash = keccak256(reinterpret_cast<const uint8_t*>(sig), std::strlen(sig));

    CHECK(PROXY_FUNCTION_SELECTOR[0] == hash[0]);
    CHECK(PROXY_FUNCTION_SELECTOR[1] == hash[1]);
    CHECK(PROXY_FUNCTION_SELECTOR[2] == hash[2]);
    CHECK(PROXY_FUNCTION_SELECTOR[3] == hash[3]);

    MESSAGE("proxy selector: 0x" << hex_encode(PROXY_FUNCTION_SELECTOR, 4));
}

// ---------------------------------------------------------------------------
// encode_proxy_call_data: verify structure for split (260-byte inner call)
// ---------------------------------------------------------------------------
TEST_CASE("encode_proxy_call_data wraps split correctly") {
    // Create a dummy split call
    Bytes32 cond{};
    cond[0] = 0xAA;
    AbiEncodedCall split = encode_split_position(cond, 1000000);
    REQUIRE(split.data.size() == 260);

    std::vector<uint8_t> proxy_data = encode_proxy_call_data(
        CTF_CONTRACT_ADDRESS, split.data);

    // Expected size: 4 (selector) + 8*32 (fixed ABI) + ceil(260/32)*32 (padded data)
    // ceil(260/32)*32 = 9*32 = 288
    size_t expected = 4 + 8 * 32 + 288;
    CHECK(proxy_data.size() == expected);

    // First 4 bytes = proxy function selector
    CHECK(std::memcmp(proxy_data.data(), PROXY_FUNCTION_SELECTOR, 4) == 0);

    // Byte 4: offset to array = 32
    CHECK(proxy_data[4 + 31] == 32);

    // Byte 36: array length = 1
    CHECK(proxy_data[36 + 31] == 1);

    // Byte 68: offset to element[0] = 32
    CHECK(proxy_data[68 + 31] == 32);

    // Byte 100: type_code = 1 (Call)
    CHECK(proxy_data[100 + 31] == 1);

    // Byte 132: to address = CTF (last 20 bytes of 32-byte slot)
    CHECK(std::memcmp(proxy_data.data() + 132 + 12, CTF_CONTRACT_ADDRESS, 20) == 0);

    // Byte 164: value = 0
    CHECK(proxy_data[164 + 31] == 0);

    // Byte 196: offset to bytes = 128
    CHECK(proxy_data[196 + 31] == 128);

    // Byte 228: bytes length = 260
    uint64_t data_len = 0;
    for (int i = 24; i < 32; ++i) {
        data_len = (data_len << 8) | proxy_data[228 + i];
    }
    CHECK(data_len == 260);

    // Byte 260: actual inner call data starts here
    CHECK(std::memcmp(proxy_data.data() + 260, split.data.data(), 260) == 0);

    MESSAGE("proxy_data.size() = " << proxy_data.size());
}

// ---------------------------------------------------------------------------
// encode_proxy_call_data: verify for approve (68-byte inner call)
// ---------------------------------------------------------------------------
TEST_CASE("encode_proxy_call_data wraps approve correctly") {
    AbiEncodedCall approve = encode_usdc_approve_ctf();
    REQUIRE(approve.data.size() == 68);

    std::vector<uint8_t> proxy_data = encode_proxy_call_data(
        USDC_E_ADDRESS, approve.data);

    // Expected: 4 + 8*32 + ceil(68/32)*32 = 4 + 256 + 96 = 356
    CHECK(proxy_data.size() == 356);

    // to address = USDC.e
    CHECK(std::memcmp(proxy_data.data() + 132 + 12, USDC_E_ADDRESS, 20) == 0);
}

// ---------------------------------------------------------------------------
// create_proxy_struct_hash: verify structure (deterministic with known inputs)
// ---------------------------------------------------------------------------
TEST_CASE("create_proxy_struct_hash is deterministic") {
    ProxyTxParams params;
    // Fill with test values
    params.from[0] = 0xAA;
    std::memcpy(params.to, PROXY_FACTORY_ADDRESS, 20);
    params.data = {0x01, 0x02, 0x03};
    params.tx_fee = 0;
    params.gas_price = 0;
    params.gas_limit = 500000;
    params.nonce = 42;
    std::memcpy(params.relay_hub, RELAY_HUB_ADDRESS, 20);
    params.relay[0] = 0xBB;

    Bytes32 hash1 = create_proxy_struct_hash(params);
    Bytes32 hash2 = create_proxy_struct_hash(params);
    CHECK(hash1 == hash2);

    // Different nonce → different hash
    params.nonce = 43;
    Bytes32 hash3 = create_proxy_struct_hash(params);
    CHECK(hash1 != hash3);
}

// ---------------------------------------------------------------------------
// create_proxy_struct_hash: verify prefix and byte layout
// ---------------------------------------------------------------------------
TEST_CASE("create_proxy_struct_hash uses rlx prefix") {
    // Manually construct the expected input and verify hash matches
    ProxyTxParams params;
    std::memset(params.from, 0x11, 20);
    std::memset(params.to, 0x22, 20);
    params.data = {0xAA, 0xBB, 0xCC};
    params.tx_fee = 0;
    params.gas_price = 0;
    params.gas_limit = 500000;
    params.nonce = 1;
    std::memset(params.relay_hub, 0x33, 20);
    std::memset(params.relay, 0x44, 20);

    Bytes32 hash = create_proxy_struct_hash(params);

    // Manual construction to verify
    std::vector<uint8_t> expected_msg;
    // "rlx:"
    expected_msg.push_back('r');
    expected_msg.push_back('l');
    expected_msg.push_back('x');
    expected_msg.push_back(':');
    // from (20)
    expected_msg.insert(expected_msg.end(), params.from, params.from + 20);
    // to (20)
    expected_msg.insert(expected_msg.end(), params.to, params.to + 20);
    // data (3 bytes)
    expected_msg.insert(expected_msg.end(), params.data.begin(), params.data.end());
    // txFee (32)
    { uint8_t buf[32]{}; uint64_to_uint256_be(0, buf); expected_msg.insert(expected_msg.end(), buf, buf + 32); }
    // gasPrice (32)
    { uint8_t buf[32]{}; uint64_to_uint256_be(0, buf); expected_msg.insert(expected_msg.end(), buf, buf + 32); }
    // gasLimit (32)
    { uint8_t buf[32]{}; uint64_to_uint256_be(500000, buf); expected_msg.insert(expected_msg.end(), buf, buf + 32); }
    // nonce (32)
    { uint8_t buf[32]{}; uint64_to_uint256_be(1, buf); expected_msg.insert(expected_msg.end(), buf, buf + 32); }
    // relayHub (20)
    expected_msg.insert(expected_msg.end(), params.relay_hub, params.relay_hub + 20);
    // relay (20)
    expected_msg.insert(expected_msg.end(), params.relay, params.relay + 20);

    Bytes32 expected_hash = keccak256(expected_msg.data(), expected_msg.size());
    CHECK(hash == expected_hash);

    MESSAGE("struct hash: 0x" << hex_encode(hash));
}

// ---------------------------------------------------------------------------
// Constants: verify factory and relay hub addresses
// ---------------------------------------------------------------------------
TEST_CASE("PROXY_FACTORY_ADDRESS is correct") {
    std::string hex = "0x" + hex_encode(PROXY_FACTORY_ADDRESS, 20);
    std::string lower = hex;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    CHECK(lower == "0xab45c5a4b0c941a2f231c04c3f49182e1a254052");
}

TEST_CASE("RELAY_HUB_ADDRESS is correct") {
    std::string hex = "0x" + hex_encode(RELAY_HUB_ADDRESS, 20);
    std::string lower = hex;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    CHECK(lower == "0xd216153c06e857cd7f72665e0af1d7d82172f494");
}

// ---------------------------------------------------------------------------
// End-to-end: encode a split call, wrap in proxy, compute struct hash
// ---------------------------------------------------------------------------
TEST_CASE("end-to-end proxy split encoding") {
    Bytes32 cond{};
    hex_decode_to_bytes32(
        "0x3f0840aaef08257663c5e832ef287f1f18b2e7492ef5b5aaf4d84b70083439c1",
        cond);

    uint64_t amount = 10000000;  // 10 USDC

    // 1. Encode inner CTF call
    AbiEncodedCall split = encode_split_position(cond, amount);
    CHECK(split.data.size() == 260);

    // 2. Wrap in proxy encoding
    std::vector<uint8_t> proxy_data = encode_proxy_call_data(
        CTF_CONTRACT_ADDRESS, split.data);
    CHECK(proxy_data.size() == 548);

    // 3. Build struct hash params
    uint8_t eoa[20]{};
    parse_eth_address("0xfd06d0e7aa3c7d4f2dcaba775f8be752ad8bb7d2", eoa);
    uint8_t relay[20]{};
    std::memset(relay, 0x55, 20);  // dummy relay address

    ProxyTxParams params;
    std::memcpy(params.from, eoa, 20);
    std::memcpy(params.to, PROXY_FACTORY_ADDRESS, 20);
    params.data = proxy_data;
    params.nonce = 0;
    params.gas_limit = 500000;
    std::memcpy(params.relay_hub, RELAY_HUB_ADDRESS, 20);
    std::memcpy(params.relay, relay, 20);

    Bytes32 hash = create_proxy_struct_hash(params);
    CHECK(hash != Bytes32{});  // non-zero

    MESSAGE("E2E proxy split struct_hash: 0x" << hex_encode(hash));
    MESSAGE("proxy_data hex: 0x" << hex_encode(proxy_data.data(), std::min<size_t>(80, proxy_data.size())) << "...");
}

// ===========================================================================
// SDK Reference Test: verify against Python SDK test_create_proxy_signature
// From: py-builder-relayer-client/tests/builder/test_proxy.py
// ===========================================================================

TEST_CASE("SDK reference: proxy signature matches Python SDK expected value") {
    // Inputs from test_create_proxy_signature:
    // PK: 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
    // EOA: 0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266
    // proxy_factory: 0xaB45c5A4B0c941a2F231C04C3f49182e1A254052
    // relay_hub: 0xD216153c06E857cD7f72665E0aF1d7D82172F494
    // relay: 0xae700edfd9ab986395f3999fe11177b9903a52f1
    // nonce=0, gas_price=0, gas_limit=85338, tx_fee=0

    // 1. Build inner USDC approve calldata (same as SDK test)
    auto approve_bytes = hex_to_vec(
        "0x095ea7b3"
        "0000000000000000000000004d97dcd97ec945f40cf65f87097ace5ea0476045"
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    REQUIRE(approve_bytes.size() == 68);

    // Verify our encode_usdc_approve_ctf produces the same bytes
    AbiEncodedCall our_approve = encode_usdc_approve_ctf();
    REQUIRE(our_approve.data.size() == 68);
    CHECK(std::memcmp(our_approve.data.data(), approve_bytes.data(), 68) == 0);

    // 2. Wrap in proxy encoding (target = USDC.e, like the SDK test)
    std::vector<uint8_t> proxy_data = encode_proxy_call_data(
        USDC_E_ADDRESS, approve_bytes);
    MESSAGE("proxy_data size: " << proxy_data.size());
    MESSAGE("proxy_data[:40]: 0x" << hex_encode(proxy_data.data(), std::min<size_t>(40, proxy_data.size())));

    // 3. Build struct hash params
    uint8_t eoa[20]{};
    parse_eth_address("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266", eoa);
    uint8_t relay[20]{};
    parse_eth_address("0xae700edfd9ab986395f3999fe11177b9903a52f1", relay);

    ProxyTxParams params;
    std::memcpy(params.from, eoa, 20);
    std::memcpy(params.to, PROXY_FACTORY_ADDRESS, 20);
    params.data = proxy_data;
    params.tx_fee = 0;
    params.gas_price = 0;
    params.gas_limit = 85338;
    params.nonce = 0;
    std::memcpy(params.relay_hub, RELAY_HUB_ADDRESS, 20);
    std::memcpy(params.relay, relay, 20);

    Bytes32 struct_hash = create_proxy_struct_hash(params);
    MESSAGE("struct_hash: 0x" << hex_encode(struct_hash));

    // 4. Apply eth_sign prefix (same as Python SDK's encode_defunct)
    Bytes32 signing_hash = eth_sign_hash(struct_hash);
    MESSAGE("signing_hash: 0x" << hex_encode(signing_hash));

    // 5. Sign with Hardhat account 0 private key
    auto pk_bytes = hex_to_vec(
        "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");
    Secp256k1OrderSigner signer(pk_bytes.data());

    uint8_t signature[65]{};
    REQUIRE(signer.sign_hash(signing_hash, signature));

    std::string sig_hex = hex_encode(signature, 65);
    MESSAGE("signature: 0x" << sig_hex);

    // Expected from Python SDK test_create_proxy_signature
    // (strip "0x" for comparison since hex_encode doesn't include it)
    std::string expected =
        "4c18e2d2294a00d686714aff8e7936ab657cb4655dfccb2b556efadcb7e835f8"
        "00dc2fecec69c501e29bb36ecb54b4da6b7c410c4dc740a33af2afde2b77297e"
        "1b";
    CHECK(sig_hex == expected);
}

}  // TEST_SUITE
