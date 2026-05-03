#include <doctest/doctest.h>
#include "exec/exec_intent.h"
#include "exec/exec_feedback.h"
#include "exec/exec_queue_sink.h"
#include "exec/execution_gateway.h"
#include "exec/rate_limiter.h"
#include "exec/heartbeat_manager.h"
#include "events/scheduler_events.h"
#include "common/token_inventory.h"
#include "crypto/order_signer.h"
#include "crypto/hex_utils.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

static const char* TEST_PRIVATE_KEY_HEX =
    "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
static const char* TEST_ADDRESS = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";

template <typename Pred>
bool wait_for(Pred pred, int timeout_ms = 2000) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

lt::GatewayConfig make_test_gateway_config() {
    lt::GatewayConfig cfg;
    cfg.gateway_enabled = true;
    cfg.request_timeout_ms = 100;
    cfg.heartbeat.interval_ms = 0;  // disable heartbeats in unit tests
    cfg.owner_uuid = "test-owner";
    cfg.maker_address = TEST_ADDRESS;
    cfg.signer_address = TEST_ADDRESS;

    // Avoid the production default (clob.polymarket.com). Tests must not
    // perform real DNS or TLS handshakes — the warmup logic in run() would
    // otherwise spawn a getaddrinfo worker that blocks the io_context
    // destructor for many seconds when several gateway tests run in the
    // same process. 127.0.0.1:1 resolves instantly and refuses immediately.
    cfg.rest_host = "127.0.0.1";
    cfg.rest_port = "1";
    cfg.skip_warmup = true;

    // Explicit auth override avoids env var dependency.
    cfg.poly_api_key = "test-key";
    cfg.poly_api_secret_b64 = "dGVzdC1zZWNyZXQ=";  // base64("test-secret")
    cfg.poly_api_passphrase = "test-pass";
    cfg.poly_api_address = TEST_ADDRESS;
    return cfg;
}

}  // namespace

TEST_SUITE("ExecGatewayIntegration") {

TEST_CASE("ExecQueueSink pushes to queue") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_PLACE_BID;
    intent.asset_id = lt::AssetId("asset-1");
    intent.price = 5200;
    intent.qty = lt::qty_from_int(100);
    intent.intent_id = 42;

    auto result = sink.accept(intent);
    CHECK(result == lt::SinkResult::ACCEPTED);
    CHECK(sink.intent_count() == 1);
    CHECK(sink.overflow_count() == 0);

    // Verify intent was pushed to queue
    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->type == lt::ExecIntentType::PLACE_ORDER);
    CHECK(front->side == lt::Side::BID);
    CHECK(front->price == 5200);
    CHECK(front->size == lt::qty_from_int(100));
    CHECK(front->intent_id == 42);
}

TEST_CASE("ExecQueueSink maps ASK correctly") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_PLACE_ASK;
    intent.asset_id = lt::AssetId("asset-1");
    intent.price = 7500;
    intent.qty = lt::qty_from_int(50);

    auto result = sink.accept(intent);
    CHECK(result == lt::SinkResult::ACCEPTED);

    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->type == lt::ExecIntentType::PLACE_ORDER);
    CHECK(front->side == lt::Side::ASK);
}

TEST_CASE("ExecQueueSink maps CANCEL correctly") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_CANCEL_BID;
    intent.asset_id = lt::AssetId("asset-1");

    auto result = sink.accept(intent);
    CHECK(result == lt::SinkResult::ACCEPTED);

    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->type == lt::ExecIntentType::CANCEL_ORDER);
    CHECK(front->side == lt::Side::BID);
}

TEST_CASE("ExecQueueSink overflow returns OVERFLOW") {
    lt::SpscQueue<lt::ExecIntent> queue(2);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_PLACE_BID;
    intent.price = 5000;
    intent.qty = lt::qty_from_int(10);

    // Fill the queue
    CHECK(sink.accept(intent) == lt::SinkResult::ACCEPTED);
    CHECK(sink.accept(intent) == lt::SinkResult::ACCEPTED);

    // Third push should overflow
    CHECK(sink.accept(intent) == lt::SinkResult::OVERFLOW);
    CHECK(sink.overflow_count() == 1);
}

TEST_CASE("ExecFeedback set_error truncates long messages") {
    lt::ExecFeedback fb;
    std::string long_msg(200, 'x');
    fb.set_error(long_msg.c_str());

    CHECK(std::strlen(fb.error_msg) < 128);
    CHECK(fb.error_msg[127] == '\0');
}

TEST_CASE("ExecFeedback kind values") {
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::REQUEST_SENT) == 0);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_ACCEPTED) == 1);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_REJECTED) == 2);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::CANCEL_CONFIRMED) == 3);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::RATE_LIMITED) == 4);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::EXCHANGE_UNAVAILABLE) == 5);
    CHECK(static_cast<uint8_t>(lt::ExecFeedbackKind::TIMEOUT) == 6);
}

TEST_CASE("ExecIntent POD default values") {
    lt::ExecIntent intent;
    CHECK(intent.type == lt::ExecIntentType::PLACE_ORDER);
    CHECK(intent.side == lt::Side::BID);
    CHECK(intent.price == 0);
    CHECK(intent.size == 0);
    CHECK(intent.neg_risk == false);
    CHECK(intent.expiration == 0);
}

TEST_CASE("SchedulerEvent exec fields") {
    lt::SchedulerEvent ev;
    ev.source = lt::EventSource::EXEC_INTERNAL;
    ev.kind = lt::SchedulerEventKind::EXEC_ORDER_ACK;
    ev.exec_feedback_kind = static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_ACCEPTED);
    ev.exec_http_status = 200;
    ev.exec_accepted = true;
    ev.intent_ref_id = 42;
    ev.client_order_id = lt::OrderId("client-001");

    CHECK(ev.exec_feedback_kind == 1);
    CHECK(ev.exec_http_status == 200);
    CHECK(ev.exec_accepted);
    CHECK(ev.intent_ref_id == 42);
}

TEST_CASE("Multiple intents through queue maintain order") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    for (int i = 0; i < 10; ++i) {
        lt::ExecutionIntent intent;
        intent.action = lt::IntentAction::WOULD_PLACE_BID;
        intent.intent_id = static_cast<uint32_t>(i);
        intent.price = 5000 + i * 100;
        intent.qty = lt::qty_from_int(10);
        CHECK(sink.accept(intent) == lt::SinkResult::ACCEPTED);
    }

    // Verify order preserved
    for (int i = 0; i < 10; ++i) {
        auto* front = queue.front();
        REQUIRE(front != nullptr);
        CHECK(front->intent_id == static_cast<uint32_t>(i));
        CHECK(front->price == 5000 + i * 100);
        queue.pop();
    }
}

TEST_CASE("ExecutionGateway rejects insufficient SELL inventory without REST submit") {
    lt::SpscQueue<lt::ExecIntent> intent_q(64);
    lt::SpscQueue<lt::SchedulerEvent> feedback_q(64);
    lt::Metrics metrics;
    lt::AsyncLogger logger("test_exec_gateway_local_reject.log", 1024, lt::LogLevel::WARN);
    logger.start();

    lt::TokenInventory inventory;
    inventory.register_token(lt::AssetId("asset-1"));
    inventory.set_position(lt::AssetId("asset-1"), lt::qty_from_int(5));

    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    std::atomic<bool> fatal{false};
    lt::GatewayConfig cfg = make_test_gateway_config();
    lt::ExecutionGateway gateway(intent_q, feedback_q, metrics, logger, cfg, signer, &fatal,
                                 &inventory);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.side = lt::Side::ASK;
    intent.asset_id = lt::AssetId("asset-1");
    intent.size = lt::qty_from_int(20);
    intent.price = 6000;
    intent.intent_id = 42;
    intent.client_order_id = lt::OrderId("client-42");
    REQUIRE(intent_q.try_push(intent));

    std::thread t([&]() { gateway.run(); });
    bool got_feedback = wait_for([&]() { return feedback_q.front() != nullptr; });
    REQUIRE(got_feedback);

    auto* ev = feedback_q.front();
    REQUIRE(ev != nullptr);
    CHECK(ev->source == lt::EventSource::EXEC_INTERNAL);
    CHECK(ev->kind == lt::SchedulerEventKind::EXEC_ORDER_REJECT);
    CHECK(ev->intent_ref_id == 42);
    CHECK(ev->exec_feedback_kind == static_cast<uint8_t>(lt::ExecFeedbackKind::ORDER_REJECTED));

    CHECK(metrics.get(lt::MetricId::EXEC_LOCAL_INVENTORY_REJECTS) == 1);
    CHECK(metrics.get(lt::MetricId::EXEC_REST_REQUESTS_ORDER) == 0);
    CHECK_FALSE(fatal.load());

    gateway.request_shutdown();
    t.join();

    logger.stop();
    std::remove("test_exec_gateway_local_reject.log");
}

TEST_CASE("ExecutionGateway critical feedback overflow sets fatal on local reject") {
    lt::SpscQueue<lt::ExecIntent> intent_q(64);
    lt::SpscQueue<lt::SchedulerEvent> feedback_q(1);  // tiny queue to force overflow
    lt::Metrics metrics;
    lt::AsyncLogger logger("test_exec_gateway_critical_overflow.log", 1024, lt::LogLevel::WARN);
    logger.start();

    // Pre-fill queue so critical feedback push cannot succeed.
    lt::SchedulerEvent prefill;
    prefill.kind = lt::SchedulerEventKind::EXEC_ORDER_ACK;
    REQUIRE(feedback_q.try_push(prefill));

    lt::TokenInventory inventory;
    inventory.register_token(lt::AssetId("asset-1"));
    inventory.set_position(lt::AssetId("asset-1"), 0);

    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    std::atomic<bool> fatal{false};
    lt::GatewayConfig cfg = make_test_gateway_config();
    lt::ExecutionGateway gateway(intent_q, feedback_q, metrics, logger, cfg, signer, &fatal,
                                 &inventory);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.side = lt::Side::ASK;
    intent.asset_id = lt::AssetId("asset-1");
    intent.size = lt::qty_from_int(10);
    intent.price = 6000;
    intent.intent_id = 77;
    intent.client_order_id = lt::OrderId("client-77");
    REQUIRE(intent_q.try_push(intent));

    std::thread t([&]() { gateway.run(); });
    bool fatal_set = wait_for([&]() { return fatal.load(); }, 3000);
    CHECK(fatal_set);

    CHECK(metrics.get(lt::MetricId::EXEC_LOCAL_INVENTORY_REJECTS) == 1);
    CHECK(metrics.get(lt::MetricId::EXEC_INTENT_QUEUE_OVERFLOW) >= 1);
    CHECK(metrics.get(lt::MetricId::EXEC_REST_REQUESTS_ORDER) == 0);

    gateway.request_shutdown();
    t.join();

    logger.stop();
    std::remove("test_exec_gateway_critical_overflow.log");
}

}  // TEST_SUITE
