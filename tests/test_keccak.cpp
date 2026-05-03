#include <doctest/doctest.h>
#include "crypto/keccak.h"
#include "crypto/hex_utils.h"

#include <cstring>

TEST_SUITE("Keccak-256") {

TEST_CASE("keccak256 empty string") {
    lt::Bytes32 result = lt::keccak256(nullptr, 0);
    std::string hex = lt::hex_encode(result);
    CHECK(hex == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470");
}

TEST_CASE("keccak256 'abc'") {
    const uint8_t data[] = {'a', 'b', 'c'};
    lt::Bytes32 result = lt::keccak256(data, 3);
    std::string hex = lt::hex_encode(result);
    CHECK(hex == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

TEST_CASE("keccak256 'hello'") {
    const char* msg = "hello";
    lt::Bytes32 result = lt::keccak256(
        reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));
    std::string hex = lt::hex_encode(result);
    CHECK(hex == "1c8aff950685c2ed4bc3174f3472287b56d9517b9c948127319a09a7a36deac8");
}

TEST_CASE("streaming hasher matches one-shot") {
    const char* msg = "test message for streaming";
    auto len = std::strlen(msg);

    lt::Bytes32 oneshot = lt::keccak256(
        reinterpret_cast<const uint8_t*>(msg), len);

    lt::Keccak256Hasher hasher;
    hasher.update(reinterpret_cast<const uint8_t*>(msg), 5);
    hasher.update(reinterpret_cast<const uint8_t*>(msg + 5), len - 5);
    lt::Bytes32 streamed = hasher.finalize();

    CHECK(oneshot == streamed);
}

TEST_CASE("keccak256 is NOT sha3-256") {
    // SHA-3-256("abc") = 3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532
    // Keccak-256("abc") = 4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45
    const uint8_t data[] = {'a', 'b', 'c'};
    lt::Bytes32 result = lt::keccak256(data, 3);
    std::string hex = lt::hex_encode(result);
    CHECK(hex != "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532");
    CHECK(hex == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45");
}

}  // TEST_SUITE
