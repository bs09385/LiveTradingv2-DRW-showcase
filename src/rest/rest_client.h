#pragma once

#include <memory>
#include <string>
#include "rest/rest_types.h"

// Forward declare Boost types to avoid header inclusion
namespace boost { namespace asio { class io_context; } }

namespace lt {

struct RestClientConfig {
    std::string host = "clob.polymarket.com";
    std::string port = "443";
    int64_t request_timeout_ms = 5000;
    size_t max_pending_requests = 64;  // bounded queue; 0 = unlimited
    size_t max_pipeline_depth = 4;
    int64_t reconnect_base_ms = 500;
    int64_t reconnect_max_ms = 30000;
};

// Boost.Beast async HTTPS client (Pimpl pattern)
// Maintains a persistent TLS connection with keep-alive.
// Thread ownership: called from T3 (ExecutionGateway), runs on T3's io_context.
class RestClient {
public:
    explicit RestClient(boost::asio::io_context& ioc, const RestClientConfig& config);
    ~RestClient();

    RestClient(const RestClient&) = delete;
    RestClient& operator=(const RestClient&) = delete;

    // Send an authenticated REST request. Callback invoked on T3's io_context.
    void async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb);

    // Check if connection is established
    bool is_connected() const;

    // Request shutdown
    void request_shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
