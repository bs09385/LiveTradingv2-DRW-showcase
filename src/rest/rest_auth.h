#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "rest/rest_types.h"

namespace lt {

class RestAuth {
public:
    // Reads POLY_API_KEY, POLY_API_SECRET, POLY_API_PASSPHRASE, POLY_ADDRESS from env.
    // Throws if any required var is missing.
    RestAuth();

    // Construct with explicit credentials (for testing)
    RestAuth(std::string api_key, std::string api_secret_b64,
             std::string passphrase, std::string address);

    // Build L2 authentication headers for a request.
    // method: "GET", "POST", "DELETE"
    // path: e.g., "/order"
    // body: JSON body (empty for GET/DELETE)
    L2Headers build_headers(HttpMethod method, std::string_view path,
                            std::string_view body) const;

    // Check if credentials are loaded
    bool is_valid() const { return valid_; }

    // Redacted summary (for logging)
    std::string redacted_summary() const;

private:
    std::string api_key_;
    std::vector<uint8_t> api_secret_decoded_;  // base64-decoded secret
    std::string passphrase_;
    std::string address_;
    bool valid_ = false;
};

}  // namespace lt
