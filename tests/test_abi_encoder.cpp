#include <doctest/doctest.h>
#include "inventory/abi_encoder.h"
#include "crypto/keccak.h"
#include "crypto/hex_utils.h"

#include <cstring>

TEST_SUITE("AbiEncoder") {

TEST_CASE("split selector matches keccak256 of canonical signature") {
    const char* sig = "splitPosition(address,bytes32,bytes32,uint256[],uint256)";
    lt::Bytes32 hash = lt::keccak256(
        reinterpret_cast<const uint8_t*>(sig), std::strlen(sig));
    CHECK(std::memcmp(lt::SPLIT_POSITION_SELECTOR, hash.data(), 4) == 0);
}

TEST_CASE("merge selector matches keccak256 of canonical signature") {
    const char* sig = "mergePositions(address,bytes32,bytes32,uint256[],uint256)";
    lt::Bytes32 hash = lt::keccak256(
        reinterpret_cast<const uint8_t*>(sig), std::strlen(sig));
    CHECK(std::memcmp(lt::MERGE_POSITIONS_SELECTOR, hash.data(), 4) == 0);
}

TEST_CASE("redeem selector matches keccak256 of canonical signature") {
    const char* sig = "redeemPositions(address,bytes32,bytes32,uint256[])";
    lt::Bytes32 hash = lt::keccak256(
        reinterpret_cast<const uint8_t*>(sig), std::strlen(sig));
    CHECK(std::memcmp(lt::REDEEM_POSITIONS_SELECTOR, hash.data(), 4) == 0);
}

TEST_CASE("encode_split_position produces 260 bytes") {
    lt::Bytes32 cond{};
    std::memset(cond.data(), 0xAB, 32);
    auto call = lt::encode_split_position(cond, 5000000);  // 5 USDC
    CHECK(call.data.size() == 260);
}

TEST_CASE("encode_merge_positions produces 260 bytes") {
    lt::Bytes32 cond{};
    std::memset(cond.data(), 0xCD, 32);
    auto call = lt::encode_merge_positions(cond, 10000000);
    CHECK(call.data.size() == 260);
}

TEST_CASE("encode_redeem_positions produces 228 bytes") {
    lt::Bytes32 cond{};
    std::memset(cond.data(), 0xEF, 32);
    auto call = lt::encode_redeem_positions(cond);
    CHECK(call.data.size() == 228);
}

TEST_CASE("split and merge have different selectors") {
    CHECK(std::memcmp(lt::SPLIT_POSITION_SELECTOR, lt::MERGE_POSITIONS_SELECTOR, 4) != 0);
    CHECK(std::memcmp(lt::SPLIT_POSITION_SELECTOR, lt::REDEEM_POSITIONS_SELECTOR, 4) != 0);
    CHECK(std::memcmp(lt::MERGE_POSITIONS_SELECTOR, lt::REDEEM_POSITIONS_SELECTOR, 4) != 0);
}

TEST_CASE("split encoding has correct selector at offset 0") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    CHECK(std::memcmp(call.data.data(), lt::SPLIT_POSITION_SELECTOR, 4) == 0);
}

TEST_CASE("merge encoding has correct selector at offset 0") {
    lt::Bytes32 cond{};
    auto call = lt::encode_merge_positions(cond, 1000000);
    CHECK(std::memcmp(call.data.data(), lt::MERGE_POSITIONS_SELECTOR, 4) == 0);
}

TEST_CASE("redeem encoding has correct selector at offset 0") {
    lt::Bytes32 cond{};
    auto call = lt::encode_redeem_positions(cond);
    CHECK(std::memcmp(call.data.data(), lt::REDEEM_POSITIONS_SELECTOR, 4) == 0);
}

TEST_CASE("split encoding has USDC.e address at offset 4") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    // Address is left-padded: 12 zero bytes + 20 address bytes
    for (int i = 4; i < 16; ++i) {
        CHECK(call.data[i] == 0);
    }
    CHECK(std::memcmp(call.data.data() + 16, lt::USDC_E_ADDRESS, 20) == 0);
}

TEST_CASE("split encoding has parentCollectionId zero at offset 36") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    for (int i = 36; i < 68; ++i) {
        CHECK(call.data[i] == 0);
    }
}

TEST_CASE("split encoding has condition_id at offset 68") {
    lt::Bytes32 cond{};
    std::memset(cond.data(), 0x42, 32);
    auto call = lt::encode_split_position(cond, 1000000);
    CHECK(std::memcmp(call.data.data() + 68, cond.data(), 32) == 0);
}

TEST_CASE("split encoding has array offset 160 at offset 100") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    // offset 100: uint256 = 160 = 0x00...00a0
    CHECK(call.data[100 + 31] == 0xa0);
    for (int i = 100; i < 100 + 31; ++i) {
        CHECK(call.data[i] == 0);
    }
}

TEST_CASE("split encoding has amount at offset 132") {
    lt::Bytes32 cond{};
    uint64_t amount = 5000000;  // 5 USDC
    auto call = lt::encode_split_position(cond, amount);
    // uint256 at offset 132, big-endian
    uint8_t expected[32]{};
    lt::uint64_to_uint256_be(amount, expected);
    CHECK(std::memcmp(call.data.data() + 132, expected, 32) == 0);
}

TEST_CASE("split encoding has array length 2 at offset 164") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    CHECK(call.data[164 + 31] == 2);
    for (int i = 164; i < 164 + 31; ++i) {
        CHECK(call.data[i] == 0);
    }
}

TEST_CASE("split encoding has indexSet [1, 2] at offsets 196 and 228") {
    lt::Bytes32 cond{};
    auto call = lt::encode_split_position(cond, 1000000);
    // indexSet[0] = 1
    CHECK(call.data[196 + 31] == 1);
    // indexSet[1] = 2
    CHECK(call.data[228 + 31] == 2);
}

TEST_CASE("redeem encoding has array offset 128 at offset 100") {
    lt::Bytes32 cond{};
    auto call = lt::encode_redeem_positions(cond);
    // offset 100: uint256 = 128 = 0x00...0080
    CHECK(call.data[100 + 31] == 0x80);
    for (int i = 100; i < 100 + 31; ++i) {
        CHECK(call.data[i] == 0);
    }
}

TEST_CASE("redeem encoding has array length 2 and indexSets [1,2]") {
    lt::Bytes32 cond{};
    auto call = lt::encode_redeem_positions(cond);
    // arrayLen at offset 132
    CHECK(call.data[132 + 31] == 2);
    // indexSet[0] at offset 164
    CHECK(call.data[164 + 31] == 1);
    // indexSet[1] at offset 196
    CHECK(call.data[196 + 31] == 2);
}

TEST_CASE("different condition IDs produce different encodings") {
    lt::Bytes32 cond1{};
    lt::Bytes32 cond2{};
    std::memset(cond1.data(), 0x11, 32);
    std::memset(cond2.data(), 0x22, 32);
    auto call1 = lt::encode_split_position(cond1, 1000000);
    auto call2 = lt::encode_split_position(cond2, 1000000);
    CHECK(call1.data != call2.data);
}

TEST_CASE("different amounts produce different encodings") {
    lt::Bytes32 cond{};
    auto call1 = lt::encode_split_position(cond, 1000000);
    auto call2 = lt::encode_split_position(cond, 2000000);
    CHECK(call1.data != call2.data);
}

TEST_CASE("CTF contract address is correct") {
    std::string hex = lt::hex_encode(lt::CTF_CONTRACT_ADDRESS, 20);
    CHECK(hex == "4d97dcd97ec945f40cf65f87097ace5ea0476045");
}

TEST_CASE("USDC.e address is correct") {
    std::string hex = lt::hex_encode(lt::USDC_E_ADDRESS, 20);
    CHECK(hex == "2791bca1f2de4661ed88a30c99a7a9449aa84174");
}

}  // TEST_SUITE
