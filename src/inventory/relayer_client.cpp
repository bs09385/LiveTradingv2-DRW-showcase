// Boost.Asio includes Windows headers that #define ERROR.
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>

#ifdef ERROR
#undef ERROR
#endif

#include "inventory/relayer_client.h"
#include "inventory/relayer_auth.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "simdjson.h"

namespace lt {

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

struct HttpResult {
    std::string body;
    int status = 0;
    std::string error;  // non-empty on exception/failure

    bool ok() const { return status >= 200 && status < 300 && error.empty(); }
};

// Unified HTTPS request with builder auth headers.
// verb: http::verb::get or http::verb::post
// For GET: body should be empty. Success = 200.
// For POST: body is JSON. Success = 200 or 201.
HttpResult sync_builder_request(const std::string& host, const std::string& port,
                                const std::string& target,
                                http::verb verb,
                                const std::string& body,
                                const BuilderAuthHeaders& headers,
                                int timeout_ms) {
    HttpResult result;
    try {
        net::io_context ioc;
        ssl::context ctx{ssl::context::tlsv12_client};
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver{ioc};
        beast::ssl_stream<beast::tcp_stream> stream{ioc, ctx};

        if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
            result.error = "SNI failed for " + host;
            return result;
        }

        auto results = resolver.resolve(host, port);
        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        beast::get_lowest_layer(stream).connect(results);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req{verb, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "LiveTradingv2/1.0");
        req.set(http::field::accept, "application/json");
        req.set(http::field::connection, "close");

        req.set("POLY_BUILDER_API_KEY", headers.api_key);
        req.set("POLY_BUILDER_SIGNATURE", headers.signature);
        req.set("POLY_BUILDER_TIMESTAMP", headers.timestamp);
        req.set("POLY_BUILDER_PASSPHRASE", headers.passphrase);

        if (verb == http::verb::post) {
            req.set(http::field::content_type, "application/json");
            req.body() = body;
            req.prepare_payload();
        }

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::write(stream, req);

        beast::flat_buffer buffer;
        buffer.reserve(1048576);
        http::response_parser<http::string_body> parser;
        parser.body_limit(1048576);

        beast::get_lowest_layer(stream).expires_after(std::chrono::milliseconds(timeout_ms));
        http::read(stream, buffer, parser);

        auto& res = parser.get();
        result.status = static_cast<int>(res.result_int());
        result.body = std::move(res.body());

        beast::error_code ec;
        stream.shutdown(ec);

        if (!result.ok()) {
            result.error = "http=" + std::to_string(result.status) +
                           " body=" + result.body.substr(0, 500);
        }
        return result;
    } catch (const std::exception& e) {
        result.error = std::string("exception: ") + e.what();
        return result;
    } catch (...) {
        result.error = "unknown exception";
        return result;
    }
}

bool is_terminal_state(const std::string& state) {
    return state == "STATE_CONFIRMED" ||
           state == "STATE_FAILED" ||
           state == "STATE_INVALID";
}

}  // namespace

struct RelayerClient::Impl {
    RelayerConfig config_;
    std::string api_key_;
    std::string api_secret_b64_;
    std::string passphrase_;
    std::string last_error_;  // most recent error detail for logging

    Impl(const RelayerConfig& config,
         const std::string& api_key,
         const std::string& api_secret_b64,
         const std::string& passphrase)
        : config_(config),
          api_key_(api_key),
          api_secret_b64_(api_secret_b64),
          passphrase_(passphrase) {}

    BuilderAuthHeaders make_headers(std::string_view method, std::string_view path,
                                    std::string_view body = "") {
        return build_builder_headers(api_key_, api_secret_b64_, passphrase_,
                                     method, path, body);
    }

    HttpResult do_get(const std::string& target) {
        auto headers = make_headers("GET", target);
        return sync_builder_request(config_.host, config_.port, target,
                                    http::verb::get, "", headers,
                                    config_.timeout_ms);
    }

    HttpResult do_post(const std::string& target, const std::string& body) {
        auto headers = make_headers("POST", target, body);
        return sync_builder_request(config_.host, config_.port, target,
                                    http::verb::post, body, headers,
                                    config_.timeout_ms);
    }
};

RelayerClient::RelayerClient(const RelayerConfig& config,
                             const std::string& builder_api_key,
                             const std::string& builder_api_secret_b64,
                             const std::string& builder_passphrase)
    : impl_(std::make_unique<Impl>(config, builder_api_key,
                                    builder_api_secret_b64, builder_passphrase)) {}

RelayerClient::~RelayerClient() = default;

const std::string& RelayerClient::last_error() const {
    return impl_->last_error_;
}

Expected<RelayPayload> RelayerClient::get_relay_payload(const std::string& eoa_address) {
    std::string addr_lower = eoa_address;
    std::transform(addr_lower.begin(), addr_lower.end(), addr_lower.begin(), ::tolower);

    std::string target = "/relay-payload?address=" + addr_lower + "&type=PROXY";
    auto result = impl_->do_get(target);

    if (!result.ok()) {
        impl_->last_error_ = "GET " + target + " -> " + result.error;
        return ErrorCode::NETWORK_ERROR;
    }

    impl_->last_error_.clear();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(result.body).get(doc);
    if (err) {
        impl_->last_error_ = std::string("relay-payload JSON parse: ") +
                              simdjson::error_message(err) +
                              " body=" + result.body.substr(0, 200);
        return ErrorCode::JSON_PARSE_ERROR;
    }

    RelayPayload payload;

    // Parse nonce (may be string or number)
    std::string_view nonce_sv;
    if (doc["nonce"].get_string().get(nonce_sv) == simdjson::SUCCESS) {
        try { payload.nonce = std::stoull(std::string(nonce_sv)); }
        catch (...) {
            impl_->last_error_ = "relay-payload bad nonce: " + std::string(nonce_sv);
            return ErrorCode::JSON_PARSE_ERROR;
        }
    } else {
        uint64_t nonce_val;
        if (doc["nonce"].get_uint64().get(nonce_val) == simdjson::SUCCESS) {
            payload.nonce = nonce_val;
        } else {
            impl_->last_error_ = "relay-payload missing nonce: " + result.body.substr(0, 200);
            return ErrorCode::JSON_MISSING_FIELD;
        }
    }

    // Parse relay address
    std::string_view addr_sv;
    if (doc["address"].get_string().get(addr_sv) == simdjson::SUCCESS) {
        payload.relay_address = std::string(addr_sv);
    } else {
        impl_->last_error_ = "relay-payload missing address: " + result.body.substr(0, 200);
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<RelayPayload>(std::move(payload));
}

Expected<RelayerSubmitResult> RelayerClient::submit(const std::string& body) {
    std::string target = "/submit";
    auto result = impl_->do_post(target, body);

    if (!result.ok()) {
        impl_->last_error_ = "POST " + target + " -> " + result.error;
        return ErrorCode::NETWORK_ERROR;
    }

    impl_->last_error_.clear();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(result.body).get(doc);
    if (err) {
        impl_->last_error_ = std::string("submit JSON parse: ") + simdjson::error_message(err);
        return ErrorCode::JSON_PARSE_ERROR;
    }

    RelayerSubmitResult sr;
    std::string_view sv;
    // Server returns "transactionID" (capital ID)
    if (doc["transactionID"].get_string().get(sv) == simdjson::SUCCESS) {
        sr.transaction_id = std::string(sv);
    } else if (doc["transactionId"].get_string().get(sv) == simdjson::SUCCESS) {
        sr.transaction_id = std::string(sv);
    } else if (doc["id"].get_string().get(sv) == simdjson::SUCCESS) {
        sr.transaction_id = std::string(sv);
    }

    if (doc["transactionHash"].get_string().get(sv) == simdjson::SUCCESS) {
        sr.transaction_hash = std::string(sv);
    }

    // Capture immediate state if present
    if (doc["state"].get_string().get(sv) == simdjson::SUCCESS) {
        sr.state = std::string(sv);
    }

    if (sr.transaction_id.empty()) {
        impl_->last_error_ = "submit missing txId: " + result.body.substr(0, 500);
        return ErrorCode::JSON_MISSING_FIELD;
    }

    return Expected<RelayerSubmitResult>(std::move(sr));
}

Expected<std::string> RelayerClient::get_transaction_state(const std::string& tx_id) {
    std::string target = "/transaction?id=" + tx_id;
    auto result = impl_->do_get(target);

    if (!result.ok()) {
        impl_->last_error_ = "GET " + target + " -> " + result.error;
        return ErrorCode::NETWORK_ERROR;
    }

    impl_->last_error_.clear();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(result.body).get(doc);
    if (err) {
        impl_->last_error_ = std::string("transaction JSON parse: ") + simdjson::error_message(err);
        return ErrorCode::JSON_PARSE_ERROR;
    }

    // The relayer returns an array of transactions: [{...}]
    // Unwrap to the first element if it's an array.
    simdjson::dom::element txn = doc;
    if (doc.type() == simdjson::dom::element_type::ARRAY) {
        simdjson::dom::array arr;
        if (doc.get_array().get(arr) == simdjson::SUCCESS && arr.size() > 0) {
            txn = *arr.begin();
        } else {
            impl_->last_error_ = "transaction empty array: " + result.body.substr(0, 200);
            return ErrorCode::JSON_MISSING_FIELD;
        }
    }

    std::string_view state_sv;
    if (txn["state"].get_string().get(state_sv) == simdjson::SUCCESS) {
        return Expected<std::string>(std::string(state_sv));
    }
    if (txn["status"].get_string().get(state_sv) == simdjson::SUCCESS) {
        return Expected<std::string>(std::string(state_sv));
    }

    impl_->last_error_ = "transaction missing state: " + result.body.substr(0, 200);
    return ErrorCode::JSON_MISSING_FIELD;
}

Expected<std::string> RelayerClient::query_safe_address(const std::string& eoa_address) {
    std::string addr_lower = eoa_address;
    std::transform(addr_lower.begin(), addr_lower.end(), addr_lower.begin(), ::tolower);

    std::string target = "/deployed?address=" + addr_lower;
    auto result = impl_->do_get(target);

    if (!result.ok()) {
        impl_->last_error_ = "GET " + target + " -> " + result.error;
        return ErrorCode::NETWORK_ERROR;
    }

    impl_->last_error_.clear();

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    auto err = parser.parse(result.body).get(doc);
    if (err) {
        impl_->last_error_ = std::string("deployed JSON parse: ") + simdjson::error_message(err);
        return ErrorCode::JSON_PARSE_ERROR;
    }

    std::string_view addr_sv;
    if (doc["address"].get_string().get(addr_sv) == simdjson::SUCCESS) {
        return Expected<std::string>(std::string(addr_sv));
    }
    if (doc["safe"].get_string().get(addr_sv) == simdjson::SUCCESS) {
        return Expected<std::string>(std::string(addr_sv));
    }

    impl_->last_error_ = "deployed missing address: " + result.body.substr(0, 200);
    return ErrorCode::JSON_MISSING_FIELD;
}

}  // namespace lt
