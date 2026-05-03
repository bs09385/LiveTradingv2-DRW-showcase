#include "doctest/doctest.h"
#include "inventory/rlp_encoder.h"

#include <cstring>

namespace lt {

TEST_SUITE("RLP Encoder") {

TEST_CASE("rlp_encode_uint64: zero encodes as 0x80") {
    std::vector<uint8_t> out;
    rlp_encode_uint64(out, 0);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0x80);
}

TEST_CASE("rlp_encode_uint64: single byte values") {
    // 1 -> 0x01 (single byte, in [0x00, 0x7f] range)
    {
        std::vector<uint8_t> out;
        rlp_encode_uint64(out, 1);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == 0x01);
    }
    // 127 -> 0x7f
    {
        std::vector<uint8_t> out;
        rlp_encode_uint64(out, 127);
        REQUIRE(out.size() == 1);
        CHECK(out[0] == 0x7f);
    }
}

TEST_CASE("rlp_encode_uint64: 128 needs length prefix") {
    std::vector<uint8_t> out;
    rlp_encode_uint64(out, 128);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 0x81);
    CHECK(out[1] == 0x80);
}

TEST_CASE("rlp_encode_uint64: 1024") {
    std::vector<uint8_t> out;
    rlp_encode_uint64(out, 1024);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0x82);
    CHECK(out[1] == 0x04);
    CHECK(out[2] == 0x00);
}

TEST_CASE("rlp_encode_uint64: large value") {
    // 0x0400 = 1024
    std::vector<uint8_t> out;
    rlp_encode_uint64(out, 256);
    REQUIRE(out.size() == 3);
    CHECK(out[0] == 0x82);
    CHECK(out[1] == 0x01);
    CHECK(out[2] == 0x00);
}

TEST_CASE("rlp_encode_string: empty string") {
    std::vector<uint8_t> out;
    rlp_encode_string(out, nullptr, 0);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0x80);
}

TEST_CASE("rlp_encode_string: single byte 0x00") {
    uint8_t data[] = {0x00};
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 1);
    // 0x00 is in [0x00, 0x7f] range, encoded as itself
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0x00);
}

TEST_CASE("rlp_encode_string: single byte 0x7f") {
    uint8_t data[] = {0x7f};
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 1);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0x7f);
}

TEST_CASE("rlp_encode_string: single byte 0x80 needs prefix") {
    uint8_t data[] = {0x80};
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 1);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == 0x81);
    CHECK(out[1] == 0x80);
}

TEST_CASE("rlp_encode_string: 'dog'") {
    const uint8_t data[] = {'d', 'o', 'g'};
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 3);
    REQUIRE(out.size() == 4);
    CHECK(out[0] == 0x83);
    CHECK(out[1] == 'd');
    CHECK(out[2] == 'o');
    CHECK(out[3] == 'g');
}

TEST_CASE("rlp_encode_string: 55-byte string") {
    uint8_t data[55];
    std::memset(data, 'a', 55);
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 55);
    REQUIRE(out.size() == 56);
    CHECK(out[0] == (0x80 + 55));
}

TEST_CASE("rlp_encode_string: 56-byte string (long string)") {
    uint8_t data[56];
    std::memset(data, 'b', 56);
    std::vector<uint8_t> out;
    rlp_encode_string(out, data, 56);
    // 0xb7 + 1 = 0xb8, then 0x38 (56), then data
    REQUIRE(out.size() == 58);
    CHECK(out[0] == 0xb8);
    CHECK(out[1] == 56);
}

TEST_CASE("rlp_wrap_list: empty list") {
    std::vector<uint8_t> items;
    std::vector<uint8_t> out;
    rlp_wrap_list(out, items);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0xc0);
}

TEST_CASE("rlp_wrap_list: [\"cat\", \"dog\"]") {
    std::vector<uint8_t> items;
    const uint8_t cat[] = {'c', 'a', 't'};
    const uint8_t dog[] = {'d', 'o', 'g'};
    rlp_encode_string(items, cat, 3);
    rlp_encode_string(items, dog, 3);

    std::vector<uint8_t> out;
    rlp_wrap_list(out, items);
    // items = 0x83 'c' 'a' 't' 0x83 'd' 'o' 'g' = 8 bytes
    // list prefix: 0xc0 + 8 = 0xc8
    REQUIRE(out.size() == 9);
    CHECK(out[0] == 0xc8);
    CHECK(out[1] == 0x83);
}

TEST_CASE("rlp_encode_uint256: all zeros") {
    uint8_t val[32]{};
    std::vector<uint8_t> out;
    rlp_encode_uint256(out, val);
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 0x80);
}

TEST_CASE("rlp_encode_uint256: small value") {
    uint8_t val[32]{};
    val[31] = 42;
    std::vector<uint8_t> out;
    rlp_encode_uint256(out, val);
    // 42 is in single-byte range
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 42);
}

TEST_CASE("rlp_encode_uint256: 32-byte value") {
    uint8_t val[32];
    std::memset(val, 0xff, 32);
    std::vector<uint8_t> out;
    rlp_encode_uint256(out, val);
    // 32 bytes, prefix 0x80 + 32 = 0xa0
    REQUIRE(out.size() == 33);
    CHECK(out[0] == 0xa0);
    CHECK(out[1] == 0xff);
}

}  // TEST_SUITE

}  // namespace lt
