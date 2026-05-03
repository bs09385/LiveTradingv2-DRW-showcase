#include "inventory/relayer_auth.h"
#include "crypto/hmac_sha256.h"

#include <charconv>
#include <chrono>

namespace lt {

BuilderAuthHeaders build_builder_headers(
    const std::string& api_key,
    const std::string& api_secret_b64,
    const std::string& passphrase,
    std::string_view method,
    std::string_view path,
    std::string_view body) {

    // Timestamp = current UNIX seconds
    auto now = std::chrono::system_clock::now();
    auto epoch_secs = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    char ts_buf[32];
    auto [ts_ptr, ts_ec] = std::to_chars(ts_buf, ts_buf + sizeof(ts_buf), epoch_secs);
    if (ts_ec != std::errc()) {
        ts_buf[0] = '0';
        ts_ptr = ts_buf + 1;
    }
    std::string timestamp(ts_buf, static_cast<std::size_t>(ts_ptr - ts_buf));

    // Message = timestamp + method + path + body
    std::string message;
    message.reserve(timestamp.size() + method.size() + path.size() + body.size());
    message.append(timestamp);
    message.append(method);
    message.append(path);
    message.append(body);

    // Decode secret from base64
    auto secret_bytes = base64_decode(api_secret_b64);

    // Signature = base64url(HMAC-SHA256(decoded_secret, message))
    auto hmac = hmac_sha256(secret_bytes.data(), secret_bytes.size(),
                            reinterpret_cast<const uint8_t*>(message.data()),
                            message.size());
    std::string signature = base64url_encode(hmac.data(), hmac.size());

    BuilderAuthHeaders headers;
    headers.api_key = api_key;
    headers.signature = signature;
    headers.timestamp = timestamp;
    headers.passphrase = passphrase;
    return headers;
}

}  // namespace lt
