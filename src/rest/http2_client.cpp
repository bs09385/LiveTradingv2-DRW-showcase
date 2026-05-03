#include "rest/http2_client.h"
#include "rest/rest_client.h"  // RestClientConfig
#include "common/clock.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/read.hpp>

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace net = boost::asio;
namespace ssl_ns = boost::asio::ssl;
using tcp = net::ip::tcp;

namespace lt {

// Body data provider context for POST requests
struct BodyProvider {
    std::string body;
    size_t offset = 0;
};

// Per-stream context
struct StreamContext {
    RestCallback cb;
    int http_status = 0;
    std::string response_body;
    Timestamp_ns send_ts = 0;
    BodyProvider body_provider;
};

struct Http2Client::Impl {
    Http2ClientConfig config;
    net::io_context& ioc;
    ssl_ns::context ssl_ctx{ssl_ns::context::tlsv12_client};
    tcp::resolver resolver;
    std::unique_ptr<ssl_ns::stream<tcp::socket>> tls_stream;
    net::steady_timer reconnect_timer;
    net::steady_timer timeout_scan_timer;

    nghttp2_session* session = nullptr;
    std::unordered_map<int32_t, StreamContext> streams;
    std::queue<std::tuple<RestRequest, L2Headers, RestCallback>> pending;

    // Optional log callback — set via set_log_callback().
    TransportLogFn log_fn_;

    void h2log(int level, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        if (!log_fn_) return;
        char buf[256];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log_fn_(level, buf);
    }

    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> connected{false};
    bool connecting = false;
    bool writing = false;
    bool reading = false;
    bool reconnect_scheduled = false;
    bool goaway_received = false;
    int64_t reconnect_backoff_ms;

    // TLS session cache for resumption on reconnect
    SSL_SESSION* saved_session = nullptr;

    // DNS endpoint cache
    tcp::resolver::results_type cached_endpoints;
    bool has_cached_endpoints = false;
    Timestamp_ns dns_cached_at = 0;

    // Double-buffered writes: active_buf_ is being written to the socket,
    // staging_buf_ accumulates frames submitted while the write is in-flight.
    // On write completion, swap and flush — no extra RTT waiting.
    std::vector<uint8_t> write_bufs_[2];
    int active_buf_ = 0;       // index currently being written
    bool staged_pending_ = false;  // staging buf has data to flush
    // Read buffer
    std::array<uint8_t, 16384> read_buf{};

    // Max concurrent streams from server SETTINGS
    uint32_t server_max_streams = 100;

    static constexpr size_t kMaxResponseBody = 1048576;  // 1MB

    Impl(net::io_context& io, const Http2ClientConfig& cfg)
        : config(cfg), ioc(io), resolver(io),
          reconnect_timer(io), timeout_scan_timer(io),
          reconnect_backoff_ms(cfg.reconnect_base_ms) {
        ssl_ctx.set_verify_mode(ssl_ns::verify_none);
        SSL_CTX_set_session_cache_mode(ssl_ctx.native_handle(), SSL_SESS_CACHE_CLIENT);

        // Set ALPN to "h2" for HTTP/2
        static const unsigned char alpn[] = {2, 'h', '2'};
        SSL_CTX_set_alpn_protos(ssl_ctx.native_handle(), alpn, sizeof(alpn));

        write_bufs_[0].reserve(32768);
        write_bufs_[1].reserve(32768);
    }

    ~Impl() {
        if (session) {
            nghttp2_session_del(session);
            session = nullptr;
        }
        if (saved_session) {
            SSL_SESSION_free(saved_session);
            saved_session = nullptr;
        }
    }

    // ---- Connection lifecycle ----

    void start_connect() {
        if (connecting || connected.load(std::memory_order_relaxed) ||
            shutdown_requested.load(std::memory_order_relaxed))
            return;
        connecting = true;

        if (has_cached_endpoints) {
            constexpr Timestamp_ns kDnsCacheExpiryNs = 300'000'000'000LL;  // 5 min
            if (SteadyClock::now() - dns_cached_at > kDnsCacheExpiryNs) {
                has_cached_endpoints = false;
            } else {
                on_resolve(cached_endpoints);
                return;
            }
        }

        h2log(0, "H2: resolving %s:%s", config.host.c_str(), config.port.c_str());
        resolver.async_resolve(config.host, config.port,
            [this](boost::system::error_code ec, tcp::resolver::results_type results) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    h2log(1, "H2: DNS failed: %s", ec.message().c_str());
                    connecting = false;
                    fail_all_pending("DNS resolve failed");
                    if (!shutdown_requested.load(std::memory_order_relaxed))
                        schedule_reconnect();
                    return;
                }
                cached_endpoints = results;
                has_cached_endpoints = true;
                dns_cached_at = SteadyClock::now();
                on_resolve(results);
            });
    }

    void on_resolve(tcp::resolver::results_type results) {
        tls_stream = std::make_unique<ssl_ns::stream<tcp::socket>>(ioc, ssl_ctx);

        SSL_set_tlsext_host_name(tls_stream->native_handle(), config.host.c_str());

        // Restore saved TLS session for resumption
        if (saved_session) {
            SSL_set_session(tls_stream->native_handle(), saved_session);
        }

        auto& sock = tls_stream->lowest_layer();
        sock.async_connect(*results.begin(),
            [this](boost::system::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    h2log(1, "H2: TCP connect failed: %s", ec.message().c_str());
                    connecting = false;
                    fail_all_pending("TCP connect failed");
                    if (!shutdown_requested.load(std::memory_order_relaxed))
                        schedule_reconnect();
                    return;
                }
                h2log(0, "H2: TCP connected");
                on_tcp_connect();
            });
    }

    void on_tcp_connect() {
        // Disable Nagle's
        tls_stream->lowest_layer().set_option(tcp::no_delay(true));

        tls_stream->async_handshake(ssl_ns::stream_base::client,
            [this](boost::system::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    h2log(1, "H2: TLS handshake failed: %s", ec.message().c_str());
                    connecting = false;
                    fail_all_pending("TLS handshake failed");
                    if (!shutdown_requested.load(std::memory_order_relaxed))
                        schedule_reconnect();
                    return;
                }
                on_tls_handshake();
            });
    }

    void on_tls_handshake() {
        // Verify ALPN selected "h2"
        const unsigned char* alpn_data = nullptr;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(tls_stream->native_handle(), &alpn_data, &alpn_len);
        if (alpn_len != 2 || alpn_data[0] != 'h' || alpn_data[1] != '2') {
            h2log(1, "H2: ALPN failed: server selected '%.*s' (len=%u), expected 'h2'",
                  alpn_len, alpn_data ? (const char*)alpn_data : "", alpn_len);
            connecting = false;
            fail_all_pending("ALPN h2 not selected");
            schedule_reconnect();
            return;
        }

        // Save TLS session for future resumption
        if (saved_session) {
            SSL_SESSION_free(saved_session);
        }
        saved_session = SSL_get1_session(tls_stream->native_handle());

        // Initialize nghttp2 session
        if (!init_nghttp2_session()) {
            connecting = false;
            schedule_reconnect();
            return;
        }

        // Send connection preface + SETTINGS
        nghttp2_settings_entry settings[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS,
             static_cast<uint32_t>(config.max_concurrent_streams)},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1048576},  // 1MB
        };
        nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE,
                                settings, sizeof(settings) / sizeof(settings[0]));

        connecting = false;
        connected.store(true, std::memory_order_relaxed);
        goaway_received = false;
        reconnect_backoff_ms = config.reconnect_base_ms;
        h2log(0, "H2: connected (h2 negotiated, %zu pending)", pending.size());

        // Flush connection preface + SETTINGS
        session_send();

        // Start read pump
        do_read();

        // Start periodic timeout scan
        schedule_timeout_scan();

        // Submit any pending requests
        try_submit_pending();
    }

    bool init_nghttp2_session() {
        if (session) {
            nghttp2_session_del(session);
            session = nullptr;
        }

        nghttp2_session_callbacks* callbacks = nullptr;
        nghttp2_session_callbacks_new(&callbacks);

        nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_cb);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, on_data_chunk_cb);
        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_cb);
        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_cb);

        int rv = nghttp2_session_client_new(&session, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        return rv == 0;
    }

    void schedule_reconnect() {
        if (shutdown_requested.load(std::memory_order_relaxed) || reconnect_scheduled) return;

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

        reconnect_backoff_ms = std::min(reconnect_backoff_ms * 2, config.reconnect_max_ms);
    }

    void on_connection_error() {
        h2log(1, "H2: connection error: streams=%zu pending=%zu", streams.size(), pending.size());
        connected.store(false, std::memory_order_relaxed);
        writing = false;
        reading = false;
        timeout_scan_timer.cancel();

        // Fail all in-flight streams and pending requests so callers
        // aren't left waiting forever (e.g. heartbeat_in_flight_ guard).
        fail_all_streams("connection error");
        fail_all_pending("connection error");

        if (tls_stream) {
            boost::system::error_code ec;
            tls_stream->lowest_layer().cancel(ec);
            tls_stream->lowest_layer().close(ec);
            tls_stream.reset();
        }

        if (session) {
            nghttp2_session_del(session);
            session = nullptr;
        }

        if (!shutdown_requested.load(std::memory_order_relaxed)) {
            schedule_reconnect();
        }
    }

    // ---- Request submission ----

    void enqueue_request(const RestRequest& req, const L2Headers& headers, RestCallback cb) {
        pending.push({req, headers, std::move(cb)});

        if (!connected.load(std::memory_order_relaxed) && !connecting && !reconnect_scheduled) {
            start_connect();
        } else if (connected.load(std::memory_order_relaxed)) {
            try_submit_pending();
        }
    }

    void try_submit_pending() {
        if (!session || goaway_received) return;

        while (!pending.empty()) {
            // Check server stream limit
            uint32_t active = static_cast<uint32_t>(streams.size());
            uint32_t limit = std::min(server_max_streams,
                static_cast<uint32_t>(config.max_concurrent_streams));
            if (active >= limit) break;

            auto [req, headers, cb] = std::move(pending.front());
            pending.pop();

            submit_request(req, headers, std::move(cb));
        }

        session_send();
    }

    void submit_request(const RestRequest& req, const L2Headers& headers, RestCallback cb) {
        const char* method_str = http_method_str(req.method);

        // Build nghttp2 header array (lowercase per HTTP/2 spec)
        // We need to keep string storage alive until nghttp2_submit_request returns
        std::string content_length_str;
        if (!req.body.empty()) {
            content_length_str = std::to_string(req.body.size());
        }

        std::vector<nghttp2_nv> nva;
        nva.reserve(14);

        auto add_header = [&](const char* name, size_t nlen,
                             const char* value, size_t vlen) {
            nghttp2_nv nv;
            nv.name = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(name));
            nv.namelen = nlen;
            nv.value = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(value));
            nv.valuelen = vlen;
            // Let nghttp2 copy values — the source strings (req, headers) are
            // locals that may be destroyed before session_send() serializes them.
            nv.flags = NGHTTP2_NV_FLAG_NONE;
            nva.push_back(nv);
        };

        // Pseudo-headers (must come first)
        add_header(":method", 7, method_str, std::strlen(method_str));
        add_header(":path", 5, req.path.c_str(), req.path.size());
        add_header(":scheme", 7, "https", 5);
        add_header(":authority", 10, config.host.c_str(), config.host.size());

        // Standard headers
        static const char kUserAgent[] = "LiveTradingv2/1.0";
        static const char kAccept[] = "application/json";
        static const char kContentType[] = "application/json";

        add_header("user-agent", 10, kUserAgent, sizeof(kUserAgent) - 1);
        add_header("accept", 6, kAccept, sizeof(kAccept) - 1);

        // Auth headers (lowercase for HTTP/2)
        add_header("poly_api_key", 12, headers.api_key.c_str(), headers.api_key.size());
        add_header("poly_signature", 14, headers.signature.c_str(), headers.signature.size());
        add_header("poly_timestamp", 14, headers.timestamp.c_str(), headers.timestamp.size());
        add_header("poly_passphrase", 15, headers.passphrase.c_str(), headers.passphrase.size());
        add_header("poly_address", 12, headers.address.c_str(), headers.address.size());

        if (!req.body.empty()) {
            add_header("content-type", 12, kContentType, sizeof(kContentType) - 1);
            add_header("content-length", 14,
                       content_length_str.c_str(), content_length_str.size());
        }

        // Prepare stream context
        StreamContext ctx;
        ctx.cb = std::move(cb);
        ctx.send_ts = SteadyClock::now();

        nghttp2_data_provider* data_prd_ptr = nullptr;
        nghttp2_data_provider data_prd{};

        if (!req.body.empty()) {
            ctx.body_provider.body = req.body;
            ctx.body_provider.offset = 0;
            data_prd.read_callback = body_read_cb;
            data_prd_ptr = &data_prd;
        }

        int32_t stream_id = nghttp2_submit_request(
            session, nullptr, nva.data(), nva.size(),
            data_prd_ptr, nullptr);

        if (stream_id < 0) {
            // Submit failed — invoke callback with error
            RestResponse err_resp;
            err_resp.http_status = 0;
            err_resp.timed_out = false;
            if (ctx.cb) {
                try { ctx.cb(std::move(err_resp)); } catch (...) {}
            }
            return;
        }

        // Insert into stream map — body_read_cb looks up body via stream_id
        streams.emplace(stream_id, std::move(ctx));
    }

    // ---- Asio read/write pump ----

    // Drain nghttp2 output into the staging buffer.  If no write is
    // in-flight, start one immediately.  If a write IS in-flight, the
    // frames accumulate in the staging buffer and flush the instant the
    // current write completes — no extra RTT wait.
    void session_send() {
        if (!session || !connected.load(std::memory_order_relaxed)) return;

        // Drain nghttp2 into the staging buffer (opposite of active_buf_)
        int staging = 1 - active_buf_;
        const uint8_t* data = nullptr;
        ssize_t len;
        while ((len = nghttp2_session_mem_send(session, &data)) > 0) {
            write_bufs_[staging].insert(write_bufs_[staging].end(), data, data + len);
        }

        if (len < 0) {
            on_connection_error();
            return;
        }

        if (!write_bufs_[staging].empty()) {
            staged_pending_ = true;
        }

        // If a write is already in-flight, the completion handler will
        // pick up the staged data.
        if (writing) return;

        flush_staged();
    }

    // Swap staging → active and start the async write.
    void flush_staged() {
        if (!staged_pending_) return;

        // Swap: staging becomes active, old active (now empty) becomes staging
        active_buf_ = 1 - active_buf_;
        staged_pending_ = false;

        writing = true;
        auto& buf = write_bufs_[active_buf_];
        net::async_write(*tls_stream,
            net::buffer(buf.data(), buf.size()),
            [this](boost::system::error_code ec, std::size_t) {
                writing = false;
                write_bufs_[active_buf_].clear();
                if (ec) {
                    on_connection_error();
                    return;
                }
                // Immediately flush anything that accumulated during our write.
                // Also drain nghttp2 in case callbacks queued more frames.
                session_send();
            });
    }

    void do_read() {
        if (reading || !connected.load(std::memory_order_relaxed) || !tls_stream) return;
        reading = true;

        tls_stream->async_read_some(
            net::buffer(read_buf.data(), read_buf.size()),
            [this](boost::system::error_code ec, std::size_t bytes_read) {
                reading = false;
                if (ec) {
                    on_connection_error();
                    return;
                }

                ssize_t rv = nghttp2_session_mem_recv(
                    session, read_buf.data(), bytes_read);
                if (rv < 0) {
                    on_connection_error();
                    return;
                }

                // Flush any response frames (WINDOW_UPDATE, etc.)
                session_send();

                // Continue reading
                if (connected.load(std::memory_order_relaxed)) {
                    do_read();
                }
            });
    }

    // ---- Timeout scan ----

    void schedule_timeout_scan() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;

        timeout_scan_timer.expires_after(std::chrono::milliseconds(500));
        timeout_scan_timer.async_wait([this](boost::system::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            scan_timeouts();
            schedule_timeout_scan();
        });
    }

    void scan_timeouts() {
        if (!session) return;

        Timestamp_ns now = SteadyClock::now();
        int64_t timeout_ns = config.request_timeout_ms * 1'000'000LL;

        std::vector<int32_t> timed_out_streams;
        for (auto& [stream_id, ctx] : streams) {
            if (now - ctx.send_ts > timeout_ns) {
                timed_out_streams.push_back(stream_id);
            }
        }

        for (int32_t sid : timed_out_streams) {
            nghttp2_submit_rst_stream(session, NGHTTP2_FLAG_NONE, sid, NGHTTP2_CANCEL);

            auto it = streams.find(sid);
            if (it != streams.end()) {
                RestResponse resp;
                resp.http_status = 0;
                resp.timed_out = true;
                resp.latency_ns = now - it->second.send_ts;
                if (it->second.cb) {
                    try { it->second.cb(std::move(resp)); } catch (...) {}
                }
                streams.erase(it);
            }
        }

        if (!timed_out_streams.empty()) {
            session_send();
        }
    }

    // ---- Failure helpers ----

    void fail_all_streams(const char* /*reason*/) {
        for (auto& [sid, ctx] : streams) {
            RestResponse resp;
            resp.http_status = 0;
            resp.timed_out = false;
            resp.latency_ns = SteadyClock::now() - ctx.send_ts;
            if (ctx.cb) {
                try { ctx.cb(std::move(resp)); } catch (...) {}
            }
        }
        streams.clear();
    }

    void fail_all_pending(const char* /*reason*/) {
        while (!pending.empty()) {
            auto& [req, headers, cb] = pending.front();
            RestResponse resp;
            resp.http_status = 0;
            resp.timed_out = false;
            if (cb) {
                try { cb(std::move(resp)); } catch (...) {}
            }
            pending.pop();
        }
    }

    void request_shutdown_impl() {
        shutdown_requested.store(true, std::memory_order_relaxed);
        reconnect_timer.cancel();
        timeout_scan_timer.cancel();

        // Cancel any in-flight DNS resolve. On Windows, async_resolve uses a
        // worker thread; without an explicit cancel the io_context destructor
        // can block indefinitely in win_iocp_io_context::shutdown() waiting
        // for the resolver completion to drain.
        resolver.cancel();

        fail_all_streams("shutdown");
        fail_all_pending("shutdown");

        if (tls_stream) {
            boost::system::error_code ec;
            tls_stream->lowest_layer().cancel(ec);
            tls_stream->lowest_layer().close(ec);
            tls_stream.reset();
        }

        if (session) {
            nghttp2_session_del(session);
            session = nullptr;
        }
    }

    // ---- nghttp2 callbacks (static) ----

    static int on_header_cb(nghttp2_session* /*session*/,
                            const nghttp2_frame* frame,
                            const uint8_t* name, size_t namelen,
                            const uint8_t* value, size_t valuelen,
                            uint8_t /*flags*/, void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        if (frame->hd.type != NGHTTP2_HEADERS) return 0;

        auto it = self->streams.find(frame->hd.stream_id);
        if (it == self->streams.end()) return 0;

        // Capture :status pseudo-header
        if (namelen == 7 && std::memcmp(name, ":status", 7) == 0) {
            it->second.http_status = 0;
            for (size_t i = 0; i < valuelen; ++i) {
                it->second.http_status = it->second.http_status * 10 + (value[i] - '0');
            }
        }

        return 0;
    }

    static int on_data_chunk_cb(nghttp2_session* /*session*/,
                                uint8_t /*flags*/, int32_t stream_id,
                                const uint8_t* data, size_t len,
                                void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        auto it = self->streams.find(stream_id);
        if (it == self->streams.end()) return 0;

        // Enforce body size limit
        if (it->second.response_body.size() + len > kMaxResponseBody) {
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }

        it->second.response_body.append(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static int on_stream_close_cb(nghttp2_session* /*session*/,
                                  int32_t stream_id,
                                  uint32_t /*error_code*/,
                                  void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        auto it = self->streams.find(stream_id);
        if (it == self->streams.end()) return 0;

        Timestamp_ns now = SteadyClock::now();
        RestResponse resp;
        resp.http_status = it->second.http_status;
        resp.body = std::move(it->second.response_body);
        resp.latency_ns = now - it->second.send_ts;
        resp.timed_out = false;

        RestCallback cb = std::move(it->second.cb);
        self->streams.erase(it);

        if (cb) {
            try { cb(std::move(resp)); } catch (...) {}
        }

        // Try to submit pending requests now that a stream slot freed up
        self->try_submit_pending();

        return 0;
    }

    static int on_frame_recv_cb(nghttp2_session* /*session*/,
                                const nghttp2_frame* frame,
                                void* user_data) {
        auto* self = static_cast<Impl*>(user_data);

        if (frame->hd.type == NGHTTP2_GOAWAY) {
            // Stop submitting new requests, let existing streams drain
            self->goaway_received = true;
            // If no streams in flight, reconnect immediately
            if (self->streams.empty()) {
                self->on_connection_error();
            }
        }

        if (frame->hd.type == NGHTTP2_SETTINGS &&
            (frame->hd.flags & NGHTTP2_FLAG_ACK) == 0) {
            // Update server's max concurrent streams
            uint32_t val = nghttp2_session_get_remote_settings(
                self->session, NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS);
            if (val > 0) {
                self->server_max_streams = val;
            }
        }

        return 0;
    }

    // Body data provider callback for POST requests.
    // Looks up the BodyProvider via streams map (safe: only called during
    // session_send() which runs after stream insertion).
    static ssize_t body_read_cb(nghttp2_session* /*session*/,
                                int32_t stream_id,
                                uint8_t* buf, size_t length,
                                uint32_t* data_flags,
                                nghttp2_data_source* /*source*/,
                                void* user_data) {
        auto* self = static_cast<Impl*>(user_data);
        auto it = self->streams.find(stream_id);
        if (it == self->streams.end()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }
        auto* bp = &it->second.body_provider;

        size_t remaining = bp->body.size() - bp->offset;
        size_t to_copy = std::min(remaining, length);

        if (to_copy > 0) {
            std::memcpy(buf, bp->body.data() + bp->offset, to_copy);
            bp->offset += to_copy;
        }

        if (bp->offset >= bp->body.size()) {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        }

        return static_cast<ssize_t>(to_copy);
    }
};

// ---- Public API ----

Http2Client::Http2Client(boost::asio::io_context& ioc, const Http2ClientConfig& config)
    : impl_(std::make_unique<Impl>(ioc, config)) {}

Http2Client::~Http2Client() = default;

void Http2Client::async_send(const RestRequest& req, const L2Headers& headers, RestCallback cb) {
    impl_->enqueue_request(req, headers, std::move(cb));
}

bool Http2Client::is_connected() const {
    return impl_->connected.load(std::memory_order_relaxed);
}

void Http2Client::request_shutdown() {
    impl_->request_shutdown_impl();
}

void Http2Client::set_log_callback(TransportLogFn fn) {
    impl_->log_fn_ = std::move(fn);
}

}  // namespace lt
