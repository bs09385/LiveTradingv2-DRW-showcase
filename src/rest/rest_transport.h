#pragma once

#include <functional>
#include <memory>
#include <string>
#include "rest/rest_types.h"

// Forward declare Boost types to avoid header inclusion
namespace boost { namespace asio { class io_context; } }

namespace lt {

struct RestClientConfig;

// Log callback: (level 0=INFO 1=WARN 2=ERROR, message)
using TransportLogFn = std::function<void(int level, const char* msg)>;

// Abstract transport interface for REST requests.
// Implemented by RestClient (HTTP/1.1) and Http2Client (HTTP/2).
// Virtual dispatch cost (~1ns) is irrelevant on T3 vs ~50ms REST RTT.
class IRestTransport {
public:
    virtual ~IRestTransport() = default;

    virtual void async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb) = 0;
    virtual bool is_connected() const = 0;
    virtual void request_shutdown() = 0;

    // Optional: route diagnostics to caller's logger.
    virtual void set_log_callback(TransportLogFn) {}
};

// Factory: creates RestClient or Http2Client based on config.
// use_http2: if true, returns Http2Client; otherwise RestClient.
// max_concurrent_streams: only used by Http2Client (ignored by RestClient).
std::unique_ptr<IRestTransport> make_rest_transport(
    boost::asio::io_context& ioc,
    const RestClientConfig& config,
    bool use_http2,
    int max_concurrent_streams = 100);

}  // namespace lt
