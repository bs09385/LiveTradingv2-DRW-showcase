#pragma once

#include <memory>
#include <string>
#include "rest/rest_types.h"
#include "rest/rest_transport.h"

// Forward declare Boost types to avoid header inclusion
namespace boost { namespace asio { class io_context; } }

namespace lt {

struct RestClientConfig;

struct Http2ClientConfig {
    std::string host = "clob.polymarket.com";
    std::string port = "443";
    int64_t request_timeout_ms = 5000;
    int max_concurrent_streams = 100;
    int64_t reconnect_base_ms = 500;
    int64_t reconnect_max_ms = 30000;
};

// HTTP/2 REST client using nghttp2 + Boost.Asio.
// Stream-multiplexed: no head-of-line blocking between concurrent requests.
// Thread ownership: called from T3 (ExecutionGateway), runs on T3's io_context.
class Http2Client : public IRestTransport {
public:
    explicit Http2Client(boost::asio::io_context& ioc, const Http2ClientConfig& config);
    ~Http2Client() override;

    Http2Client(const Http2Client&) = delete;
    Http2Client& operator=(const Http2Client&) = delete;

    void async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb) override;
    bool is_connected() const override;
    void request_shutdown() override;
    void set_log_callback(TransportLogFn fn) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lt
