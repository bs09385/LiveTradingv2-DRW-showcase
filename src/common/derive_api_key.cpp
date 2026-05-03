// Boost.Asio includes Windows headers that #define ERROR.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>

#ifdef ERROR
#undef ERROR
#endif

#include "common/derive_api_key.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include "crypto/eip712.h"
#include "crypto/hex_utils.h"
#include "crypto/hmac_sha256.h"
#include "crypto/keccak.h"
#include "crypto/order_signer.h"
#include "simdjson.h"

namespace lt {

// --------------------------------------------------------------------------
// ClobAuth EIP-712 signing for Polymarket L1 authentication
// --------------------------------------------------------------------------
// Domain:  EIP712Domain(string name, string version, uint256 chainId)
//          name = "ClobAuthDomain", version = "1", chainId = 137
// Struct:  ClobAuth(address address, string timestamp, uint256 nonce, string message)
//          message = "This message attests that I control the given wallet"
// --------------------------------------------------------------------------

static Bytes32 compute_clob_auth_domain_typehash() {
    const char* s = "EIP712Domain(string name,string version,uint256 chainId)";
    return keccak256(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

static Bytes32 compute_clob_auth_typehash() {
    const char* s = "ClobAuth(address address,string timestamp,uint256 nonce,string message)";
    return keccak256(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

static const Bytes32 CLOB_AUTH_DOMAIN_TYPEHASH = compute_clob_auth_domain_typehash();
static const Bytes32 CLOB_AUTH_TYPEHASH = compute_clob_auth_typehash();

static Bytes32 compute_clob_auth_domain_separator() {
    const char* name = "ClobAuthDomain";
    Bytes32 name_hash = keccak256(reinterpret_cast<const uint8_t*>(name), std::strlen(name));

    const char* version = "1";
    Bytes32 version_hash = keccak256(reinterpret_cast<const uint8_t*>(version), std::strlen(version));

    Bytes32 chain_id{};
    uint64_to_uint256_be(POLYGON_CHAIN_ID, chain_id.data());

    // encode: domain_typehash || nameHash || versionHash || chainId = 4 * 32 = 128
    uint8_t buf[128];
    std::memcpy(buf + 0,  CLOB_AUTH_DOMAIN_TYPEHASH.data(), 32);
    std::memcpy(buf + 32, name_hash.data(), 32);
    std::memcpy(buf + 64, version_hash.data(), 32);
    std::memcpy(buf + 96, chain_id.data(), 32);

    return keccak256(buf, 128);
}

static const Bytes32 CLOB_AUTH_DOMAIN_SEP = compute_clob_auth_domain_separator();

static Bytes32 hash_clob_auth_struct(const uint8_t address[20],
                                     const std::string& timestamp,
                                     uint64_t nonce) {
    const char* msg = "This message attests that I control the given wallet";

    Bytes32 addr_bytes = address_to_bytes32(address);
    Bytes32 timestamp_hash = keccak256(
        reinterpret_cast<const uint8_t*>(timestamp.data()), timestamp.size());
    Bytes32 nonce_bytes{};
    uint64_to_uint256_be(nonce, nonce_bytes.data());
    Bytes32 msg_hash = keccak256(
        reinterpret_cast<const uint8_t*>(msg), std::strlen(msg));

    // encode: typehash || address || timestampHash || nonce || messageHash = 5 * 32 = 160
    uint8_t buf[160];
    std::memcpy(buf + 0,   CLOB_AUTH_TYPEHASH.data(), 32);
    std::memcpy(buf + 32,  addr_bytes.data(), 32);
    std::memcpy(buf + 64,  timestamp_hash.data(), 32);
    std::memcpy(buf + 96,  nonce_bytes.data(), 32);
    std::memcpy(buf + 128, msg_hash.data(), 32);

    return keccak256(buf, 160);
}

// --------------------------------------------------------------------------
// Build L1 auth headers with fresh timestamp
// --------------------------------------------------------------------------
struct L1Headers {
    std::string poly_address;
    std::string poly_signature;
    std::string poly_timestamp;
    std::string poly_nonce;
};

static Expected<L1Headers> build_l1_headers(Secp256k1OrderSigner& signer,
                                             const uint8_t addr_bytes[20],
                                             const std::string& checksummed_addr,
                                             uint64_t nonce) {
    auto now = std::chrono::system_clock::now();
    auto epoch_s = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    std::string timestamp = std::to_string(epoch_s);

    Bytes32 struct_hash = hash_clob_auth_struct(addr_bytes, timestamp, nonce);
    Bytes32 signing_hash = eip712_signing_hash(CLOB_AUTH_DOMAIN_SEP, struct_hash);

    uint8_t sig_bytes[65];
    if (!signer.sign_hash(signing_hash, sig_bytes)) {
        return ErrorCode::EXEC_SIGNING_FAILED;
    }

    L1Headers h;
    h.poly_address = checksummed_addr;
    h.poly_signature = "0x" + hex_encode(sig_bytes, 65);
    h.poly_timestamp = timestamp;
    h.poly_nonce = std::to_string(nonce);
    return Expected<L1Headers>(std::move(h));
}

// --------------------------------------------------------------------------

Expected<DerivedApiCredentials> derive_api_key(const uint8_t private_key[32],
                                               const std::string& /*address*/) {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace net = boost::asio;
    namespace ssl = boost::asio::ssl;
    using tcp = net::ip::tcp;

    try {
        // 1. Create signer and derive canonical address from private key
        Secp256k1OrderSigner signer(private_key);

        uint8_t addr_bytes[20]{};
        if (!signer.get_signer_address(addr_bytes)) {
            std::cerr << "  derive-api-key: failed to derive address from key\n";
            return ErrorCode::EXEC_SIGNING_FAILED;
        }
        std::string checksummed_addr = eip55_checksum(addr_bytes);
        constexpr uint64_t nonce = 0;

        // 2. Build L1 auth headers (first attempt)
        auto hdr_result = build_l1_headers(signer, addr_bytes, checksummed_addr, nonce);
        if (!hdr_result.ok()) {
            return hdr_result.error;
        }
        auto headers = std::move(hdr_result.value);

        // 3. Set up TLS connection to clob.polymarket.com
        const std::string host = "clob.polymarket.com";
        constexpr int timeout_ms = 10000;

        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            return ErrorCode::REST_CONNECTION_FAILED;
        }

        auto results = resolver.resolve(host, "443");
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        beast::get_lowest_layer(stream).connect(results);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        stream.handshake(ssl::stream_base::client);

        // 4. Try POST /auth/api-key first (create new credentials)
        {
            http::request<http::empty_body> req{http::verb::post, "/auth/api-key", 11};
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "LiveTradingv2/1.0");
            req.set(http::field::accept, "application/json");
            // keep-alive to reuse connection for fallback
            req.set("POLY_ADDRESS", headers.poly_address);
            req.set("POLY_SIGNATURE", headers.poly_signature);
            req.set("POLY_TIMESTAMP", headers.poly_timestamp);
            req.set("POLY_NONCE", headers.poly_nonce);
            req.prepare_payload();

            std::cerr << "  derive-api-key: POST /auth/api-key ...\n";
            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
            http::write(stream, req);
        }

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::read(stream, buffer, res);

        std::cerr << "  derive-api-key: POST /auth/api-key -> HTTP "
                  << res.result_int() << "\n";

        // 5. If POST failed, rebuild headers with fresh timestamp and try GET
        if (res.result_int() != 200) {
            std::cerr << "  derive-api-key: create failed, trying GET /auth/derive-api-key ...\n";

            auto hdr2_result = build_l1_headers(signer, addr_bytes, checksummed_addr, nonce);
            if (!hdr2_result.ok()) {
                return hdr2_result.error;
            }
            auto headers2 = std::move(hdr2_result.value);

            http::request<http::empty_body> req2{http::verb::get, "/auth/derive-api-key", 11};
            req2.set(http::field::host, host);
            req2.set(http::field::user_agent, "LiveTradingv2/1.0");
            req2.set(http::field::accept, "application/json");
            req2.set(http::field::connection, "close");
            req2.set("POLY_ADDRESS", headers2.poly_address);
            req2.set("POLY_SIGNATURE", headers2.poly_signature);
            req2.set("POLY_TIMESTAMP", headers2.poly_timestamp);
            req2.set("POLY_NONCE", headers2.poly_nonce);
            req2.prepare_payload();

            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
            http::write(stream, req2);

            buffer.consume(buffer.size());
            res = {};
            beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
            http::read(stream, buffer, res);

            std::cerr << "  derive-api-key: GET /auth/derive-api-key -> HTTP "
                      << res.result_int() << "\n";
        }

        // Graceful TLS shutdown (ignore errors)
        beast::error_code ec;
        stream.shutdown(ec);

        if (res.result_int() != 200) {
            std::cerr << "  derive-api-key HTTP " << res.result_int()
                      << ": " << res.body() << "\n";
            return ErrorCode::REST_AUTH_FAILED;
        }

        // 6. Parse JSON response
        std::string body = std::move(res.body());
        simdjson::dom::parser parser;
        simdjson::dom::element doc;
        auto err = parser.parse(body).get(doc);
        if (err) {
            return ErrorCode::JSON_PARSE_ERROR;
        }

        DerivedApiCredentials creds;
        std::string_view sv;

        if (doc["apiKey"].get_string().get(sv) != simdjson::SUCCESS) {
            return ErrorCode::JSON_MISSING_FIELD;
        }
        creds.api_key = std::string(sv);

        if (doc["secret"].get_string().get(sv) != simdjson::SUCCESS) {
            return ErrorCode::JSON_MISSING_FIELD;
        }
        creds.api_secret = std::string(sv);

        if (doc["passphrase"].get_string().get(sv) != simdjson::SUCCESS) {
            return ErrorCode::JSON_MISSING_FIELD;
        }
        creds.api_passphrase = std::string(sv);

        return Expected<DerivedApiCredentials>(std::move(creds));
    } catch (const std::exception& e) {
        std::cerr << "  derive-api-key exception: " << e.what() << "\n";
        return ErrorCode::REST_CONNECTION_FAILED;
    } catch (...) {
        return ErrorCode::REST_CONNECTION_FAILED;
    }
}

}  // namespace lt
