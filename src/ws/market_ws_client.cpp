#include "ws/market_ws_client.h"

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <sstream>

#include "common/clock.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

namespace lt {

// O(1) hash-based dedup: fixed-size table indexed by hash % N.
// Collisions overwrite (approximate dedup — downstream has exact dedup).
struct MessageDedup {
    static constexpr int kSlots = 2048;
    uint64_t table[kSlots]{};

    bool is_duplicate(const char* data, std::size_t len) {
        uint64_t h = fnv1a_hash(data, len);
        int slot = static_cast<int>(h % kSlots);
        if (table[slot] == h) return true;
        table[slot] = h;
        return false;
    }
};

struct MarketWsClient::Impl {
    // Per-connection state
    struct Conn {
        int id;
        std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
        beast::flat_buffer buffer;
        std::string read_payload;
        std::string subscribe_payload;
        net::steady_timer ping_timer;
        net::steady_timer reconnect_timer;
        bool reconnecting = false;
        int64_t reconnect_delay_ms;
        Timestamp_ns last_message_ts = 0;
        std::vector<std::string> write_queue;
        bool writing = false;

        Conn(net::io_context& ioc, int id_, int64_t base_delay)
            : id(id_), ping_timer(ioc), reconnect_timer(ioc),
              reconnect_delay_ms(base_delay) {}
    };

    WsClientConfig config;
    net::io_context ioc;
    ssl::context ssl_ctx{ssl::context::tlsv12_client};
    tcp::resolver resolver{ioc};
    net::steady_timer stale_timer{ioc};

    OnMessageCb on_message;
    OnConnectedCb on_connected;
    OnDisconnectedCb on_disconnected;

    std::atomic<bool> shutdown_requested{false};
    int connected_count_ = 0;

    std::string host;
    std::string port;
    std::string path;

    // Redundant connections
    std::vector<std::unique_ptr<Conn>> conns_;
    MessageDedup dedup_;

    // Track dynamic subscriptions so reconnected connections catch up
    std::vector<std::string> active_subscriptions_;

    Impl(const WsClientConfig& cfg) : config(cfg) {
        auto url = config.endpoint;
        if (url.substr(0, 6) == "wss://") url = url.substr(6);
        else if (url.substr(0, 5) == "ws://") url = url.substr(5);

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

        int n = std::max(1, config.redundancy);
        for (int i = 0; i < n; ++i) {
            conns_.push_back(std::make_unique<Conn>(ioc, i, config.reconnect_base_ms));
        }
    }

    // --- Connection lifecycle (parameterized by Conn&) ---

    void start_connect(Conn& c) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        c.reconnecting = false;

        // Close previous connection to prevent in-flight async callbacks
        // from firing on a destroyed SSL stream (segfault in libcrypto).
        if (c.ws) {
            beast::get_lowest_layer(*c.ws).cancel();
            beast::error_code ec;
            beast::get_lowest_layer(*c.ws).socket().close(ec);
            c.ws.reset();
        }

        c.ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            ioc, ssl_ctx);

        resolver.async_resolve(host, port,
            [this, &c](beast::error_code ec, tcp::resolver::results_type results) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error(c, "resolve", ec);
                    return;
                }
                on_resolve(c, results);
            });
    }

    void on_resolve(Conn& c, tcp::resolver::results_type results) {
        beast::get_lowest_layer(*c.ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(*c.ws).async_connect(
            results, [this, &c](beast::error_code ec, tcp::resolver::results_type::endpoint_type) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error(c, "connect", ec);
                    return;
                }
                on_tcp_connect(c);
            });
    }

    void on_tcp_connect(Conn& c) {
        beast::get_lowest_layer(*c.ws).socket().set_option(tcp::no_delay(true));

        if (!SSL_set_tlsext_host_name(c.ws->next_layer().native_handle(), host.c_str())) {
            handle_error(c, "ssl_sni", beast::error_code{});
            return;
        }

        beast::get_lowest_layer(*c.ws).expires_after(std::chrono::seconds(10));
        c.ws->next_layer().async_handshake(
            ssl::stream_base::client, [this, &c](beast::error_code ec) {
                if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                    handle_error(c, "ssl_handshake", ec);
                    return;
                }
                on_ssl_handshake(c);
            });
    }

    void on_ssl_handshake(Conn& c) {
        beast::get_lowest_layer(*c.ws).expires_never();
        c.ws->read_message_max(16 * 1024 * 1024);

        c.ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        c.ws->set_option(websocket::stream_base::decorator([this](websocket::request_type& req) {
            req.set(http::field::host, host);
            req.set(http::field::user_agent, "LiveTradingv2/1.0");
        }));

        c.ws->async_handshake(host, path, [this, &c](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) {
                handle_error(c, "ws_handshake", ec);
                return;
            }
            on_ws_connected(c);
        });
    }

    void on_ws_connected(Conn& c) {
        c.reconnect_delay_ms = config.reconnect_base_ms;

        // Fire on_connected only for the first connection to succeed
        if (++connected_count_ == 1 && on_connected) {
            try { on_connected(); } catch (...) {}
        }

        send_subscribe(c);

        // Replay any dynamic subscriptions this connection missed (e.g., reconnect
        // after slot rotation added new tokens while this connection was down)
        if (!active_subscriptions_.empty()) {
            std::ostringstream ss;
            ss << R"({"assets_ids":[)";
            for (std::size_t i = 0; i < active_subscriptions_.size(); ++i) {
                if (i != 0) ss << ',';
                ss << '"' << active_subscriptions_[i] << '"';
            }
            ss << R"(],"operation":"subscribe","custom_feature_enabled":true})";
            conn_enqueue_write(c, ss.str());
        }

        if (connected_count_ == 1) start_stale_timer();
        do_read(c);
    }

    void send_subscribe(Conn& c) {
        if (config.asset_ids.empty()) {
            start_ping_timer(c);
            return;
        }

        std::ostringstream ss;
        ss << R"({"assets_ids":[)";
        for (std::size_t i = 0; i < config.asset_ids.size(); ++i) {
            if (i != 0) ss << ',';
            ss << '"' << config.asset_ids[i] << '"';
        }
        ss << R"(],"type":"market","custom_feature_enabled":true})";
        c.subscribe_payload = ss.str();

        c.ws->async_write(net::buffer(c.subscribe_payload),
            [this, &c](beast::error_code ec, std::size_t) {
                if (ec) { handle_error(c, "subscribe_write", ec); return; }
                start_ping_timer(c);
            });
    }

    void start_ping_timer(Conn& c) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        c.ping_timer.expires_after(std::chrono::milliseconds(config.ping_interval_ms));
        c.ping_timer.async_wait([this, &c](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            send_ping(c);
        });
    }

    void send_ping(Conn& c) {
        static constexpr char kPingPayload[] = "PING";
        if (!c.ws || shutdown_requested.load(std::memory_order_relaxed)) return;
        c.ws->async_write(net::buffer(kPingPayload, sizeof(kPingPayload) - 1),
            [this, &c](beast::error_code ec, std::size_t) {
                if (ec) { handle_error(c, "ping_write", ec); return; }
                start_ping_timer(c);
            });
    }

    void start_stale_timer() {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (config.stale_threshold_ms <= 0) return;
        stale_timer.expires_after(std::chrono::milliseconds(10000));
        stale_timer.async_wait([this](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            // Check if ANY connection has recent data
            auto now = SteadyClock::now();
            Timestamp_ns newest = 0;
            for (auto& c : conns_) {
                if (c->last_message_ts > newest) newest = c->last_message_ts;
            }
            if (newest > 0 && (now - newest) > config.stale_threshold_ms * 1'000'000LL) {
                // All connections stale — reconnect all
                for (auto& c : conns_) {
                    if (!c->reconnecting) handle_error(*c, "stale_watchdog", {});
                }
                return;
            }
            start_stale_timer();
        });
    }

    void do_read(Conn& c) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        c.buffer.clear();
        c.ws->async_read(c.buffer, [this, &c](beast::error_code ec, std::size_t bytes) {
            if (shutdown_requested.load(std::memory_order_relaxed)) return;
            if (ec) { handle_error(c, "read", ec); return; }

            auto recv_ts = SteadyClock::now();
            c.last_message_ts = recv_ts;

            c.read_payload.resize(bytes);
            net::buffer_copy(net::buffer(c.read_payload.data(), bytes), c.buffer.data());

            // Dedup: drop if another connection already delivered this message
            if (conns_.size() > 1 &&
                dedup_.is_duplicate(c.read_payload.data(), c.read_payload.size())) {
                do_read(c);
                return;
            }

            if (on_message) {
                try {
                    on_message(std::string_view(c.read_payload.data(), c.read_payload.size()),
                              recv_ts);
                } catch (...) {
                    handle_error(c, "on_message", {});
                    return;
                }
            }
            do_read(c);
        });
    }

    void handle_error(Conn& c, const char* where, beast::error_code ec) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        if (c.reconnecting) return;

        c.ping_timer.cancel();
        if (connected_count_ > 0) --connected_count_;

        // Only fire on_disconnected for connection 0 to avoid 20x metric noise.
        // Also fire if ALL connections are now down (total failure).
        if ((c.id == 0 || connected_count_ <= 0) && on_disconnected) {
            std::string reason = std::string(where) + "[" + std::to_string(c.id) + "]: " +
                                 (ec ? ec.message() : "unknown error");
            try { on_disconnected(reason); } catch (...) {}
        }

        schedule_reconnect(c);
    }

    void schedule_reconnect(Conn& c) {
        if (shutdown_requested.load(std::memory_order_relaxed)) return;
        c.reconnecting = true;
        c.reconnect_timer.expires_after(std::chrono::milliseconds(c.reconnect_delay_ms));
        c.reconnect_timer.async_wait([this, &c](beast::error_code ec) {
            if (ec || shutdown_requested.load(std::memory_order_relaxed)) return;
            start_connect(c);
        });
        c.reconnect_delay_ms = std::min(c.reconnect_delay_ms * 2, config.reconnect_max_ms);
    }

    // --- Write serialization per-connection ---

    void conn_enqueue_write(Conn& c, std::string payload) {
        c.write_queue.push_back(std::move(payload));
        if (!c.writing) conn_drain_write(c);
    }

    void conn_drain_write(Conn& c) {
        if (c.write_queue.empty() || !c.ws) {
            c.writing = false;
            return;
        }
        c.writing = true;
        std::string& msg = c.write_queue.front();
        c.ws->async_write(net::buffer(msg), [this, &c](beast::error_code ec, std::size_t) {
            c.write_queue.erase(c.write_queue.begin());
            if (ec) { c.writing = false; return; }
            conn_drain_write(c);
        });
    }

    // --- Dynamic subscribe/unsubscribe (sent to ALL connections) ---

    void do_subscribe_add(const std::vector<std::string>& token_ids) {
        if (token_ids.empty()) return;
        // Track for replay on reconnect
        for (const auto& id : token_ids) active_subscriptions_.push_back(id);

        std::ostringstream ss;
        ss << R"({"assets_ids":[)";
        for (std::size_t i = 0; i < token_ids.size(); ++i) {
            if (i != 0) ss << ',';
            ss << '"' << token_ids[i] << '"';
        }
        ss << R"(],"operation":"subscribe","custom_feature_enabled":true})";
        std::string payload = ss.str();
        for (auto& c : conns_) {
            if (c->ws && !shutdown_requested.load(std::memory_order_relaxed))
                conn_enqueue_write(*c, payload);
        }
    }

    void do_unsubscribe(const std::vector<std::string>& token_ids) {
        if (token_ids.empty()) return;
        std::ostringstream ss;
        ss << R"({"assets_ids":[)";
        for (std::size_t i = 0; i < token_ids.size(); ++i) {
            if (i != 0) ss << ',';
            ss << '"' << token_ids[i] << '"';
        }
        ss << R"(],"operation":"unsubscribe"})";
        std::string payload = ss.str();
        for (auto& c : conns_) {
            if (c->ws && !shutdown_requested.load(std::memory_order_relaxed))
                conn_enqueue_write(*c, payload);
        }
    }

    void shutdown() {
        shutdown_requested.store(true, std::memory_order_relaxed);
        net::post(ioc, [this] {
            stale_timer.cancel();
            // Cancel any in-flight DNS resolve so the io_context destructor
            // doesn't block in win_iocp_io_context::shutdown() waiting on
            // the Windows resolver worker thread.
            resolver.cancel();
            for (auto& c : conns_) {
                c->ping_timer.cancel();
                c->reconnect_timer.cancel();
                if (c->ws) {
                    beast::get_lowest_layer(*c->ws).cancel();
                    c->ws->async_close(websocket::close_code::normal, [](beast::error_code) {});
                }
            }
            ioc.stop();
        });
    }
};

MarketWsClient::MarketWsClient(const WsClientConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

MarketWsClient::~MarketWsClient() = default;

void MarketWsClient::set_on_message(OnMessageCb cb) { impl_->on_message = std::move(cb); }
void MarketWsClient::set_on_connected(OnConnectedCb cb) { impl_->on_connected = std::move(cb); }
void MarketWsClient::set_on_disconnected(OnDisconnectedCb cb) {
    impl_->on_disconnected = std::move(cb);
}

void MarketWsClient::run() {
    // Stagger connection starts
    for (std::size_t i = 0; i < impl_->conns_.size(); ++i) {
        auto& c = *impl_->conns_[i];
        if (i == 0) {
            impl_->start_connect(c);
        } else {
            auto stagger = std::make_shared<net::steady_timer>(impl_->ioc);
            stagger->expires_after(std::chrono::milliseconds(
                static_cast<int64_t>(i) * impl_->config.redundancy_stagger_ms));
            stagger->async_wait([this, &c, stagger](beast::error_code ec) {
                if (!ec) impl_->start_connect(c);
            });
        }
    }
    impl_->ioc.run();
}

void MarketWsClient::request_shutdown() { impl_->shutdown(); }

void MarketWsClient::send_subscribe_add(const std::vector<std::string>& token_ids) {
    auto ids = token_ids;
    net::post(impl_->ioc, [this, ids = std::move(ids)]() {
        impl_->do_subscribe_add(ids);
    });
}

void MarketWsClient::send_unsubscribe(const std::vector<std::string>& token_ids) {
    auto ids = token_ids;
    net::post(impl_->ioc, [this, ids = std::move(ids)]() {
        impl_->do_unsubscribe(ids);
    });
}

}  // namespace lt
