#pragma once

#include <string>

#include "common/error.h"

namespace lt {

struct AccountInfo {
    std::string name;
    std::string private_key;
    std::string address;
    std::string owner_uuid;
    std::string api_key;
    std::string api_secret;
    std::string api_passphrase;
    std::string builder_api_key;
    std::string builder_api_secret;
    std::string builder_api_passphrase;
    std::string safe_address;  // optional, auto-detected if empty

    // Volatile-zero sensitive fields to limit exposure window.
    void clear_secrets() {
        for (auto& c : private_key) {
            volatile char& v = const_cast<volatile char&>(c);
            v = '\0';
        }
        private_key.clear();
        for (auto& c : api_secret) {
            volatile char& v = const_cast<volatile char&>(c);
            v = '\0';
        }
        api_secret.clear();
        for (auto& c : builder_api_secret) {
            volatile char& v = const_cast<volatile char&>(c);
            v = '\0';
        }
        builder_api_secret.clear();
    }
};

Expected<AccountInfo> load_account(const std::string& path);

// Write AccountInfo back to a JSON file.
// Returns OK on success, CONFIG_PARSE_ERROR on write failure.
ErrorCode save_account(const std::string& path, const AccountInfo& info);

}  // namespace lt
