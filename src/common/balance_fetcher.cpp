#include "common/balance_fetcher.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/discovery.h"
#include "simdjson.h"

namespace lt {

namespace {

// USDC contract on Polygon PoS (6 decimals — raw value = micro-USDC).
constexpr const char* kUsdcContract = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";

// balanceOf(address) function selector.
constexpr const char* kBalanceOfSelector = "0x70a08231";

// Parse a hex string (with or without 0x prefix) into int64_t.
// Returns -1 on overflow or parse failure.
int64_t parse_hex_balance(const char* hex, size_t len) {
    if (len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
        len -= 2;
    }

    // Skip leading zeros.
    while (len > 0 && *hex == '0') {
        ++hex;
        --len;
    }

    // Empty after stripping = zero balance.
    if (len == 0) return 0;

    // int64_t max is ~9.2e18 = 16 hex digits. USDC with 6 decimals means
    // max ~9.2 trillion USDC — more than enough.
    if (len > 16) return -1;

    int64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = hex[i];
        int nibble;
        if (c >= '0' && c <= '9')      nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
        else return -1;
        result = (result << 4) | nibble;
    }
    return result;
}

}  // namespace

Expected<int64_t> fetch_usdc_balance(const std::string& polygon_rpc_url,
                                     const std::string& address) {
    if (polygon_rpc_url.empty()) {
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    // Strip 0x prefix from address and lowercase.
    std::string addr = address;
    if (addr.size() >= 2 && addr[0] == '0' && (addr[1] == 'x' || addr[1] == 'X')) {
        addr = addr.substr(2);
    }
    for (auto& c : addr) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    if (addr.size() != 40) {
        std::fprintf(stderr, "balance_fetcher: invalid address length %zu\n", addr.size());
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    // Build calldata: balanceOf(address) = 0x70a08231 + address padded to 32 bytes.
    // 32 bytes = 64 hex chars, address is 20 bytes = 40 hex chars, so 24 leading zeros.
    char calldata[75];  // "0x70a08231" (10) + 64 hex + null
    std::snprintf(calldata, sizeof(calldata),
                  "%s%024d%s", kBalanceOfSelector, 0, addr.c_str());

    // Build JSON-RPC request body.
    char body[512];
    std::snprintf(body, sizeof(body),
        R"({"jsonrpc":"2.0","method":"eth_call","params":[{"to":"%s","data":"%s"},"latest"],"id":1})",
        kUsdcContract, calldata);

    std::string host = extract_host(polygon_rpc_url);
    std::string path = extract_path(polygon_rpc_url);

    auto response = sync_https_post(host, path, body, 10000);
    if (response.empty()) {
        std::fprintf(stderr, "balance_fetcher: RPC request failed\n");
        return ErrorCode::NETWORK_ERROR;
    }

    // Parse JSON-RPC response: {"jsonrpc":"2.0","id":1,"result":"0x..."}
    try {
        simdjson::dom::parser parser;
        auto doc_result = parser.parse(response);
        if (doc_result.error()) {
            std::fprintf(stderr, "balance_fetcher: JSON parse error\n");
            return ErrorCode::NETWORK_ERROR;
        }

        auto doc = doc_result.value();

        // Check for RPC error.
        simdjson::dom::object err_obj;
        if (!doc["error"].get_object().get(err_obj)) {
            std::string_view msg;
            err_obj["message"].get_string().get(msg);
            std::fprintf(stderr, "balance_fetcher: RPC error: %.*s\n",
                         static_cast<int>(msg.size()), msg.data());
            return ErrorCode::NETWORK_ERROR;
        }

        std::string_view result_hex;
        if (doc["result"].get_string().get(result_hex)) {
            std::fprintf(stderr, "balance_fetcher: missing result field\n");
            return ErrorCode::NETWORK_ERROR;
        }

        int64_t balance = parse_hex_balance(result_hex.data(), result_hex.size());
        if (balance < 0) {
            std::fprintf(stderr, "balance_fetcher: hex parse overflow\n");
            return ErrorCode::NETWORK_ERROR;
        }

        return Expected<int64_t>(balance);
    } catch (...) {
        std::fprintf(stderr, "balance_fetcher: parse exception\n");
        return ErrorCode::NETWORK_ERROR;
    }
}

// Parse hex string to uint64_t. Returns false on overflow (> 2^64).
static bool parse_hex_u64(const char* hex, size_t len, uint64_t& out) {
    if (len >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2; len -= 2;
    }
    while (len > 0 && *hex == '0') { ++hex; --len; }
    if (len == 0) { out = 0; return true; }
    if (len > 16) return false;  // > 2^64
    uint64_t result = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = hex[i];
        int nibble;
        if (c >= '0' && c <= '9')      nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
        else return false;
        result = (result << 4) | static_cast<uint64_t>(nibble);
    }
    out = result;
    return true;
}

Expected<double> fetch_pol_balance(const std::string& polygon_rpc_url,
                                   const std::string& address) {
    if (polygon_rpc_url.empty()) {
        return ErrorCode::CONFIG_PARSE_ERROR;
    }

    // Lowercase address with 0x prefix
    std::string addr = address;
    if (addr.size() < 2 || addr[0] != '0' || (addr[1] != 'x' && addr[1] != 'X')) {
        addr = "0x" + addr;
    }
    for (size_t i = 2; i < addr.size(); ++i) {
        addr[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(addr[i])));
    }

    char body[256];
    std::snprintf(body, sizeof(body),
        R"({"jsonrpc":"2.0","method":"eth_getBalance","params":["%s","latest"],"id":1})",
        addr.c_str());

    std::string host = extract_host(polygon_rpc_url);
    std::string path = extract_path(polygon_rpc_url);

    auto response = sync_https_post(host, path, body, 10000);
    if (response.empty()) {
        return ErrorCode::NETWORK_ERROR;
    }

    try {
        simdjson::dom::parser parser;
        auto doc_result = parser.parse(response);
        if (doc_result.error()) return ErrorCode::NETWORK_ERROR;
        auto doc = doc_result.value();

        simdjson::dom::object err_obj;
        if (doc["error"].get_object().get(err_obj) == simdjson::SUCCESS) {
            return ErrorCode::NETWORK_ERROR;
        }

        std::string_view result_hex;
        if (doc["result"].get_string().get(result_hex) != simdjson::SUCCESS) {
            return ErrorCode::NETWORK_ERROR;
        }

        uint64_t wei = 0;
        if (!parse_hex_u64(result_hex.data(), result_hex.size(), wei)) {
            return ErrorCode::NETWORK_ERROR;
        }

        return Expected<double>(static_cast<double>(wei) / 1e18);
    } catch (...) {
        return ErrorCode::NETWORK_ERROR;
    }
}

}  // namespace lt
