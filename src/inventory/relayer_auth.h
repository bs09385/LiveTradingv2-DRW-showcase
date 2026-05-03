#pragma once

#include <string>
#include <string_view>

namespace lt {

// Builder HMAC auth headers for Polymarket Relayer v2 API.
// Same HMAC-SHA256 algorithm as L2 CLOB auth (rest_auth.h), different header names.
struct BuilderAuthHeaders {
    std::string api_key;
    std::string signature;
    std::string timestamp;
    std::string passphrase;
};

// Build auth headers for a relayer request.
// method: "GET" or "POST"
// path: e.g. "/nonce", "/submit"
// body: JSON body for POST, empty for GET
BuilderAuthHeaders build_builder_headers(
    const std::string& api_key,
    const std::string& api_secret_b64,
    const std::string& passphrase,
    std::string_view method,
    std::string_view path,
    std::string_view body = "");

}  // namespace lt
