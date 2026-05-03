#pragma once

#include <cstdint>
#include <string>

#include "common/error.h"

namespace lt {

struct DerivedApiCredentials {
    std::string api_key;
    std::string api_secret;
    std::string api_passphrase;
};

// Derive Polymarket CLOB API credentials via L1 auth (personal_sign).
// Performs a blocking HTTPS POST to clob.polymarket.com/auth/derive-api-key.
// private_key: 32-byte raw private key
// address: Ethereum address with 0x prefix (e.g. "0x1234...")
Expected<DerivedApiCredentials> derive_api_key(const uint8_t private_key[32],
                                               const std::string& address);

}  // namespace lt
