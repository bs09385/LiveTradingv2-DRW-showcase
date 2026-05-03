#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

// HMAC-SHA256 via OpenSSL EVP_MAC API
std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                     const uint8_t* msg, size_t msg_len);

// Base64 decode (standard alphabet)
std::vector<uint8_t> base64_decode(std::string_view b64);

// Base64 encode (standard alphabet)
std::string base64_encode(const uint8_t* data, size_t len);

// Base64url encode: standard base64 with + -> -, / -> _, no padding
std::string base64url_encode(const uint8_t* data, size_t len);

}  // namespace lt
