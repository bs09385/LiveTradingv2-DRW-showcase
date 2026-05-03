#include "rest/rest_auth.h"
#include "crypto/hmac_sha256.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <chrono>
#include <stdexcept>

namespace lt {

RestAuth::RestAuth() {
    const char* key = std::getenv("POLY_API_KEY");
    const char* secret = std::getenv("POLY_API_SECRET");
    const char* pass = std::getenv("POLY_API_PASSPHRASE");
    const char* addr = std::getenv("POLY_ADDRESS");

    if (!key || !secret || !pass || !addr) {
        throw std::runtime_error(
            "Missing required env vars: POLY_API_KEY, POLY_API_SECRET, "
            "POLY_API_PASSPHRASE, POLY_ADDRESS");
    }

    api_key_ = key;
    api_secret_decoded_ = base64_decode(secret);
    passphrase_ = pass;
    address_ = addr;
    // Polymarket L2 auth requires lowercase address
    std::transform(address_.begin(), address_.end(), address_.begin(), ::tolower);

    if (api_secret_decoded_.empty()) {
        throw std::runtime_error("POLY_API_SECRET base64 decode failed");
    }

    valid_ = true;
}

RestAuth::RestAuth(std::string api_key, std::string api_secret_b64,
                   std::string passphrase, std::string address)
    : api_key_(std::move(api_key)),
      passphrase_(std::move(passphrase)),
      address_(std::move(address)) {
    // Polymarket L2 auth requires lowercase address
    std::transform(address_.begin(), address_.end(), address_.begin(), ::tolower);
    api_secret_decoded_ = base64_decode(api_secret_b64);
    if (api_secret_decoded_.empty()) {
        throw std::runtime_error("API secret base64 decode failed");
    }
    valid_ = true;
}

L2Headers RestAuth::build_headers(HttpMethod method, std::string_view path,
                                  std::string_view body) const {
    // Timestamp = current UNIX seconds
    auto now = std::chrono::system_clock::now();
    auto epoch_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    char ts_buf[32];
    auto [ts_ptr, ts_ec] = std::to_chars(ts_buf, ts_buf + sizeof(ts_buf), epoch_secs);
    if (ts_ec != std::errc()) {
        // Impossible with 32-byte buffer for int64_t, but don't crash T3.
        // Fallback "0" will cause auth failure → 401 → handled by gateway.
        ts_buf[0] = '0';
        ts_ptr = ts_buf + 1;
    }
    std::string timestamp(ts_buf, static_cast<std::size_t>(ts_ptr - ts_buf));

    // Message = timestamp + method + path + body
    std::string_view method_str = http_method_str(method);
    std::string message;
    message.reserve(timestamp.size() + method_str.size() + path.size() + body.size());
    message.append(timestamp);
    message.append(method_str);
    message.append(path.data(), path.size());
    message.append(body.data(), body.size());

    // Signature = base64url(HMAC-SHA256(secret, message))
    auto hmac = hmac_sha256(api_secret_decoded_.data(), api_secret_decoded_.size(),
                             reinterpret_cast<const uint8_t*>(message.data()),
                             message.size());
    std::string signature = base64url_encode(hmac.data(), hmac.size());

    L2Headers headers;
    headers.api_key = api_key_;
    headers.signature = signature;
    headers.timestamp = timestamp;
    headers.passphrase = passphrase_;
    headers.address = address_;
    return headers;
}

std::string RestAuth::redacted_summary() const {
    std::string summary = "RestAuth{key=";
    if (api_key_.size() > 8) {
        summary += api_key_.substr(0, 4) + "..." + api_key_.substr(api_key_.size() - 4);
    } else {
        summary += "****";
    }
    summary += ", addr=";
    if (address_.size() > 10) {
        summary += address_.substr(0, 6) + "..." + address_.substr(address_.size() - 4);
    } else {
        summary += "****";
    }
    summary += "}";
    return summary;
}

}  // namespace lt
