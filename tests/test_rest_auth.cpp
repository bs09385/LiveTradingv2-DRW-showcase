#include <doctest/doctest.h>
#include "rest/rest_auth.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hex_utils.h"

#include <cstring>
#include <stdexcept>

TEST_SUITE("RestAuth") {

TEST_CASE("build_headers returns all 5 fields") {
    // Use a known base64 secret for testing
    std::string api_key = "test-key-123";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";  // base64("test-secret")
    std::string passphrase = "test-passphrase";
    std::string address = "0x1234567890abcdef1234567890abcdef12345678";

    lt::RestAuth auth(api_key, secret_b64, passphrase, address);
    CHECK(auth.is_valid());

    auto headers = auth.build_headers(lt::HttpMethod::POST, "/order", R"({"test":"body"})");

    CHECK(headers.api_key == api_key);
    CHECK_FALSE(headers.signature.empty());
    CHECK_FALSE(headers.timestamp.empty());
    CHECK(headers.passphrase == passphrase);
    CHECK(headers.address == address);
}

TEST_CASE("signature changes with different body") {
    std::string api_key = "test-key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";
    std::string address = "0xaddr";

    lt::RestAuth auth(api_key, secret_b64, passphrase, address);

    auto h1 = auth.build_headers(lt::HttpMethod::POST, "/order", R"({"body":"one"})");
    auto h2 = auth.build_headers(lt::HttpMethod::POST, "/order", R"({"body":"two"})");

    CHECK(h1.signature != h2.signature);
}

TEST_CASE("signature changes with different method") {
    std::string api_key = "test-key";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "pass";
    std::string address = "0xaddr";

    lt::RestAuth auth(api_key, secret_b64, passphrase, address);

    auto h1 = auth.build_headers(lt::HttpMethod::POST, "/order", "");
    auto h2 = auth.build_headers(lt::HttpMethod::DELETE_METHOD, "/order", "");

    CHECK(h1.signature != h2.signature);
}

TEST_CASE("redacted summary doesn't leak secrets") {
    std::string api_key = "abcdefghijklmnop";
    std::string secret_b64 = "dGVzdC1zZWNyZXQ=";
    std::string passphrase = "my-secret-pass";
    std::string address = "0x1234567890abcdef1234567890abcdef12345678";

    lt::RestAuth auth(api_key, secret_b64, passphrase, address);
    std::string summary = auth.redacted_summary();

    // Should not contain the full API key or passphrase
    CHECK(summary.find(api_key) == std::string::npos);
    CHECK(summary.find(passphrase) == std::string::npos);
    // Should contain partial key
    CHECK(summary.find("abcd") != std::string::npos);
}

TEST_CASE("invalid base64 secret throws") {
    CHECK_THROWS_AS(
        lt::RestAuth("key", "", "pass", "addr"),
        std::runtime_error);
}

}  // TEST_SUITE
