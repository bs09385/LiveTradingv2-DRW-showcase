#include <doctest/doctest.h>
#include "inventory/relayer_auth.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hex_utils.h"

#include <cstring>

TEST_SUITE("RelayerAuth") {

TEST_CASE("build_builder_headers returns all 4 fields") {
    std::string api_key = "test-builder-key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";  // base64("test-secret")
    std::string passphrase = "test-builder-pass";

    auto headers = lt::build_builder_headers(
        api_key, secret_b64, passphrase, "GET", "/nonce");

    CHECK(headers.api_key == api_key);
    CHECK_FALSE(headers.signature.empty());
    CHECK_FALSE(headers.timestamp.empty());
    CHECK(headers.passphrase == passphrase);
}

TEST_CASE("signature changes with different path") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    auto h1 = lt::build_builder_headers(api_key, secret_b64, passphrase, "GET", "/nonce");
    auto h2 = lt::build_builder_headers(api_key, secret_b64, passphrase, "GET", "/submit");

    // Timestamps may differ too, but signatures should definitely differ
    // due to different paths even if timestamps were the same
    CHECK(h1.signature != h2.signature);
}

TEST_CASE("signature changes with different method") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    auto h1 = lt::build_builder_headers(api_key, secret_b64, passphrase, "GET", "/submit");
    auto h2 = lt::build_builder_headers(api_key, secret_b64, passphrase, "POST", "/submit");

    CHECK(h1.signature != h2.signature);
}

TEST_CASE("signature changes with different body") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    auto h1 = lt::build_builder_headers(
        api_key, secret_b64, passphrase, "POST", "/submit", R"({"a":1})");
    auto h2 = lt::build_builder_headers(
        api_key, secret_b64, passphrase, "POST", "/submit", R"({"b":2})");

    CHECK(h1.signature != h2.signature);
}

TEST_CASE("empty body produces valid signature") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    auto headers = lt::build_builder_headers(
        api_key, secret_b64, passphrase, "GET", "/nonce", "");
    CHECK_FALSE(headers.signature.empty());
}

TEST_CASE("different secrets produce different signatures") {
    std::string api_key = "key";
    std::string passphrase = "pass";

    // base64("secret-1") and base64("secret-2")
    std::string secret1 = lt::base64_encode(
        reinterpret_cast<const uint8_t*>("secret-1"), 8);
    std::string secret2 = lt::base64_encode(
        reinterpret_cast<const uint8_t*>("secret-2"), 8);

    auto h1 = lt::build_builder_headers(api_key, secret1, passphrase, "GET", "/nonce");
    auto h2 = lt::build_builder_headers(api_key, secret2, passphrase, "GET", "/nonce");

    CHECK(h1.signature != h2.signature);
}

TEST_CASE("timestamp is a reasonable unix epoch value") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    auto headers = lt::build_builder_headers(
        api_key, secret_b64, passphrase, "GET", "/nonce");

    // Timestamp should be parseable as a number > 1700000000 (Nov 2023)
    long long ts = std::stoll(headers.timestamp);
    CHECK(ts > 1700000000LL);
}

TEST_CASE("signature uses base64url encoding (no + or /)") {
    std::string api_key = "key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";

    // Run multiple times to increase chance of hitting chars that differ
    for (int i = 0; i < 10; ++i) {
        auto headers = lt::build_builder_headers(
            api_key, secret_b64, passphrase, "POST", "/submit",
            std::string("{\"n\":") + std::to_string(i) + "}");

        // base64url replaces + with - and / with _ (padding = is kept)
        CHECK(headers.signature.find('+') == std::string::npos);
        CHECK(headers.signature.find('/') == std::string::npos);
    }
}

}  // TEST_SUITE
