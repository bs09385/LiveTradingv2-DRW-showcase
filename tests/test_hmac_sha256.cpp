#include <doctest/doctest.h>
#include "crypto/hmac_sha256.h"
#include "crypto/hex_utils.h"

#include <cstring>

TEST_SUITE("HMAC-SHA256") {

TEST_CASE("RFC 4231 test vector 1") {
    // Key: 0x0b * 20 bytes
    // Data: "Hi There"
    // Expected: b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
    uint8_t key[20];
    std::memset(key, 0x0b, 20);
    const char* data = "Hi There";
    auto hmac = lt::hmac_sha256(key, 20,
        reinterpret_cast<const uint8_t*>(data), std::strlen(data));
    std::string hex = lt::hex_encode(hmac.data(), hmac.size());
    CHECK(hex == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST_CASE("RFC 4231 test vector 2") {
    // Key: "Jefe"
    // Data: "what do ya want for nothing?"
    // Expected: 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
    const char* key = "Jefe";
    const char* data = "what do ya want for nothing?";
    auto hmac = lt::hmac_sha256(
        reinterpret_cast<const uint8_t*>(key), std::strlen(key),
        reinterpret_cast<const uint8_t*>(data), std::strlen(data));
    std::string hex = lt::hex_encode(hmac.data(), hmac.size());
    CHECK(hex == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST_CASE("base64 encode/decode roundtrip") {
    const uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    std::string encoded = lt::base64_encode(data, 6);
    auto decoded = lt::base64_decode(encoded);
    REQUIRE(decoded.size() == 6);
    CHECK(std::memcmp(decoded.data(), data, 6) == 0);
}

TEST_CASE("base64url encode keeps padding") {
    const uint8_t data[] = {0xFF, 0xFE};
    std::string encoded = lt::base64url_encode(data, 2);
    // Should not contain standard base64 chars + and /
    CHECK(encoded.find('+') == std::string::npos);
    CHECK(encoded.find('/') == std::string::npos);
    // Should preserve padding (matching official Polymarket client behavior)
    CHECK(encoded.find('=') != std::string::npos);
}

TEST_CASE("base64 decode handles base64url input") {
    // base64url: - instead of +, _ instead of /
    std::string b64url = "SGVsbG8gV29ybGQ";
    auto decoded = lt::base64_decode(b64url);
    std::string result(decoded.begin(), decoded.end());
    CHECK(result == "Hello World");
}

}  // TEST_SUITE
