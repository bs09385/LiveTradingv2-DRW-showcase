#include "rest/rest_client.h"
#include "common/clock.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>

#include <atomic>
#include <optional>
#include <queue>
#include <random>

#include <openssl/ssl.h>
#include <zlib.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = net::ip::tcp;

namespace lt {

// Decompress a gzip-encoded buffer. Returns false on error.
static bool gzip_decompress(const std::string& compressed, std::string& out) {
    z_stream zs{};
    // 16 + MAX_WBITS tells zlib to expect gzip header
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) return false;

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    char buf[16384];
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        int ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            return false;
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
        if (ret == Z_STREAM_END) break;
    } while (zs.avail_out == 0);

    inflateEnd(&zs);
    return true;
}

struct RestClient::Impl {
    RestClientConfig config;
    net::io_context& ioc;
    ssl::context ssl_ctx{ssl::context::tlsv12_client};
    tcp::resolver resolver;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> stream;
    net::steady_timer timeout_timer;

    net::steady_timer reconnect_timer;
    int64_t reconnect_backoff_ms;
    bool reconnect_scheduled = false;

    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> connected{false};
    bool connecting = false;

    // Request queue and in-flight tracking for HTTP pipelining
    struct PendingRequest {
        RestRequest req;
        L2Headers headers;
        RestCallback cb;
    };
    struct InFlightRequest {
        RestCallback cb;
        Timestamp_ns send_ts = 0;
    };
    std::queue<PendingRequest> pending_;
    std::queue<InFlightRequest> in_flight_;
    bool writing_ = false;
    bool reading_ = false;
    // Response buffer and parser (recreated per read for body_limit enforcement)
    beast::flat_buffer buffer;
    std::optional<http::response_parser<http::string_body>> parser_;

    // TLS session cache for resumption on reconnect
    SSL_SESSION* saved_session_ = nullptr;

    // DNS endpoint cache (avoids re-resolution on reconnect, expires after 5 min)
    tcp::resolver::results_type cached_endpoints_;
    bool has_cached_endpoints_ = false;
    Timestamp_ns dns_cached_at_ = 0;

    Impl(net::io_context& io, const RestClientConfig& cfg)
        : config(cfg), ioc(io), resolver(io), timeout_timer(io),
          reconnect_timer(io), reconnect_backoff_ms(cfg.reconnect_base_ms) {
        // verify_none: no CA bundle on this MinGW system.
        ssl_ctx.set_verify_mode(ssl::verify_none);

        // Enable client-side TLS session caching for faster reconnects
        SSL_CTX_set_session_cache_mode(ssl_ctx.native_handle(), SSL_SESS_CACHE_CLIENT);
    }

    ~Impl() {
        if (saved_session_) {
            SSL_SESSION_free(saved_session_);
            saved_session_ = nullptr;
        }
    }

    void start_connect() {
        if (connecting || connected.load(std::memory_order_relaxed) ||
            shutdown_requested.load(std::memory_order_relaxed))
            return;
        connecting = true;

        // Use cached DNS endpoints if available and not expired (skip ~50-100ms DNS lookup)
        if (has_cached_endpoints_) {
            constexpr Timestamp_ns kDnsCacheExpiryNs = 300'000'000'000LL;  // 5 minutes
            if (SteadyClock::now() - dns_cached_at_ > kDnsCacheExpiryNs) {
                has_cached_endpoints_ = false;
            } else {
                on_resolve(cached_endpoints_);
                return;
            }
        }

        resolver.async_resolve(config.host, config.port,
            [this](beast::error_code ec, tcp::resolver::results_type results) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    if (shutdown_requested.load(std::memory_order_relaxed)) {
                        connecting = false;
                        fail_all("shutdown");
                    } else {
                        on_connect_error(ec.message());
                    }
                    return;
                }
                // Cache resolved endpoints for future reconnects
                cached_endpoints_ = results;
                has_cached_endpoints_ = true;
                dns_cached_at_ = SteadyClock::now();
                on_resolve(results);
            });
    }

    void schedule_reconnect() {
        if (shutdown_requested.load(std::memory_order_relaxed) || reconnect_scheduled) return;

        // Apply +/- 25% jitter
        static thread_local std::mt19937 rng{std::random_device{}()};
        int64_t jitter_range = reconnect_backoff_ms / 4;
        std::uniform_int_distribution<int64_t> dist(-jitter_range, jitter_range);
        int64_t delay_ms = reconnect_backoff_ms + dist(rng);
        if (delay_ms < 1) delay_ms = 1;

        reconnect_scheduled = true;
        reconnect_timer.expires_after(std::chrono::milliseconds(delay_ms));
        reconnect_timer.async_wait([this](boost::system::error_code ec) {
            reconnect_scheduled = false;
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            start_connect();
        });

        // Exponential backoff for next attempt, capped at max
        reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, config.reconnect_max_ms);
    }

    void on_connect_error(const std::string& reason) {
        connected.store(false, std::memory_order_relaxed);
        connecting = false;
        writing_ = false;
        reading_ = false;
        if (stream) {
            beast::get_lowest_layer(*stream).cancel();
            stream.reset();
        }
        if (!shutdown_requested.load(std::memory_order_relaxed) && !pending_.empty()) {
            schedule_reconnect();
        }
        (void)reason;
    }

    void on_resolve(tcp::resolver::results_type results) {
        stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(ioc, ssl_ctx);

        if (!SSL_set_tlsext_host_name(stream->native_handle(), config.host.c_str())) {
            on_connect_error("SSL_set_tlsext_host_name failed");
            return;
        }

        beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*stream).async_connect(results,
            [this](beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    if (shutdown_requested.load(std::memory_order_relaxed)) {
                        connecting = false;
                        fail_all("shutdown");
                    } else {
                        on_connect_error(ec.message());
                    }
                    return;
                }
                on_tcp_connect();
            });
    }

    void on_tcp_connect() {
        // Disable Nagle's algorithm — send small packets immediately (~40ms savings)
        beast::get_lowest_layer(*stream).socket().set_option(tcp::no_delay(true));

        // Restore saved TLS session for resumption (1-RTT handshake instead of 2-RTT)
        if (saved_session_) {
            SSL_set_session(stream->native_handle(), saved_session_);
        }

        beast::get_lowest_layer(*stream).expires_after(std::chrono::seconds(10));
        stream->async_handshake(ssl::stream_base::client,
            [this](beast::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    if (shutdown_requested.load(std::memory_order_relaxed)) {
                        connecting = false;
                        fail_all("shutdown");
                    } else {
                        on_connect_error(ec.message());
                    }
                    return;
                }
                // Save TLS session for future resumption
                if (saved_session_) {
                    SSL_SESSION_free(saved_session_);
                }
                saved_session_ = SSL_get1_session(stream->native_handle());

                // Clear any stale deadline — idle connection has no timeout
                beast::get_lowest_layer(*stream).expires_never();

                connecting = false;
                connected.store(true, std::memory_order_relaxed);
                reconnect_backoff_ms = config.reconnect_base_ms;  // reset on success
                try_write_next();
            });
    }

    void enqueue_request(const RestRequest& req, const L2Headers& headers, RestCallback cb) {
        // Bounded queue: account for both unsent and in-flight requests.
        if (config.max_pending_requests > 0 &&
            (pending_.size() + in_flight_.size()) >= config.max_pending_requests) {
            RestResponse overflow_resp;
            overflow_resp.http_status = 0;
            overflow_resp.timed_out = false;
            if (cb) {
                try { cb(std::move(overflow_resp)); } catch (...) {}
            }
            return;
        }

        pending_.push({req, headers, std::move(cb)});

        if (!connected.load(std::memory_order_relaxed) && !connecting && !reconnect_scheduled) {
            start_connect();
        } else if (connected.load(std::memory_order_relaxed)) {
            try_write_next();
        }
    }

    void try_write_next() {
        if (writing_ || pending_.empty() || !connected.load(std::memory_order_relaxed)) return;
        if (shutdown_requested.load(std::memory_order_relaxed)) {
            fail_all("shutdown");
            return;
        }

        const size_t max_pipeline_depth =
            (config.max_pipeline_depth == 0) ? 1 : config.max_pipeline_depth;
        if (in_flight_.size() >= max_pipeline_depth) return;

        writing_ = true;

        PendingRequest front = std::move(pending_.front());
        pending_.pop();
        Timestamp_ns send_ts = SteadyClock::now();
        in_flight_.push({std::move(front.cb), send_ts});

        // Build HTTP request
        http::verb verb = http::verb::get;
        switch (front.req.method) {
            case HttpMethod::GET: verb = http::verb::get; break;
            case HttpMethod::POST: verb = http::verb::post; break;
            case HttpMethod::DELETE_METHOD: verb = http::verb::delete_; break;
        }

        auto http_req = std::make_shared<http::request<http::string_body>>(
            verb, front.req.path, 11);
        http_req->set(http::field::host, config.host);
        http_req->set(http::field::user_agent, "LiveTradingv2/1.0");
        http_req->set(http::field::connection, "keep-alive");
        http_req->set(http::field::accept, "application/json");
        if (verb == http::verb::get) {
            http_req->set(http::field::accept_encoding, "gzip");
        }

        // L2 auth headers
        http_req->set("POLY_API_KEY", front.headers.api_key);
        http_req->set("POLY_SIGNATURE", front.headers.signature);
        http_req->set("POLY_TIMESTAMP", front.headers.timestamp);
        http_req->set("POLY_PASSPHRASE", front.headers.passphrase);
        http_req->set("POLY_ADDRESS", front.headers.address);

        if (!front.req.body.empty()) {
            http_req->set(http::field::content_type, "application/json");
            http_req->body() = front.req.body;
            http_req->prepare_payload();
        }

        beast::get_lowest_layer(*stream).expires_after(
            std::chrono::milliseconds(config.request_timeout_ms));

        http::async_write(*stream, *http_req,
            [this, http_req](beast::error_code ec, std::size_t) {
                writing_ = false;
                if (ec) {
                    on_write_error(ec);
                    return;
                }
                if (!reading_) {
                    do_read();
                }
                try_write_next();
            });
    }

    void do_read() {
        if (reading_ || in_flight_.empty() || !connected.load(std::memory_order_relaxed)) return;
        reading_ = true;
        beast::get_lowest_layer(*stream).expires_after(
            std::chrono::milliseconds(config.request_timeout_ms));
        buffer.clear();
        parser_.emplace();
        parser_->body_limit(1048576);  // 1MB max response body
        http::async_read(*stream, buffer, *parser_,
            [this](beast::error_code ec, std::size_t) {
                if (ec) {
                    on_read_error(ec);
                    return;
                }
                on_response();
            });
    }

    void on_response() {
        reading_ = false;
        if (in_flight_.empty()) {
            try_write_next();
            return;
        }

        InFlightRequest front = std::move(in_flight_.front());
        in_flight_.pop();

        auto& res = parser_->get();
        Timestamp_ns now = SteadyClock::now();
        RestResponse resp;
        resp.http_status = static_cast<int>(res.result_int());
        resp.body = std::move(res.body());
        resp.latency_ns = now - front.send_ts;
        resp.timed_out = false;

        // Decompress gzip response if Content-Encoding indicates it
        auto encoding = res[http::field::content_encoding];
        if (encoding == "gzip" && !resp.body.empty()) {
            std::string decompressed;
            if (gzip_decompress(resp.body, decompressed)) {
                resp.body = std::move(decompressed);
            }
        }

        if (front.cb) {
            try { front.cb(std::move(resp)); } catch (...) {}
        }

        if (!in_flight_.empty()) {
            do_read();
        } else {
            // No more in-flight — clear deadline so idle connection stays alive
            if (stream) {
                beast::get_lowest_layer(*stream).expires_never();
            }
        }
        try_write_next();
    }

    void fail_in_flight(bool timed_out) {
        while (!in_flight_.empty()) {
            InFlightRequest req = std::move(in_flight_.front());
            in_flight_.pop();
            RestResponse resp;
            resp.http_status = 0;
            resp.timed_out = timed_out;
            if (req.cb) {
                try { req.cb(std::move(resp)); } catch (...) {}
            }
        }
    }

    void on_write_error(beast::error_code ec) {
        connected.store(false, std::memory_order_relaxed);
        connecting = false;
        writing_ = false;
        reading_ = false;
        if (stream) {
            beast::get_lowest_layer(*stream).cancel();
            stream.reset();
        }

        fail_in_flight(ec == beast::error::timeout);

        if (!shutdown_requested.load(std::memory_order_relaxed) && !pending_.empty()) {
            schedule_reconnect();
        }
    }

    void on_read_error(beast::error_code ec) {
        connected.store(false, std::memory_order_relaxed);
        connecting = false;
        writing_ = false;
        reading_ = false;
        if (stream) {
            beast::get_lowest_layer(*stream).cancel();
            stream.reset();
        }

        fail_in_flight(ec == beast::error::timeout);

        if (!shutdown_requested.load(std::memory_order_relaxed) && !pending_.empty()) {
            schedule_reconnect();
        }
    }

    void fail_all(const std::string& reason) {
        fail_in_flight(false);
        while (!pending_.empty()) {
            PendingRequest front = std::move(pending_.front());
            pending_.pop();
            RestResponse resp;
            resp.http_status = 0;
            resp.timed_out = false;
            if (front.cb) {
                try { front.cb(std::move(resp)); } catch (...) {}
            }
        }
        writing_ = false;
        reading_ = false;
        (void)reason;
    }

    void shutdown() {
        shutdown_requested.store(true, std::memory_order_relaxed);
        reconnect_scheduled = false;
        reconnect_timer.cancel();
        // Cancel any in-flight DNS resolve so the io_context destructor
        // doesn't block in win_iocp_io_context::shutdown() waiting on
        // the Windows resolver worker thread.
        resolver.cancel();
        fail_all("shutdown");
        if (stream) {
            beast::get_lowest_layer(*stream).cancel();
        }
    }
};

RestClient::RestClient(boost::asio::io_context& ioc, const RestClientConfig& config)
    : impl_(std::make_unique<Impl>(ioc, config)) {}

RestClient::~RestClient() {
    impl_->shutdown();
}

void RestClient::async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb) {
    impl_->enqueue_request(req, headers, std::move(cb));
}

bool RestClient::is_connected() const {
    return impl_->connected.load(std::memory_order_relaxed);
}

void RestClient::request_shutdown() {
    impl_->shutdown();
}

}  // namespace lt
