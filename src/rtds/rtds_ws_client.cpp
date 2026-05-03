#include "rtds/rtds_ws_client.h"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include "common/clock.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace lt {

struct RtdsWsClient::Impl {
    RtdsWsConfig config;
    net::io_context ioc;
    ssl::context ssl_ctx{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
    tcp::resolver resolver{ioc};
    beast::flat_buffer buffer;
    net::steady_timer ping_timer{ioc};
    net::steady_timer reconnect_timer{ioc};
    net::steady_timer stale_timer{ioc};
    std::string read_payload;

    // RTDS subscribe message for crypto_prices
    static constexpr const char* kSubscribeMsg =
        R"({"action":"subscribe","subscriptions":[{"topic":"crypto_prices","type":"update"}]})";
    static constexpr const char* kPingMsg = "PING";

    RtdsOnMessageCb on_message;
    RtdsOnConnectedCb on_connected;
    RtdsOnDisconnectedCb on_disconnected;

    std::atomic<bool> shutdown_requested{false};
    int64_t reconnect_delay_ms = 1000;
    Timestamp_ns last_message_ts = 0;
    bool reconnecting_ = false;

    std::string host;
    std::string port;
    std::string path;

    Impl(const RtdsWsConfig& cfg) : config(cfg) {
        // Parse endpoint URL
        auto url = config.endpoint;
        if (url.substr(0, 6) == "wss://") {
            url = url.substr(6);
        } else if (url.substr(0, 5) == "ws://") {
            url = url.substr(5);
        }

        auto slash_pos = url.find('/');
        if (slash_pos != std::string::npos) {
            host = url.substr(0, slash_pos);
            path = url.substr(slash_pos);
        } else {
            host = url;
            path = "/";
        }

        auto colon_pos = host.find(':');
        if (colon_pos != std::string::npos) {
            port = host.substr(colon_pos + 1);
            host = host.substr(0, colon_pos);
        } else {
            port = config.endpoint.substr(0, 3) == "wss" ? "443" : "80";
        }

        ssl_ctx.set_verify_mode(ssl::verify_none);
        reconnect_delay_ms = config.reconnect_base_ms;
    }

    void start_connect() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        reconnecting_ = false;

        // Close previous connection before creating a new one.
        // Without this, the old async_read callback can fire on a
        // destroyed SSL stream during rapid reconnect cycles (segfault in libcrypto).
        if (ws) {
            beast::get_lowest_layer(*ws).cancel();
            beast::error_code ec;
            beast::get_lowest_layer(*ws).socket().close(ec);
            ws.reset();
        }

        ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            ioc, ssl_ctx);

        resolver.async_resolve(host, port,
            [this](beast::error_code ec, tcp::resolver::results_type results) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error("resolve", ec);
                    return;
                }
                on_resolve(results);
            });
    }

    void on_resolve(tcp::resolver::results_type results) {
        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*ws).async_connect(results,
            [this](beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error("connect", ec);
                    return;
                }
                on_tcp_connect();
            });
    }

    void on_tcp_connect() {
        beast::get_lowest_layer(*ws).socket().set_option(tcp::no_delay(true));

        if (!SSL_set_tlsext_host_name(ws->next_layer().native_handle(), host.c_str())) {
            handle_error("ssl_sni", beast::error_code{});
            return;
        }

        beast::get_lowest_layer(*ws).expires_after(std::chrono::seconds(10));
        ws->next_layer().async_handshake(ssl::stream_base::client,
            [this](beast::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error("ssl_handshake", ec);
                    return;
                }
                on_ssl_handshake();
            });
    }

    void on_ssl_handshake() {
        beast::get_lowest_layer(*ws).expires_never();
        ws->read_message_max(1 * 1024 * 1024);  // 1MB limit (RTDS messages are small)

        ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws->set_option(websocket::stream_base::decorator([this](websocket::request_type& req) {
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "LiveTradingv2/1.0");
        }));

        ws->async_handshake(host, path,
            [this](beast::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error("ws_handshake", ec);
                    return;
                }
                on_ws_connected();
            });
    }

    void on_ws_connected() {
        reconnect_delay_ms = config.reconnect_base_ms;

        if (on_connected) {
            try {
                on_connected();
            } catch (...) {
                handle_error("on_connected", beast::error_code{});
                return;
            }
        }

        // Send subscribe for crypto_prices
        send_subscribe();

        start_stale_timer();
        do_read();
    }

    void send_subscribe() {
        ws->async_write(net::buffer(kSubscribeMsg, std::strlen(kSubscribeMsg)),
            [this](beast::error_code ec, std::size_t) {
                if (ec) {
                    handle_error("subscribe_write", ec);
                    return;
                }
                // Start ping timer after subscribe succeeds
                start_ping_timer();
            });
    }

    void start_ping_timer() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;

        ping_timer.expires_after(std::chrono::milliseconds(config.ping_interval_ms));
        ping_timer.async_wait([this](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            send_ping();
        });
    }

    void send_ping() {
        if (!ws || shutdown_requested.load(std::memory_order_relaxed)) return;

        ws->async_write(net::buffer(kPingMsg, 4),
            [this](beast::error_code ec, std::size_t) {
                if (ec) {
                    handle_error("ping_write", ec);
                    return;
                }
                start_ping_timer();
            });
    }

    void start_stale_timer() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (config.stale_threshold_ms <= 0) return;

        stale_timer.expires_after(std::chrono::milliseconds(10000));
        stale_timer.async_wait([this](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            auto now = SteadyClock::now();
            if (last_message_ts > 0 &&
                (now - last_message_ts) > config.stale_threshold_ms * 1'000'000LL) {
                handle_error("stale_watchdog", beast::error_code{});
                return;
            }
            start_stale_timer();
        });
    }

    void do_read() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;

        buffer.clear();
        ws->async_read(buffer, [this](beast::error_code ec, std::size_t bytes_transferred) {
            if (shutdown_requested.load(std::memory_order_relaxed)) return;

            if (ec) {
                handle_error("read", ec);
                return;
            }

            auto recv_ts = SteadyClock::now();
            last_message_ts = recv_ts;
            read_payload.resize(bytes_transferred);
            auto copied = net::buffer_copy(
                net::buffer(read_payload.data(), read_payload.size()), buffer.data());
            read_payload.resize(copied);

            if (on_message) {
                try {
                    on_message(
                        std::string_view(read_payload.data(), read_payload.size()), recv_ts);
                } catch (...) {
                    handle_error("on_message", beast::error_code{});
                    return;
                }
            }

            do_read();
        });
    }

    void handle_error(const char* where, beast::error_code ec) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (reconnecting_) return;

        // Cancel active timers to prevent duplicate timer chains after reconnect.
        ping_timer.cancel();
        stale_timer.cancel();

        std::string reason = std::string(where) + ": " + (ec ? ec.message() : "unknown error");

        if (on_disconnected) {
            try {
                on_disconnected(reason);
            } catch (...) {}
        }

        schedule_reconnect();
    }

    void schedule_reconnect() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        reconnecting_ = true;

        reconnect_timer.expires_after(std::chrono::milliseconds(reconnect_delay_ms));
        reconnect_timer.async_wait([this](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            start_connect();
        });

        reconnect_delay_ms = std::min(reconnect_delay_ms * 2, config.reconnect_max_ms);
    }

    void shutdown() {
        shutdown_requested.store(true, std::memory_order_relaxed);
        net::post(ioc, [this] {
            ping_timer.cancel();
            stale_timer.cancel();
            reconnect_timer.cancel();
            // Cancel any in-flight DNS resolve so the io_context destructor
            // doesn't block in win_iocp_io_context::shutdown() waiting on
            // the Windows resolver worker thread.
            resolver.cancel();

            if (ws) {
                beast::get_lowest_layer(*ws).cancel();
                ws->async_close(websocket::close_code::normal,
                    [](beast::error_code) {});
            }

            ioc.stop();
        });
    }
};

RtdsWsClient::RtdsWsClient(const RtdsWsConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

RtdsWsClient::~RtdsWsClient() = default;

void RtdsWsClient::set_on_message(RtdsOnMessageCb cb) { impl_->on_message = std::move(cb); }
void RtdsWsClient::set_on_connected(RtdsOnConnectedCb cb) { impl_->on_connected = std::move(cb); }
void RtdsWsClient::set_on_disconnected(RtdsOnDisconnectedCb cb) {
    impl_->on_disconnected = std::move(cb);
}

void RtdsWsClient::run() {
    impl_->start_connect();
    impl_->ioc.run();
}

void RtdsWsClient::request_shutdown() { impl_->shutdown(); }

}  // namespace lt
