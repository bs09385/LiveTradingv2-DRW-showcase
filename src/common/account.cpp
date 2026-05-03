#include "common/account.h"

#include <fstream>
#include <sstream>

#include "simdjson.h"

namespace lt {

Expected<AccountInfo> load_account(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return ErrorCode::CONFIG_FILE_NOT_FOUND;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string json_str = ss.str();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(json_str).get(doc);
    if (err) {
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    AccountInfo info;

    auto read_required = [&](const char* key, std::string& out) -> bool {
        std::string_view sv;
        if (doc[key].get_string().get(sv) != simdjson::SUCCESS) {
            return false;
        }
        out = std::string(sv);
        return true;
    };

    auto read_optional = [&](const char* key, std::string& out) {
        std::string_view sv;
        if (doc[key].get_string().get(sv) == simdjson::SUCCESS) {
            out = std::string(sv);
        }
    };

    // Required: name, private_key, address
    if (!read_required("name", info.name)) return ErrorCode::JSON_MISSING_FIELD;
    if (!read_required("private_key", info.private_key)) return ErrorCode::JSON_MISSING_FIELD;
    if (!read_required("address", info.address)) return ErrorCode::JSON_MISSING_FIELD;

    // Optional: auto-derived on startup if missing
    read_optional("owner_uuid", info.owner_uuid);
    read_optional("api_key", info.api_key);
    read_optional("api_secret", info.api_secret);
    read_optional("api_passphrase", info.api_passphrase);
    read_optional("builder_api_key", info.builder_api_key);
    read_optional("builder_api_secret", info.builder_api_secret);
    read_optional("builder_api_passphrase", info.builder_api_passphrase);
    read_optional("safe_address", info.safe_address);

    return Expected<AccountInfo>(std::move(info));
}

ErrorCode save_account(const std::string& path, const AccountInfo& info) {
    std::ofstream file(path);
    if (!file.is_open()) {
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    // Escape JSON string values (handle backslashes and quotes)
    auto escape_json = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '"') { out += "\\\""; }
            else if (c == '\\') { out += "\\\\"; }
            else { out += c; }
        }
        return out;
    };

    file << "{\n";
    file << "    \"name\": \"" << escape_json(info.name) << "\",\n";
    file << "    \"private_key\": \"" << escape_json(info.private_key) << "\",\n";
    file << "    \"address\": \"" << escape_json(info.address) << "\"";

    if (!info.owner_uuid.empty()) {
        file << ",\n    \"owner_uuid\": \"" << escape_json(info.owner_uuid) << "\"";
    }
    if (!info.api_key.empty()) {
        file << ",\n    \"api_key\": \"" << escape_json(info.api_key) << "\"";
    }
    if (!info.api_secret.empty()) {
        file << ",\n    \"api_secret\": \"" << escape_json(info.api_secret) << "\"";
    }
    if (!info.api_passphrase.empty()) {
        file << ",\n    \"api_passphrase\": \"" << escape_json(info.api_passphrase) << "\"";
    }
    if (!info.builder_api_key.empty()) {
        file << ",\n    \"builder_api_key\": \"" << escape_json(info.builder_api_key) << "\"";
    }
    if (!info.builder_api_secret.empty()) {
        file << ",\n    \"builder_api_secret\": \"" << escape_json(info.builder_api_secret) << "\"";
    }
    if (!info.builder_api_passphrase.empty()) {
        file << ",\n    \"builder_api_passphrase\": \"" << escape_json(info.builder_api_passphrase) << "\"";
    }
    if (!info.safe_address.empty()) {
        file << ",\n    \"safe_address\": \"" << escape_json(info.safe_address) << "\"";
    }
    file << "\n}\n";

    return ErrorCode::OK;
}

}  // namespace lt
