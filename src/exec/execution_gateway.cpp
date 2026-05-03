#include "exec/execution_gateway.h"
#include "exec/inventory_safety.h"
#include "exec/order_builder.h"
#include "rest/rest_auth.h"
#include "rest/rest_client.h"
#include "rest/rest_transport.h"
#include "rest/rest_response_parser.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "common/clock.h"
#include "crypto/hex_utils.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>
#include <unordered_set>

// Windows wingdi.h defines ERROR as 0, which conflicts with LogLevel::ERROR
#ifdef ERROR
#undef ERROR
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include <string_view>
#include <vector>

namespace net = boost::asio;

namespace lt {

struct ExecutionGateway::Impl {
    SpscQueue<ExecIntent>& intent_queue_;
    SpscQueue<SchedulerEvent>& feedback_queue_;
    Metrics& metrics_;
    AsyncLogger& logger_;
    GatewayConfig config_;
    OrderSigner& signer_;
    std::atomic<bool>* fatal_flag_;
    TokenInventory* inventory_view_;

    net::io_context ioc_;
    net::steady_timer poll_timer_{ioc_};
    net::steady_timer heartbeat_timer_{ioc_};

    std::vector<std::unique_ptr<IRestTransport>> order_pool_;  // round-robin order connections
    uint32_t order_pool_next_ = 0;                            // round-robin index
    std::vector<std::unique_ptr<IRestTransport>> cancel_clients_;
    uint32_t cancel_next_ = 0;                                // round-robin for individual cancels
    std::unordered_set<uint32_t> cancel_responded_;  // dedup redundant cancel responses
    std::unique_ptr<RestAuth> rest_auth_;
    std::unique_ptr<OrderBuilder> order_builder_;
    RateLimiter rate_limiter_;
    HeartbeatManager heartbeat_mgr_;

    ProducerHandle log_handle_;
    std::atomic<bool> shutdown_requested_{false};
    bool running_ = false;

    // Round-robin next order connection from the pool.
    IRestTransport& next_order_client() {
        auto& c = *order_pool_[order_pool_next_ % order_pool_.size()];
        ++order_pool_next_;
        return c;
    }
    bool heartbeat_in_flight_ = false;  // guard against duplicate sends
    bool heartbeat_cancel_all_latched_ = false;  // one cancel-all per failure streak
    bool was_degraded_ = false;         // track degraded state transitions
    int64_t feedback_overflow_count_ = 0;  // total feedback queue overflows

    // Track in-flight SELL quantities per token to prevent overselling.
    // T3-owned, no thread safety needed.
    std::unordered_map<AssetId, Qty_t, AssetIdHash> inflight_sell_qty_;

    // Map intent_id -> (asset_id, qty) for in-flight SELLs, so we can
    // release the reservation when the response arrives.
    struct InflightSellEntry { AssetId asset_id; Qty_t qty; };
    std::unordered_map<uint32_t, InflightSellEntry> inflight_sell_intents_;
    bool feedback_overflow_escalated_ = false;
    bool feedback_overflow_fatal_ = false;
    static constexpr int64_t kFeedbackOverflowFatalThreshold = 1000;

    // Correlation map: intent_id -> metadata
    struct CorrelationEntry {
        OrderId client_order_id;
        ExecIntentType type;
        Timestamp_ns sent_ts = 0;
    };
    std::unordered_map<uint32_t, CorrelationEntry> correlation_;
    static constexpr size_t kMaxCorrelation = 1024;

    // Batch order accumulation buffer
    ExecIntent batch_buf_[kBatchMaxOrders]{};
    int batch_buf_count_ = 0;

    struct BatchEntry {
        uint32_t intent_id;
        OrderId client_oid;
    };

    Impl(SpscQueue<ExecIntent>& intent_queue,
         SpscQueue<SchedulerEvent>& feedback_queue,
         Metrics& metrics, AsyncLogger& logger,
         const GatewayConfig& config, OrderSigner& signer,
         std::atomic<bool>* fatal_flag,
         TokenInventory* inventory_view)
        : intent_queue_(intent_queue),
          feedback_queue_(feedback_queue),
          metrics_(metrics),
          logger_(logger),
          config_(config),
          signer_(signer),
          fatal_flag_(fatal_flag),
          inventory_view_(inventory_view),
          rate_limiter_(config.rate_limit),
          heartbeat_mgr_(config.heartbeat),
          log_handle_(logger.create_producer("T3-Gateway")) {

        // Create REST auth (reads env vars)
        try {
            bool has_explicit_auth =
                !config_.poly_api_key.empty() &&
                !config_.poly_api_secret_b64.empty() &&
                !config_.poly_api_passphrase.empty() &&
                !config_.poly_api_address.empty();
            if (has_explicit_auth) {
                rest_auth_ = std::make_unique<RestAuth>(
                    config_.poly_api_key,
                    config_.poly_api_secret_b64,
                    config_.poly_api_passphrase,
                    config_.poly_api_address);
            } else {
                rest_auth_ = std::make_unique<RestAuth>();
            }
        } catch (const std::exception& e) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "RestAuth init failed: %s", e.what());
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Create REST client (HTTP/1.1 or HTTP/2 based on config)
        RestClientConfig rest_cfg;
        rest_cfg.host = config_.rest_host;
        rest_cfg.port = config_.rest_port;
        rest_cfg.request_timeout_ms = config_.request_timeout_ms;
        rest_cfg.max_pipeline_depth = static_cast<size_t>(
            config_.rest_pipeline_depth > 0 ? config_.rest_pipeline_depth : 1);
        int pool_n = std::max(1, config_.order_connection_pool_size);
        for (int i = 0; i < pool_n; ++i) {
            order_pool_.push_back(make_rest_transport(ioc_, rest_cfg,
                config_.use_http2, config_.max_concurrent_streams));
        }

        // Create dedicated cancel connections (separate from order pipeline)
        int cancel_n = std::max(1, config_.cancel_connection_redundancy);
        for (int i = 0; i < cancel_n; ++i) {
            cancel_clients_.push_back(make_rest_transport(ioc_, rest_cfg,
                config_.use_http2, config_.max_concurrent_streams));
        }

        // Route H2 transport diagnostics into the engine log
        auto log_cb = [this](int level, const char* msg) {
            LogLevel lvl = (level >= 1) ? LogLevel::WARN : LogLevel::INFO;
            AsyncLogger::log(log_handle_, lvl, msg);
        };
        for (auto& oc : order_pool_) {
            oc->set_log_callback(log_cb);
        }
        for (auto& cc : cancel_clients_) {
            cc->set_log_callback(log_cb);
        }

        // Create order builder
        OrderBuilderConfig ob_cfg;
        ob_cfg.defer_exec = config_.defer_exec;
        ob_cfg.post_only = config_.post_only;
        ob_cfg.signature_type = config_.signature_type;
        order_builder_ = std::make_unique<OrderBuilder>(
            signer_, config_.owner_uuid,
            config_.maker_address, config_.signer_address, ob_cfg);
    }

    void run() {
        if (!rest_auth_ || order_pool_.empty() || !order_builder_) return;
        running_ = true;

        {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "ExecutionGateway starting, auth=%s, host=%s:%s",
                          rest_auth_->redacted_summary().c_str(),
                          config_.rest_host.c_str(), config_.rest_port.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }

        // Warm all connections eagerly — send GET /time so DNS + TCP + TLS
        // handshake completes before the first real order arrives.
        // Skipped by tests (skip_warmup=true) to avoid spawning Boost.Asio
        // resolver worker threads against unreachable hosts, which can pin
        // the io_context destructor in Windows IOCP shutdown when many tests
        // run in the same process.
        if (!config_.skip_warmup) {
            RestRequest warmup;
            warmup.method = HttpMethod::GET;
            warmup.path = "/time";
            warmup.created_ts = SteadyClock::now();
            auto warmup_headers = rest_auth_->build_headers(warmup.method, warmup.path, "");
            for (auto& oc : order_pool_) {
                oc->async_send(warmup, warmup_headers, [](RestResponse) {});
            }
            for (auto& cc : cancel_clients_) {
                cc->async_send(warmup, warmup_headers, [](RestResponse) {});
            }
        }

        // Start polling
        schedule_poll();
        schedule_heartbeat_check();

        // Run io_context (blocks until shutdown)
        ioc_.run();
        running_ = false;

        AsyncLogger::log(log_handle_, LogLevel::INFO, "ExecutionGateway stopped");
    }

    void request_shutdown() {
        shutdown_requested_.store(true, std::memory_order_relaxed);
        net::post(ioc_, [this]() {
            poll_timer_.cancel();
            heartbeat_timer_.cancel();
            for (auto& oc : order_pool_) oc->request_shutdown();
            for (auto& cc : cancel_clients_) cc->request_shutdown();
            ioc_.stop();
        });
    }

    void schedule_poll() {
        if (shutdown_requested_.load(std::memory_order_relaxed)) return;

        poll_timer_.expires_after(std::chrono::microseconds(100));
        poll_timer_.async_wait([this](boost::system::error_code ec) {
            if (ec || shutdown_requested_.load(std::memory_order_relaxed)) return;
            poll_intents();
            schedule_poll();
        });
    }

    void schedule_heartbeat_check() {
        if (shutdown_requested_.load(std::memory_order_relaxed)) return;

        heartbeat_timer_.expires_after(std::chrono::milliseconds(100));
        heartbeat_timer_.async_wait([this](boost::system::error_code ec) {
            if (ec || shutdown_requested_.load(std::memory_order_relaxed)) return;
            check_heartbeat();
            schedule_heartbeat_check();
        });
    }

    static bool is_place_intent(ExecIntentType t) {
        return t == ExecIntentType::PLACE_ORDER || t == ExecIntentType::REPLACE_ORDER;
    }

    int effective_batch_max() const {
        return std::min(config_.batch_max_size, kBatchMaxOrders);
    }

    void poll_intents() {
        // Drain up to 16 intents per poll cycle
        Timestamp_ns now = SteadyClock::now();
        for (int i = 0; i < 16; ++i) {
            auto* front = intent_queue_.front();
            if (!front) break;

            ExecIntent intent = *front;
            intent_queue_.pop();

            if (config_.batch_orders_enabled && is_place_intent(intent.type)) {
                // Record latency metrics before accumulating
                if (intent.created_ts > 0) {
                    metrics_.record_latency(MetricId::EXEC_ENQUEUE_TO_SEND_NS,
                                            MetricId::EXEC_ENQUEUE_TO_SEND_COUNT,
                                            now - intent.created_ts);
                }
                if (intent.recv_ts > 0) {
                    auto pipeline_ns = now - intent.recv_ts;
                    metrics_.record_latency(MetricId::FULL_PIPELINE_NS,
                                            MetricId::FULL_PIPELINE_COUNT,
                                            pipeline_ns);
                    metrics_.tracker(LatencyTrackerId::FULL_PIPELINE).record(pipeline_ns);
                }

                // Rate-limit check before accumulating
                if (!rate_limiter_.try_acquire(intent.type, now)) {
                    publish_feedback(ExecFeedbackKind::RATE_LIMITED, intent, 0, 0);
                    metrics_.inc(MetricId::EXEC_RATE_THROTTLED);
                    continue;
                }
                batch_buf_[batch_buf_count_++] = intent;
                if (batch_buf_count_ >= effective_batch_max()) {
                    flush_order_batch();
                }
            } else {
                process_intent(intent);
            }
        }
        if (batch_buf_count_ > 0) flush_order_batch();
    }

    void check_degraded_transition() {
        bool now_degraded = rate_limiter_.is_degraded() ||
                            rate_limiter_.is_exchange_unavailable() ||
                            heartbeat_mgr_.is_failed();
        if (now_degraded && !was_degraded_) {
            was_degraded_ = true;
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::GATEWAY_DEGRADED;
            fb.created_ts = SteadyClock::now();
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_GATEWAY_DEGRADED_COUNT);
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                           "Gateway entering DEGRADED state");
        } else if (!now_degraded && was_degraded_) {
            was_degraded_ = false;
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::GATEWAY_RECOVERED;
            fb.created_ts = SteadyClock::now();
            publish_feedback_raw(fb);
            AsyncLogger::log(log_handle_, LogLevel::INFO,
                           "Gateway RECOVERED from degraded state");
        }
    }

    void process_intent(const ExecIntent& intent) {
        Timestamp_ns now = SteadyClock::now();

        // M7: Record enqueue-to-send and full pipeline latency
        if (intent.created_ts > 0) {
            metrics_.record_latency(MetricId::EXEC_ENQUEUE_TO_SEND_NS,
                                    MetricId::EXEC_ENQUEUE_TO_SEND_COUNT,
                                    now - intent.created_ts);
        }
        if (intent.recv_ts > 0) {
            auto pipeline_ns = now - intent.recv_ts;
            metrics_.record_latency(MetricId::FULL_PIPELINE_NS,
                                    MetricId::FULL_PIPELINE_COUNT,
                                    pipeline_ns);
            metrics_.tracker(LatencyTrackerId::FULL_PIPELINE).record(pipeline_ns);
        }

        if (!rate_limiter_.try_acquire(intent.type, now)) {
            publish_feedback(ExecFeedbackKind::RATE_LIMITED, intent, 0, 0);
            metrics_.inc(MetricId::EXEC_RATE_THROTTLED);
            return;
        }

        switch (intent.type) {
            case ExecIntentType::PLACE_ORDER:
            case ExecIntentType::REPLACE_ORDER:
                send_place_order(intent);
                break;
            case ExecIntentType::CANCEL_ORDER:
                send_cancel_order(intent);
                break;
            case ExecIntentType::CANCEL_ALL:
                send_cancel_all(intent);
                break;
            case ExecIntentType::HEARTBEAT:
                send_heartbeat();
                break;
        }
    }

    void send_place_order(const ExecIntent& intent) {
        // Hard safety gate: local inventory must cover SELL quantity
        // (including in-flight SELLs not yet filled/rejected).
        if (intent.side == Side::ASK) {
            auto inv_check = check_inventory_for_intent(intent, inventory_view_);
            Qty_t inflight = inflight_sell_qty_[intent.asset_id];
            Qty_t effective_available = inv_check.available - inflight;
            if (effective_available < intent.size) {
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                              "Reject SELL intent %u: insufficient local inventory "
                              "(asset=%.*s need=%lld have=%lld inflight=%lld)",
                              intent.intent_id, static_cast<int>(intent.asset_id.len),
                              intent.asset_id.data, static_cast<long long>(intent.size),
                              static_cast<long long>(inv_check.available),
                              static_cast<long long>(inflight));
                AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = intent.intent_id;
                fb.client_order_id = intent.client_order_id;
                fb.created_ts = SteadyClock::now();
                fb.set_error("insufficient local inventory for SELL");
                metrics_.inc(MetricId::EXEC_LOCAL_INVENTORY_REJECTS);
                publish_feedback_raw(fb);
                return;
            }
            // Track this SELL as in-flight
            inflight_sell_qty_[intent.asset_id] += intent.size;
            inflight_sell_intents_[intent.intent_id] = {intent.asset_id, intent.size};
        }

        auto result = order_builder_->build(intent);
        if (!result.ok()) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf), "OrderBuilder::build failed for intent %u",
                         intent.intent_id);
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
            publish_feedback(ExecFeedbackKind::ORDER_REJECTED, intent, 0, 0);
            return;
        }

        // Log order payload with EIP-712 debug info
        {
            const auto& body = result.value.json_body;
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "ORDER_DBG intent=%u len=%zu signing_hash=%s",
                intent.intent_id, body.size(),
                result.value.debug_signing_hash.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            std::snprintf(buf, sizeof(buf),
                "ORDER_DBG body[0:200]=%.200s",
                body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            if (body.size() > 200) {
                std::snprintf(buf, sizeof(buf),
                    "ORDER_DBG body[200:430]=%.230s",
                    body.c_str() + 200);
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            }
            if (body.size() > 430) {
                std::snprintf(buf, sizeof(buf),
                    "ORDER_DBG body[430:]=%.230s",
                    body.c_str() + 430);
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            }
        }

        // Pre-register predicted exchange order ID BEFORE sending REST request.
        // FAK orders match instantly — the user WS trade event arrives at T1
        // before the REST response reaches T3. Without pre-registration, T1
        // can't identify us as taker, inverts the fill side, and causes a
        // cascading position error.
        if (inventory_view_) {
            inventory_view_->register_our_order(result.value.predicted_exchange_oid);
        }

        // RestRequest::body allocates per REST send. Acceptable since REST calls are
        // rate-limited (~900/10s max). No allocation-free optimization needed here.
        RestRequest req;
        req.method = HttpMethod::POST;
        req.path = "/order";
        req.body = result.value.json_body;
        req.created_ts = SteadyClock::now();

        auto headers = rest_auth_->build_headers(req.method, req.path, req.body);

        // Store correlation
        if (correlation_.size() < kMaxCorrelation) {
            correlation_[intent.intent_id] = {
                intent.client_order_id, intent.type, req.created_ts};
        }

        publish_feedback(ExecFeedbackKind::REQUEST_SENT, intent, 0, 0);
        metrics_.inc(MetricId::EXEC_REST_REQUESTS_ORDER);

        uint32_t intent_id = intent.intent_id;
        OrderId client_oid = intent.client_order_id;

        next_order_client().async_send(req, headers,
            [this, intent_id, client_oid](RestResponse resp) {
                on_order_response(intent_id, client_oid, resp);
            });
    }

    void on_order_response(uint32_t intent_id, OrderId client_oid,
                           const RestResponse& resp) {
        Timestamp_ns now = SteadyClock::now();
        rate_limiter_.record_response(resp.http_status, now);
        rate_limiter_.record_rtt(resp.latency_ns);
        check_degraded_transition();
        metrics_.record_latency(MetricId::EXEC_RTT_NS, MetricId::EXEC_RTT_COUNT,
                               resp.latency_ns);
        metrics_.record_latency(MetricId::EXEC_ORDER_RTT_NS, MetricId::EXEC_ORDER_RTT_COUNT,
                               resp.latency_ns);
        metrics_.tracker(LatencyTrackerId::ORDER_RTT).record(resp.latency_ns);

        // Remove correlation
        correlation_.erase(intent_id);

        if (resp.timed_out || resp.http_status == 0) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /order timeout intent=%u http=%d lat=%.1fms",
                intent_id, resp.http_status,
                static_cast<double>(resp.latency_ns) / 1e6);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::TIMEOUT;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.latency_ns = resp.latency_ns;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_TIMEOUT_COUNT);
            metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            return;
        }

        if (resp.http_status == 429) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::RATE_LIMITED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 429;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_429);
            return;
        }

        if (resp.http_status == 503) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::EXCHANGE_UNAVAILABLE;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 503;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_503);
            return;
        }

        if (resp.http_status == 425) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /order matching engine restarting (425) intent=%u",
                intent_id);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::RATE_LIMITED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 425;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_425);
            if (rate_limiter_.is_matching_engine_down()) {
                AsyncLogger::log(log_handle_, LogLevel::FATAL,
                    "Matching engine down: 10 consecutive 425 responses -- fatal");
                if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Auth failure — credentials are invalid/expired/revoked.
        // Stop trading immediately to prevent repeated failures.
        if (resp.http_status == 401 || resp.http_status == 403) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "FATAL: REST auth failure on POST /order: http=%d body=%.200s",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("auth failure (401/403)");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            metrics_.inc(MetricId::EXEC_HTTP_4XX);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Non-2xx errors (4xx other than 429, 5xx other than 503)
        if (resp.http_status < 200 || resp.http_status >= 300) {
            // 502/504 are gateway errors — request may have been processed
            bool ambiguous = (resp.http_status == 502 || resp.http_status == 504);

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /order %s intent=%u http=%d body=%.200s",
                ambiguous ? "AMBIGUOUS" : "REJECTED",
                intent_id, resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ambiguous ? ExecFeedbackKind::TIMEOUT : ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("place request failed (non-2xx)");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            if (ambiguous) metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            if (resp.http_status >= 400 && resp.http_status < 500)
                metrics_.inc(MetricId::EXEC_HTTP_4XX);
            else if (resp.http_status >= 500)
                metrics_.inc(MetricId::EXEC_HTTP_5XX);
            return;
        }

        // Parse response (2xx only from here)
        auto parsed = parse_order_response(resp.body);
        if (parsed.ok() && parsed.value.success) {
            // Validate orderID is non-empty: success=true with empty orderID
            // means T2 can't track this order for fills or cancellation.
            if (parsed.value.order_id.empty()) {
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                    "REST: POST /order AMBIGUOUS intent=%u success=true but empty orderID "
                    "http=%d lat=%.1fms",
                    intent_id, resp.http_status,
                    static_cast<double>(resp.latency_ns) / 1e6);
                AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = intent_id;
                fb.client_order_id = client_oid;
                fb.http_status = resp.http_status;
                fb.set_error("success=true but empty orderID");
                publish_feedback_raw(fb);
                metrics_.inc(MetricId::EXEC_REST_ERRORS);
                metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
                return;
            }

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /order ACCEPTED intent=%u oid=%.60s http=%d lat=%.1fms",
                intent_id, parsed.value.order_id.c_str(),
                resp.http_status,
                static_cast<double>(resp.latency_ns) / 1e6);
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_ACCEPTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.exchange_order_id = OrderId(parsed.value.order_id);
            fb.http_status = resp.http_status;
            fb.latency_ns = resp.latency_ns;
            // Register our order ID for trade side resolution (T1 reads this)
            if (inventory_view_) {
                inventory_view_->register_our_order(fb.exchange_order_id);
            }
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_SUCCESS);
            metrics_.inc(MetricId::EXEC_CORRELATION_MATCHED);
        } else {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /order REJECTED intent=%u http=%d body=%.180s",
                intent_id, resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            if (parsed.ok()) {
                fb.set_error(parsed.value.error_msg.c_str());
            }
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
        }
    }

    void flush_order_batch() {
        if (batch_buf_count_ == 0) return;
        if (batch_buf_count_ == 1) {
            // Single order: use existing send_place_order path (skip rate check,
            // already done in poll_intents)
            send_place_order(batch_buf_[0]);
        } else {
            send_place_order_batch(batch_buf_, batch_buf_count_);
        }
        batch_buf_count_ = 0;
    }

    void send_place_order_batch(const ExecIntent* intents, int count) {
        // Inventory check per order — reject SELL orders with insufficient inventory
        // (including in-flight SELLs not yet filled/rejected), collect valid intents.
        ExecIntent valid[kBatchMaxOrders];
        int valid_count = 0;
        for (int i = 0; i < count; ++i) {
            if (intents[i].side == Side::ASK) {
                auto inv_check = check_inventory_for_intent(intents[i], inventory_view_);
                Qty_t inflight = inflight_sell_qty_[intents[i].asset_id];
                Qty_t effective_available = (inv_check.available > inflight)
                    ? inv_check.available - inflight : 0;
                if (effective_available < intents[i].size) {
                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "Reject SELL intent %u (batch): insufficient local inventory "
                                  "(asset=%.*s need=%lld have=%lld inflight=%lld)",
                                  intents[i].intent_id,
                                  static_cast<int>(intents[i].asset_id.len),
                                  intents[i].asset_id.data,
                                  static_cast<long long>(intents[i].size),
                                  static_cast<long long>(inv_check.available),
                                  static_cast<long long>(inflight));
                    AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

                    ExecFeedback fb;
                    fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                    fb.intent_id = intents[i].intent_id;
                    fb.client_order_id = intents[i].client_order_id;
                    fb.created_ts = SteadyClock::now();
                    fb.set_error("insufficient local inventory for SELL");
                    metrics_.inc(MetricId::EXEC_LOCAL_INVENTORY_REJECTS);
                    publish_feedback_raw(fb);
                    continue;
                }
            }
            valid[valid_count++] = intents[i];
        }

        if (valid_count == 0) return;

        // Fall back to single order path if only one remains after filtering
        if (valid_count == 1) {
            send_place_order(valid[0]);
            return;
        }

        // Track batch SELLs as in-flight to prevent overselling if another
        // batch arrives before response. Released in publish_feedback_raw()
        // when terminal feedback (accepted/rejected/timeout) arrives.
        for (int i = 0; i < valid_count; ++i) {
            if (valid[i].side == Side::ASK) {
                inflight_sell_qty_[valid[i].asset_id] += valid[i].size;
                inflight_sell_intents_[valid[i].intent_id] = {valid[i].asset_id, valid[i].size};
            }
        }

        auto result = order_builder_->build_batch(valid, valid_count);
        if (!result.ok()) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                         "OrderBuilder::build_batch failed for %d intents", valid_count);
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
            for (int i = 0; i < valid_count; ++i) {
                publish_feedback(ExecFeedbackKind::ORDER_REJECTED, valid[i], 0, 0);
            }
            return;
        }

        // Pre-register all predicted exchange order IDs before REST send
        // (same rationale as single-order path — FAK race prevention)
        if (inventory_view_) {
            for (int i = 0; i < valid_count; ++i) {
                inventory_view_->register_our_order(
                    result.value.predicted_exchange_oids[i]);
            }
        }

        RestRequest req;
        req.method = HttpMethod::POST;
        req.path = "/orders";
        req.body = std::move(result.value.json_body);
        req.created_ts = SteadyClock::now();

        auto headers = rest_auth_->build_headers(req.method, req.path, req.body);

        // Build correlation list captured by lambda
        std::vector<BatchEntry> entries;
        entries.reserve(static_cast<size_t>(valid_count));
        for (int i = 0; i < valid_count; ++i) {
            entries.push_back({valid[i].intent_id, result.value.client_order_ids[i]});
            publish_feedback(ExecFeedbackKind::REQUEST_SENT, valid[i], 0, 0);
        }

        metrics_.inc(MetricId::EXEC_REST_REQUESTS_BATCH);
        metrics_.add(MetricId::EXEC_BATCH_ORDERS_SENT, valid_count);

        {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                         "Sending batch order: %d orders via POST /orders", valid_count);
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }

        next_order_client().async_send(req, headers,
            [this, entries = std::move(entries)](RestResponse resp) {
                on_batch_order_response(entries, resp);
            });
    }

    void on_batch_order_response(
            const std::vector<BatchEntry>& entries,
            const RestResponse& resp) {
        Timestamp_ns now = SteadyClock::now();
        rate_limiter_.record_response(resp.http_status, now);
        rate_limiter_.record_rtt(resp.latency_ns);
        check_degraded_transition();
        metrics_.record_latency(MetricId::EXEC_RTT_NS, MetricId::EXEC_RTT_COUNT,
                               resp.latency_ns);
        metrics_.record_latency(MetricId::EXEC_ORDER_RTT_NS, MetricId::EXEC_ORDER_RTT_COUNT,
                               resp.latency_ns);
        metrics_.tracker(LatencyTrackerId::ORDER_RTT).record(resp.latency_ns);

        // Timeout or connection error — all orders ambiguous
        if (resp.timed_out || resp.http_status == 0) {
            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::TIMEOUT;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = resp.http_status;
                fb.latency_ns = resp.latency_ns;
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_TIMEOUT_COUNT);
            metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            return;
        }

        if (resp.http_status == 429) {
            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::RATE_LIMITED;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = 429;
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_HTTP_429);
            return;
        }

        if (resp.http_status == 503) {
            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::EXCHANGE_UNAVAILABLE;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = 503;
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_HTTP_503);
            return;
        }

        if (resp.http_status == 425) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /orders matching engine restarting (425) batch_size=%zu",
                entries.size());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::RATE_LIMITED;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = 425;
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_HTTP_425);
            if (rate_limiter_.is_matching_engine_down()) {
                AsyncLogger::log(log_handle_, LogLevel::FATAL,
                    "Matching engine down: 10 consecutive 425 responses -- fatal");
                if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Auth failure — stop trading immediately
        if (resp.http_status == 401 || resp.http_status == 403) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "FATAL: REST auth failure on POST /orders: http=%d body=%.200s",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);

            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = resp.http_status;
                fb.set_error("auth failure (401/403)");
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            metrics_.inc(MetricId::EXEC_HTTP_4XX);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Non-2xx errors
        if (resp.http_status < 200 || resp.http_status >= 300) {
            bool ambiguous = (resp.http_status == 502 || resp.http_status == 504);

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /orders %s http=%d body=%.200s",
                ambiguous ? "AMBIGUOUS" : "REJECTED",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ambiguous ? ExecFeedbackKind::TIMEOUT
                                    : ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = resp.http_status;
                fb.set_error("batch request failed (non-2xx)");
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            if (ambiguous) metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            if (resp.http_status >= 400 && resp.http_status < 500)
                metrics_.inc(MetricId::EXEC_HTTP_4XX);
            else if (resp.http_status >= 500)
                metrics_.inc(MetricId::EXEC_HTTP_5XX);
            return;
        }

        // Log raw batch response for debugging empty-orderID issues
        {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                         "REST: POST /orders raw response http=%d body=%.200s",
                         resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }

        // Parse batch response array (2xx only from here)
        auto parsed = parse_batch_order_response(resp.body);
        if (!parsed.ok()) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                         "Batch response parse failed (status=%d)", resp.http_status);
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
            for (const auto& e : entries) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = e.intent_id;
                fb.client_order_id = e.client_oid;
                fb.http_status = resp.http_status;
                fb.set_error("batch response parse failed");
                publish_feedback_raw(fb);
            }
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            return;
        }

        // Log warning if response array size doesn't match request
        if (parsed.value.items.size() != entries.size()) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: POST /orders response size mismatch: sent=%zu got=%zu",
                entries.size(), parsed.value.items.size());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        }

        // Process per-order results, matched by index
        size_t result_count = std::min(parsed.value.items.size(), entries.size());
        for (size_t i = 0; i < result_count; ++i) {
            const auto& item = parsed.value.items[i];
            const auto& entry = entries[i];

            if (item.success) {
                // Validate orderID non-empty on success
                if (item.order_id.empty()) {
                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                        "REST: POST /orders[%zu] AMBIGUOUS success=true but empty orderID raw=%.150s",
                        i, item.error_msg.c_str());
                    AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

                    ExecFeedback fb;
                    fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                    fb.intent_id = entry.intent_id;
                    fb.client_order_id = entry.client_oid;
                    fb.http_status = resp.http_status;
                    fb.set_error("success=true but empty orderID (batch)");
                    publish_feedback_raw(fb);
                    metrics_.inc(MetricId::EXEC_REST_ERRORS);
                    metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
                    continue;
                }

                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_ACCEPTED;
                fb.intent_id = entry.intent_id;
                fb.client_order_id = entry.client_oid;
                fb.exchange_order_id = OrderId(item.order_id);
                fb.http_status = resp.http_status;
                fb.latency_ns = resp.latency_ns;
                if (inventory_view_) {
                    inventory_view_->register_our_order(fb.exchange_order_id);
                }
                publish_feedback_raw(fb);
                metrics_.inc(MetricId::EXEC_REST_SUCCESS);
                metrics_.inc(MetricId::EXEC_CORRELATION_MATCHED);
            } else {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = entry.intent_id;
                fb.client_order_id = entry.client_oid;
                fb.http_status = resp.http_status;
                fb.set_error(item.error_msg.c_str());
                publish_feedback_raw(fb);
                metrics_.inc(MetricId::EXEC_REST_ERRORS);
            }
        }

        // If response has fewer items than we sent, reject the remainder
        for (size_t i = result_count; i < entries.size(); ++i) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = entries[i].intent_id;
            fb.client_order_id = entries[i].client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("batch response missing entry");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
        }
    }

    static void append_json_escaped(std::string& out, std::string_view s) {
        for (char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
    }

    void send_cancel_order(const ExecIntent& intent) {
        // Validate exchange_order_id is populated
        if (intent.exchange_order_id.view().empty()) {
            AsyncLogger::log(log_handle_, LogLevel::ERROR,
                           "Cancel intent has empty exchange_order_id");
            publish_feedback(ExecFeedbackKind::ORDER_REJECTED, intent, 0, 0);
            return;
        }

        // Build cancel request
        std::string body;
        body.reserve(intent.exchange_order_id.view().size() + 32);
        body += R"({"orderID":")";
        append_json_escaped(body, intent.exchange_order_id.view());
        body += R"("})";

        RestRequest req;
        req.method = HttpMethod::DELETE_METHOD;
        req.path = "/order";
        req.body = std::move(body);
        req.created_ts = SteadyClock::now();

        auto headers = rest_auth_->build_headers(req.method, req.path, req.body);

        metrics_.inc(MetricId::EXEC_REST_REQUESTS_CANCEL);
        publish_feedback(ExecFeedbackKind::REQUEST_SENT, intent, 0, 0);

        uint32_t intent_id = intent.intent_id;
        OrderId client_oid = intent.client_order_id;
        OrderId exchange_oid = intent.exchange_order_id;

        // Send cancel on one connection (round-robin). Broadcasting every
        // cancel to all N connections causes N× rate limit consumption.
        auto& cc = *cancel_clients_[cancel_next_ % cancel_clients_.size()];
        ++cancel_next_;
        cc.async_send(req, headers,
            [this, intent_id, client_oid, exchange_oid](RestResponse resp) {
                on_cancel_response(intent_id, client_oid, exchange_oid, resp);
            });
    }

    void on_cancel_response(uint32_t intent_id, OrderId client_oid,
                            OrderId exchange_oid, const RestResponse& resp) {
        // Dedup redundant cancel responses: first response wins, rest ignored
        if (cancel_clients_.size() > 1) {
            if (cancel_responded_.count(intent_id)) return;
            cancel_responded_.insert(intent_id);
        }

        Timestamp_ns now = SteadyClock::now();
        rate_limiter_.record_response(resp.http_status, now);
        rate_limiter_.record_rtt(resp.latency_ns);
        check_degraded_transition();
        metrics_.record_latency(MetricId::EXEC_RTT_NS, MetricId::EXEC_RTT_COUNT,
                               resp.latency_ns);
        metrics_.record_latency(MetricId::EXEC_CANCEL_RTT_NS, MetricId::EXEC_CANCEL_RTT_COUNT,
                               resp.latency_ns);
        metrics_.tracker(LatencyTrackerId::CANCEL_RTT).record(resp.latency_ns);

        if (resp.timed_out || resp.http_status == 0) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::TIMEOUT;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_TIMEOUT_COUNT);
            metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            return;
        }

        if (resp.http_status == 429) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::RATE_LIMITED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 429;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_429);
            return;
        }

        if (resp.http_status == 503) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::EXCHANGE_UNAVAILABLE;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 503;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_503);
            return;
        }

        if (resp.http_status == 425) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: cancel matching engine restarting (425) intent=%u",
                intent_id);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::RATE_LIMITED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = 425;
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_HTTP_425);
            if (rate_limiter_.is_matching_engine_down()) {
                AsyncLogger::log(log_handle_, LogLevel::FATAL,
                    "Matching engine down: 10 consecutive 425 responses -- fatal");
                if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Auth failure — stop trading immediately
        if (resp.http_status == 401 || resp.http_status == 403) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "FATAL: REST auth failure on cancel: http=%d body=%.200s",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);

            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("auth failure (401/403)");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            metrics_.inc(MetricId::EXEC_HTTP_4XX);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Non-2xx is a failure
        if (resp.http_status < 200 || resp.http_status >= 300) {
            // 502/504 are gateway errors — cancel may or may not have been processed
            bool ambiguous = (resp.http_status == 502 || resp.http_status == 504);

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: cancel %s http=%d body=%.200s",
                ambiguous ? "AMBIGUOUS" : "REJECTED",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = ambiguous ? ExecFeedbackKind::TIMEOUT
                                : ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("cancel request failed (non-2xx)");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            if (ambiguous) metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            if (resp.http_status >= 400 && resp.http_status < 500)
                metrics_.inc(MetricId::EXEC_HTTP_4XX);
            else if (resp.http_status >= 500)
                metrics_.inc(MetricId::EXEC_HTTP_5XX);
            return;
        }

        // Parse cancel response body — strict validation
        auto parsed = parse_cancel_response(resp.body);

        // 1. Parse error on 2xx body is ambiguous — reject, not confirm
        if (!parsed.ok()) {
            ExecFeedback fb;
            fb.kind = ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.http_status = resp.http_status;
            fb.set_error("cancel response parse failed on 2xx body");
            publish_feedback_raw(fb);
            metrics_.inc(MetricId::EXEC_REST_ERRORS);
            metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
            return;
        }

        // 2. not_canceled non-empty means at least partial failure
        if (!parsed.value.not_canceled.empty()) {
            const auto& first = parsed.value.not_canceled[0];
            char err_buf[128];
            std::snprintf(err_buf, sizeof(err_buf),
                "cancel failed: %.40s: %.40s",
                first.first.c_str(), first.second.c_str());

            // "already canceled" / "matched" means the order IS gone from the
            // exchange. Treat as CANCEL_CONFIRMED so the tracker removes it,
            // rather than ORDER_REJECTED which would leave it as phantom working.
            bool order_is_gone = (first.second.find("already canceled") != std::string::npos ||
                                  first.second.find("matched orders") != std::string::npos);

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: cancel %s intent=%u not_canceled=%zu reason=%s",
                order_is_gone ? "CONFIRMED(gone)" : "REJECTED",
                intent_id, parsed.value.not_canceled.size(), err_buf);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);

            ExecFeedback fb;
            fb.kind = order_is_gone ? ExecFeedbackKind::CANCEL_CONFIRMED
                                    : ExecFeedbackKind::ORDER_REJECTED;
            fb.intent_id = intent_id;
            fb.client_order_id = client_oid;
            fb.exchange_order_id = exchange_oid;
            fb.http_status = resp.http_status;
            fb.set_error(err_buf);
            publish_feedback_raw(fb);
            if (!order_is_gone) metrics_.inc(MetricId::EXEC_REST_ERRORS);
            return;
        }

        // 3. For single cancel: verify target order ID is in canceled array
        if (!exchange_oid.view().empty()) {
            bool found = false;
            for (const auto& cid : parsed.value.canceled) {
                if (cid == std::string(exchange_oid.view())) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                ExecFeedback fb;
                fb.kind = ExecFeedbackKind::ORDER_REJECTED;
                fb.intent_id = intent_id;
                fb.client_order_id = client_oid;
                fb.http_status = resp.http_status;
                fb.set_error("cancel target not found in canceled array");
                publish_feedback_raw(fb);
                metrics_.inc(MetricId::EXEC_REST_ERRORS);
                metrics_.inc(MetricId::EXEC_AMBIGUOUS_COUNT);
                return;
            }
        }

        // All checks passed — confirmed
        ExecFeedback fb;
        fb.kind = ExecFeedbackKind::CANCEL_CONFIRMED;
        fb.intent_id = intent_id;
        fb.client_order_id = client_oid;
        fb.http_status = resp.http_status;
        fb.latency_ns = resp.latency_ns;
        publish_feedback_raw(fb);
        metrics_.inc(MetricId::EXEC_REST_SUCCESS);
    }

    void send_cancel_all(const ExecIntent& intent) {
        RestRequest req;
        req.method = HttpMethod::DELETE_METHOD;
        req.path = "/cancel-all";
        req.created_ts = SteadyClock::now();

        auto headers = rest_auth_->build_headers(req.method, req.path, "");
        metrics_.inc(MetricId::EXEC_REST_REQUESTS_CANCEL);

        AsyncLogger::log(log_handle_, LogLevel::INFO, "REST: sending DELETE /cancel-all");

        uint32_t intent_id = intent.intent_id;

        // Send cancel-all on ALL dedicated cancel connections
        for (auto& cc : cancel_clients_) {
            cc->async_send(req, headers,
                [this, intent_id](RestResponse resp) {
                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                        "REST: DELETE /cancel-all response: http=%d body=%.200s",
                        resp.http_status, resp.body.c_str());
                    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                    on_cancel_response(intent_id, OrderId{}, OrderId{}, resp);
                });
        }
    }

    void check_heartbeat() {
        if (heartbeat_in_flight_) return;  // one at a time

        Timestamp_ns now = SteadyClock::now();

        if (!heartbeat_mgr_.is_due(now)) return;

        if (!rate_limiter_.try_acquire(ExecIntentType::HEARTBEAT, now)) {
            return;
        }

        send_heartbeat();
    }

    void send_heartbeat() {
        heartbeat_in_flight_ = true;  // set before dispatch

        RestRequest req;
        req.method = HttpMethod::POST;
        req.path = "/v1/heartbeats";
        req.created_ts = SteadyClock::now();

        // Build heartbeat body: {heartbeat_id: id|null}
        std::string current_id = heartbeat_mgr_.current_heartbeat_id();
        if (current_id.empty()) {
            req.body = R"({"heartbeat_id":null})";
        } else {
            req.body.clear();
            req.body.reserve(current_id.size() + 32);
            req.body += R"({"heartbeat_id":")";
            append_json_escaped(req.body, current_id);
            req.body += R"("})";
        }

        auto headers = rest_auth_->build_headers(req.method, req.path, req.body);
        metrics_.inc(MetricId::EXEC_REST_REQUESTS_HEARTBEAT);

        // Send heartbeat on cancel_clients_[0] — isolated from order pipeline.
        // During fill bursts, the order pipeline can be blocked by batch/FAK orders;
        // sending heartbeat there risks missing the dead man's switch threshold.
        cancel_clients_[0]->async_send(req, headers,
            [this](RestResponse resp) {
                on_heartbeat_response(resp);
            });

        // Keep all other connections warm with a lightweight GET request.
        // Cannot reuse heartbeat — it's stateful (each response returns
        // the next ID, and multiple connections sharing one ID causes conflicts).
        {
            RestRequest keepalive;
            keepalive.method = HttpMethod::GET;
            keepalive.path = "/time";
            keepalive.created_ts = SteadyClock::now();
            auto ka_headers = rest_auth_->build_headers(keepalive.method, keepalive.path, "");
            for (auto& oc : order_pool_) {
                oc->async_send(keepalive, ka_headers, [](RestResponse) {});
            }
            for (size_t i = 1; i < cancel_clients_.size(); ++i) {
                cancel_clients_[i]->async_send(keepalive, ka_headers, [](RestResponse) {});
            }
        }
    }

    void heartbeat_failure(int http_status, Timestamp_ns now) {
        heartbeat_mgr_.on_failure(now);
        metrics_.inc(MetricId::EXEC_HEARTBEAT_FAIL);

        ExecFeedback fb;
        fb.kind = ExecFeedbackKind::HEARTBEAT_FAILED;
        fb.http_status = http_status;
        publish_feedback_raw(fb);

        if (heartbeat_mgr_.should_cancel_all() && !heartbeat_cancel_all_latched_) {
            heartbeat_cancel_all_latched_ = true;
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                           "Heartbeat failure threshold reached -- cancelling all orders");
            ExecIntent cancel_all;
            cancel_all.type = ExecIntentType::CANCEL_ALL;
            cancel_all.created_ts = now;
            send_cancel_all(cancel_all);
        }
        check_degraded_transition();
    }

    void on_heartbeat_response(const RestResponse& resp) {
        heartbeat_in_flight_ = false;  // clear guard
        Timestamp_ns now = SteadyClock::now();
        rate_limiter_.record_response(resp.http_status, now);

        // Timeout or connection error
        if (resp.timed_out || resp.http_status == 0) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat timeout: http=%d", resp.http_status);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            heartbeat_failure(resp.http_status, now);
            return;
        }

        // Auth failure — server won't recognize us, will cancel all orders.
        // Trigger fatal shutdown immediately.
        if (resp.http_status == 401 || resp.http_status == 403) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "FATAL: REST auth failure on heartbeat: http=%d body=%.200s",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);
            heartbeat_failure(resp.http_status, now);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // 429 Rate limited — don't count toward consecutive failure threshold.
        // Rate limiter backoff is already applied via record_response().
        // The heartbeat wasn't rejected due to server-side issues, just throttled.
        if (resp.http_status == 429) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat rate limited (429)");
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            metrics_.inc(MetricId::EXEC_HTTP_429);
            // Don't call heartbeat_failure() — backoff will delay retry, chain is intact
            check_degraded_transition();
            return;
        }

        // 425 Matching engine restarting — same treatment as 429 for heartbeats:
        // don't count as heartbeat failure, backoff delays retry, chain stays intact.
        if (resp.http_status == 425) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat matching engine restarting (425)");
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            metrics_.inc(MetricId::EXEC_HTTP_425);
            if (rate_limiter_.is_matching_engine_down()) {
                AsyncLogger::log(log_handle_, LogLevel::FATAL,
                    "Matching engine down: 10 consecutive 425 responses -- fatal");
                if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
            }
            check_degraded_transition();
            return;
        }

        // Other non-2xx is a failure
        if (resp.http_status < 200 || resp.http_status >= 300) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat failed: http=%d body=%.200s",
                resp.http_status, resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            heartbeat_failure(resp.http_status, now);
            return;
        }

        // 2xx — parse response body
        auto parsed = parse_heartbeat_response(resp.body);
        if (!parsed.ok()) {
            // Parse error on 2xx body — treat as failure, not success.
            // Calling on_success("") would silently break the heartbeat chain,
            // causing the next heartbeat to send an empty chaining ID.
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat 2xx but parse failed: body=%.200s",
                resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            heartbeat_failure(resp.http_status, now);
            return;
        }

        // Validate heartbeat_id is non-empty — empty ID breaks chaining
        if (parsed.value.heartbeat_id.empty()) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat 2xx but empty heartbeat_id: body=%.200s",
                resp.body.c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            heartbeat_failure(resp.http_status, now);
            return;
        }

        // Success
        {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "REST: heartbeat OK: id=%.40s",
                parsed.value.heartbeat_id.c_str());
            AsyncLogger::log(log_handle_, LogLevel::DEBUG, buf);
        }

        heartbeat_mgr_.on_success(parsed.value.heartbeat_id, now);
        heartbeat_cancel_all_latched_ = false;
        metrics_.inc(MetricId::EXEC_HEARTBEAT_OK);

        ExecFeedback fb;
        fb.kind = ExecFeedbackKind::HEARTBEAT_OK;
        fb.http_status = resp.http_status;
        publish_feedback_raw(fb);
        check_degraded_transition();
    }

    void publish_feedback(ExecFeedbackKind kind, const ExecIntent& intent,
                          int http_status, Timestamp_ns latency) {
        ExecFeedback fb;
        fb.kind = kind;
        fb.intent_id = intent.intent_id;
        fb.client_order_id = intent.client_order_id;
        fb.http_status = http_status;
        fb.latency_ns = latency;
        fb.created_ts = SteadyClock::now();
        publish_feedback_raw(fb);
    }

    static bool is_critical_feedback(ExecFeedbackKind kind) {
        return kind == ExecFeedbackKind::ORDER_REJECTED ||
               kind == ExecFeedbackKind::TIMEOUT ||
               kind == ExecFeedbackKind::EXCHANGE_UNAVAILABLE;
    }

    void publish_feedback_raw(const ExecFeedback& fb) {
        // Release in-flight SELL reservation if this feedback is for a tracked SELL intent.
        // Any terminal feedback (accepted, rejected, rate_limited, timeout) releases the reservation.
        if (fb.kind != ExecFeedbackKind::REQUEST_SENT &&
            fb.kind != ExecFeedbackKind::HEARTBEAT_OK &&
            fb.kind != ExecFeedbackKind::HEARTBEAT_FAILED &&
            fb.kind != ExecFeedbackKind::GATEWAY_DEGRADED &&
            fb.kind != ExecFeedbackKind::GATEWAY_RECOVERED) {
            auto it = inflight_sell_intents_.find(fb.intent_id);
            if (it != inflight_sell_intents_.end()) {
                auto& qty_ref = inflight_sell_qty_[it->second.asset_id];
                qty_ref = (qty_ref > it->second.qty) ? qty_ref - it->second.qty : 0;
                inflight_sell_intents_.erase(it);
            }
        }

        SchedulerEvent ev = from_exec_feedback(fb);

        // Critical feedback types (rejects, timeouts) use spin-yield retry
        // to avoid silently dropping state T2 must see.
        if (is_critical_feedback(fb.kind)) {
            static constexpr int kCriticalSpinLimit = 1024;
            static constexpr int kCriticalYieldStart = 128;
            for (int attempt = 0; attempt < kCriticalSpinLimit; ++attempt) {
                if (feedback_queue_.try_push(ev)) {
                    metrics_.inc(MetricId::EXEC_INTENT_QUEUE_PUSHES);
                    return;
                }
                if (attempt >= kCriticalYieldStart) {
                    std::this_thread::yield();
                }
            }
            // Exhausted retries for critical feedback -- immediate fatal
            ++feedback_overflow_count_;
            metrics_.inc(MetricId::EXEC_INTENT_QUEUE_OVERFLOW);
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "FATAL: Critical feedback lost (kind=%u) after %d retries",
                static_cast<unsigned>(fb.kind), kCriticalSpinLimit);
            AsyncLogger::log(log_handle_, LogLevel::FATAL, buf);
            if (fatal_flag_) {
                fatal_flag_->store(true, std::memory_order_relaxed);
            }
            return;
        }

        // Non-critical feedback: try once, handle overflow gracefully
        if (!feedback_queue_.try_push(ev)) {
            ++feedback_overflow_count_;
            metrics_.inc(MetricId::EXEC_INTENT_QUEUE_OVERFLOW);

            if (!feedback_overflow_escalated_) {
                feedback_overflow_escalated_ = true;
                metrics_.inc(MetricId::EXEC_GATEWAY_DEGRADED_COUNT);
                AsyncLogger::log(log_handle_, LogLevel::ERROR,
                               "Feedback queue overflow detected -- gateway degraded");
            }

            // Logarithmic throttle: log at 1st, 10th, 100th, then every 1000th
            if (feedback_overflow_count_ == 1 ||
                feedback_overflow_count_ == 10 ||
                feedback_overflow_count_ == 100 ||
                (feedback_overflow_count_ % 1000) == 0) {
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                    "Feedback queue overflow (kind=%u, total_drops=%lld)",
                    static_cast<unsigned>(fb.kind),
                    static_cast<long long>(feedback_overflow_count_));
                AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            }

            if (!feedback_overflow_fatal_ && fatal_flag_ &&
                feedback_overflow_count_ >= kFeedbackOverflowFatalThreshold) {
                feedback_overflow_fatal_ = true;
                fatal_flag_->store(true, std::memory_order_relaxed);
                AsyncLogger::log(log_handle_, LogLevel::FATAL,
                               "Feedback queue overflow threshold exceeded -- triggering shutdown");
            }
        } else {
            metrics_.inc(MetricId::EXEC_INTENT_QUEUE_PUSHES);
        }
    }

    static SchedulerEvent from_exec_feedback(const ExecFeedback& fb) {
        SchedulerEvent ev;
        ev.source = EventSource::EXEC_INTERNAL;
        ev.recv_ts = SteadyClock::now();

        // Map feedback kind to scheduler event kind
        switch (fb.kind) {
            case ExecFeedbackKind::ORDER_ACCEPTED:
                ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
                ev.exec_accepted = true;
                break;
            case ExecFeedbackKind::ORDER_REJECTED:
            case ExecFeedbackKind::RATE_LIMITED:
            case ExecFeedbackKind::EXCHANGE_UNAVAILABLE:
                ev.kind = SchedulerEventKind::EXEC_ORDER_REJECT;
                ev.exec_accepted = false;
                break;
            case ExecFeedbackKind::TIMEOUT:
                // Ambiguous: order may have been accepted server-side.
                // Do NOT map to reject to prevent duplicate resubmission.
                ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
                ev.exec_accepted = false;  // signal ambiguity via accepted=false + ACK kind
                break;
            case ExecFeedbackKind::CANCEL_CONFIRMED:
                ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
                ev.exec_accepted = true;
                break;
            case ExecFeedbackKind::HEARTBEAT_OK:
            case ExecFeedbackKind::HEARTBEAT_FAILED:
            case ExecFeedbackKind::GATEWAY_DEGRADED:
            case ExecFeedbackKind::GATEWAY_RECOVERED:
            case ExecFeedbackKind::REQUEST_SENT:
                ev.kind = SchedulerEventKind::EXEC_ORDER_ACK;
                ev.exec_accepted = true;
                break;
        }

        ev.intent_ref_id = fb.intent_id;
        ev.client_order_id = fb.client_order_id;
        ev.order_id = fb.exchange_order_id;
        ev.exec_feedback_kind = static_cast<uint8_t>(fb.kind);
        ev.exec_http_status = fb.http_status;

        return ev;
    }
};

ExecutionGateway::ExecutionGateway(SpscQueue<ExecIntent>& intent_queue,
                                   SpscQueue<SchedulerEvent>& feedback_queue,
                                   Metrics& metrics, AsyncLogger& logger,
                                   const GatewayConfig& config, OrderSigner& signer,
                                   std::atomic<bool>* fatal_flag,
                                   TokenInventory* inventory_view)
    : impl_(std::make_unique<Impl>(intent_queue, feedback_queue, metrics, logger,
                                   config, signer, fatal_flag, inventory_view)) {}

ExecutionGateway::~ExecutionGateway() = default;

void ExecutionGateway::run() {
    impl_->run();
}

void ExecutionGateway::request_shutdown() {
    impl_->request_shutdown();
}

}  // namespace lt
