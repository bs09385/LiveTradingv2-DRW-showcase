#include "binance/binance_ws_client.h"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <cstring>

#include "common/clock.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace lt {

struct BinanceWsClient::Impl {
    BinanceWsConfig config;
    net::io_context ioc;
    ssl::context ssl_ctx{ssl::context::tlsv12_client};
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
    tcp::resolver resolver{ioc};
    beast::flat_buffer buffer;
    net::steady_timer reconnect_timer{ioc};
    net::steady_timer stale_timer{ioc};
    net::steady_timer rotate_timer{ioc};

    BinanceOnMessageCb on_message;
    BinanceOnConnectedCb on_connected;
    BinanceOnDisconnectedCb on_disconnected;
    BinanceOnStatusCb on_status;

    // T_binance_md owns these — strategy reads via getters from T2.
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> reconnecting_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int64_t> last_message_ts_ns_{0};

    int64_t reconnect_delay_ms = 1000;
    Timestamp_ns last_message_ts = 0;  // T_binance-only mirror used by stale watchdog

    std::string host;
    std::string port;
    std::string path;  // includes /stream?streams=...

    explicit Impl(const BinanceWsConfig& cfg) : config(cfg) {
        parse_endpoint(cfg.endpoint);
        ssl_ctx.set_verify_mode(ssl::verify_none);
        reconnect_delay_ms = std::max<int64_t>(1, config.reconnect_base_ms);
    }

    void parse_endpoint(const std::string& url_in) {
        std::string url = url_in;
        bool is_wss = (url.rfind("wss://", 0) == 0);
        if (is_wss) url = url.substr(6);
        else if (url.rfind("ws://", 0) == 0) url = url.substr(5);

        auto slash_pos = url.find('/');
        std::string hostport;
        if (slash_pos != std::string::npos) {
            hostport = url.substr(0, slash_pos);
            path = url.substr(slash_pos);
        } else {
            hostport = url;
            path = "/";
        }
        auto colon_pos = hostport.find(':');
        if (colon_pos != std::string::npos) {
            host = hostport.substr(0, colon_pos);
            port = hostport.substr(colon_pos + 1);
        } else {
            host = hostport;
            port = is_wss ? "443" : "80";
        }
    }

    void start_connect() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        reconnecting_.store(false, std::memory_order_relaxed);

        // Close and destroy the prior stream first. The async_read callback
        // for the old connection can otherwise fire on a destroyed SSL
        // context during rapid reconnect cycles.
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
            // Close and destroy the half-open stream before bailing — otherwise
            // it leaks until the next reconnect cycle picks it up.
            beast::error_code close_ec;
            beast::get_lowest_layer(*ws).socket().close(close_ec);
            ws.reset();
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
        // Binance combined-stream messages are modest; 16MB is a safe ceiling.
        ws->read_message_max(16 * 1024 * 1024);

        // Let Beast handle WS-protocol pings/pongs (Binance pings ~every 20s).
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
        reconnect_delay_ms = std::max<int64_t>(1, config.reconnect_base_ms);
        auto now = SteadyClock::now();
        last_message_ts = now;
        last_message_ts_ns_.store(now, std::memory_order_relaxed);

        // Emit CONNECTED sentinel exactly once per DOWN->UP transition.
        bool was_connected = connected_.exchange(true, std::memory_order_relaxed);
        if (!was_connected && on_status) {
            try { on_status(BinanceUpdateKind::CONNECTED, ""); } catch (...) {}
        }

        if (on_connected) {
            try { on_connected(); } catch (...) {
                handle_error("on_connected", beast::error_code{});
                return;
            }
        }

        start_stale_timer();
        start_rotate_timer();
        do_read();
    }

    void start_stale_timer() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (config.stale_threshold_ms <= 0) return;

        // Poll at half the staleness threshold so detection latency is bounded
        // to ~1.5x the configured window. Floor at 1s to avoid timer churn.
        int64_t poll_ms = std::max<int64_t>(1000, config.stale_threshold_ms / 2);
        stale_timer.expires_after(std::chrono::milliseconds(poll_ms));
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

    void start_rotate_timer() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (config.rotate_interval_ms <= 0) return;

        rotate_timer.expires_after(std::chrono::milliseconds(config.rotate_interval_ms));
        rotate_timer.async_wait([this](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            // Trigger a clean reconnect before Binance's 24h cutoff.
            handle_error("rotate", beast::error_code{});
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
            last_message_ts_ns_.store(recv_ts, std::memory_order_relaxed);

            if (on_message) {
                // flat_buffer's cdata() chunk is contiguous; pass a view directly
                // and skip the per-frame allocation+copy.
                auto chunk = buffer.cdata();
                std::string_view payload{
                    static_cast<const char*>(chunk.data()), bytes_transferred};
                on_message(payload, recv_ts);
            }
            do_read();
        });
    }

    void handle_error(const char* where, beast::error_code ec) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (reconnecting_.load(std::memory_order_relaxed)) return;

        stale_timer.cancel();
        rotate_timer.cancel();

        // Emit DISCONNECTED sentinel exactly once per UP->DOWN edge.
        bool was_connected = connected_.exchange(false, std::memory_order_relaxed);
        std::string reason = std::string(where) + ": " + (ec ? ec.message() : "planned");
        if (was_connected && on_status) {
            try { on_status(BinanceUpdateKind::DISCONNECTED, reason); } catch (...) {}
        }
        if (on_disconnected) {
            try { on_disconnected(reason); } catch (...) {}
        }
        schedule_reconnect();
    }

    void schedule_reconnect() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        reconnecting_.store(true, std::memory_order_relaxed);

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
            connected_.store(false, std::memory_order_relaxed);
            stale_timer.cancel();
            rotate_timer.cancel();
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

BinanceWsClient::BinanceWsClient(const BinanceWsConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}
BinanceWsClient::~BinanceWsClient() = default;

void BinanceWsClient::set_on_message(BinanceOnMessageCb cb) { impl_->on_message = std::move(cb); }
void BinanceWsClient::set_on_connected(BinanceOnConnectedCb cb) { impl_->on_connected = std::move(cb); }
void BinanceWsClient::set_on_disconnected(BinanceOnDisconnectedCb cb) {
    impl_->on_disconnected = std::move(cb);
}
void BinanceWsClient::set_on_status(BinanceOnStatusCb cb) {
    impl_->on_status = std::move(cb);
}

bool BinanceWsClient::is_connected() const noexcept {
    return impl_->connected_.load(std::memory_order_relaxed);
}

int64_t BinanceWsClient::last_message_ts_ns() const noexcept {
    return impl_->last_message_ts_ns_.load(std::memory_order_relaxed);
}

void BinanceWsClient::run() {
    impl_->start_connect();
    impl_->ioc.run();
}

void BinanceWsClient::request_shutdown() { impl_->shutdown(); }

}  // namespace lt
