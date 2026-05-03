#include <doctest/doctest.h>
#include "exec/exec_intent.h"
#include "exec/exec_feedback.h"
#include "exec/exec_queue_sink.h"
#include "exec/heartbeat_manager.h"
#include "rest/rest_response_parser.h"
#include "events/scheduler_events.h"
#include "queue/spsc_queue.h"
#include "crypto/hex_utils.h"
#include "crypto/order_signer.h"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// Cancel ID Plumbing (Patch 1)
// ---------------------------------------------------------------------------
TEST_SUITE("M4 Audit R2 - Cancel ID Plumbing") {

TEST_CASE("ExecQueueSink carries exchange_order_id for cancel intent") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_CANCEL_BID;
    intent.asset_id = lt::AssetId("asset-1");
    intent.market_id = lt::AssetId("market-1");
    intent.exchange_order_id = lt::OrderId("0xdeadbeef1234");
    intent.client_order_id = lt::OrderId("client-42");
    intent.intent_id = 99;

    auto result = sink.accept(intent);
    CHECK(result == lt::SinkResult::ACCEPTED);

    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->type == lt::ExecIntentType::CANCEL_ORDER);
    CHECK(front->exchange_order_id.view() == "0xdeadbeef1234");
    CHECK(front->client_order_id.view() == "client-42");
    CHECK(front->market_id.view() == "market-1");
    CHECK(front->intent_id == 99);
}

TEST_CASE("PLACE intent leaves exchange_order_id empty") {
    lt::SpscQueue<lt::ExecIntent> queue(64);
    lt::ExecQueueSink sink(queue);

    lt::ExecutionIntent intent;
    intent.action = lt::IntentAction::WOULD_PLACE_BID;
    intent.asset_id = lt::AssetId("asset-2");
    intent.market_id = lt::AssetId("market-2");
    intent.client_order_id = lt::OrderId("client-43");
    // exchange_order_id deliberately not set

    auto result = sink.accept(intent);
    CHECK(result == lt::SinkResult::ACCEPTED);

    auto* front = queue.front();
    REQUIRE(front != nullptr);
    CHECK(front->type == lt::ExecIntentType::PLACE_ORDER);
    CHECK(front->exchange_order_id.view().empty());
    CHECK(front->client_order_id.view() == "client-43");
    CHECK(front->market_id.view() == "market-2");
}

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Strict Cancel Validation (Patch 2)
// ---------------------------------------------------------------------------
TEST_SUITE("M4 Audit R2 - Cancel Strict Validation") {

TEST_CASE("cancel response: invalid JSON on 2xx is parse error") {
    // Simulates gateway receiving garbled 2xx body.
    // Parser should return error.
    auto parsed = lt::parse_cancel_response("not valid json at all!!!");
    CHECK_FALSE(parsed.ok());
}

TEST_CASE("cancel response: target found in canceled array") {
    std::string json = R"({"canceled":["0xorder1","0xtarget","0xorder2"],"not_canceled":{}})";
    auto parsed = lt::parse_cancel_response(json);
    REQUIRE(parsed.ok());

    // Check target is present
    bool found = false;
    for (const auto& cid : parsed.value.canceled) {
        if (cid == "0xtarget") {
            found = true;
            break;
        }
    }
    CHECK(found);
    CHECK(parsed.value.not_canceled.empty());
}

TEST_CASE("cancel response: target NOT in canceled array") {
    std::string json = R"({"canceled":["0xother1","0xother2"],"not_canceled":{}})";
    auto parsed = lt::parse_cancel_response(json);
    REQUIRE(parsed.ok());

    // Target is not in the canceled list
    bool found = false;
    for (const auto& cid : parsed.value.canceled) {
        if (cid == "0xtarget") {
            found = true;
            break;
        }
    }
    CHECK_FALSE(found);
}

TEST_CASE("cancel response: empty arrays are ambiguous for single cancel") {
    // Both arrays empty on a single cancel means the target wasn't confirmed.
    std::string json = R"({"canceled":[],"not_canceled":{}})";
    auto parsed = lt::parse_cancel_response(json);
    REQUIRE(parsed.ok());
    CHECK(parsed.value.canceled.empty());
    CHECK(parsed.value.not_canceled.empty());
    // Gateway should reject single cancel with exchange_oid when target not found
}

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Heartbeat Degraded (Patch 3)
// ---------------------------------------------------------------------------
TEST_SUITE("M4 Audit R2 - Heartbeat Degraded") {

TEST_CASE("is_failed() true at failure threshold") {
    lt::HeartbeatConfig cfg;
    cfg.max_consecutive_failures = 3;
    cfg.cancel_all_on_failure = false;  // independent of cancel_all
    lt::HeartbeatManager mgr(cfg);

    CHECK_FALSE(mgr.is_failed());

    mgr.on_failure(0);
    mgr.on_failure(0);
    CHECK_FALSE(mgr.is_failed());

    mgr.on_failure(0);
    CHECK(mgr.is_failed());
    CHECK(mgr.consecutive_failures() == 3);
}

TEST_CASE("is_failed() resets on success") {
    lt::HeartbeatConfig cfg;
    cfg.max_consecutive_failures = 2;
    lt::HeartbeatManager mgr(cfg);

    mgr.on_failure(0);
    mgr.on_failure(0);
    CHECK(mgr.is_failed());

    mgr.on_success("hb-1", 1000000);
    CHECK_FALSE(mgr.is_failed());
    CHECK(mgr.consecutive_failures() == 0);
}

TEST_CASE("is_failed() independent of cancel_all_on_failure") {
    // is_failed() checks threshold regardless of cancel_all_on_failure config.
    // should_cancel_all() additionally requires cancel_all_on_failure=true.
    lt::HeartbeatConfig cfg;
    cfg.max_consecutive_failures = 2;
    cfg.cancel_all_on_failure = false;
    lt::HeartbeatManager mgr(cfg);

    mgr.on_failure(0);
    mgr.on_failure(0);

    CHECK(mgr.is_failed());                // threshold reached
    CHECK_FALSE(mgr.should_cancel_all());  // cancel_all_on_failure is false
}

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Address Derivation Guard (Patch 5)
// ---------------------------------------------------------------------------
TEST_SUITE("M4 Audit R2 - Address Guard") {

TEST_CASE("known privkey derives expected address") {
    // Well-known Hardhat account #0 private key
    static const char* TEST_PK =
        "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
    static const char* EXPECTED_ADDR = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";

    lt::Bytes32 pk{};
    REQUIRE(lt::hex_decode_to_bytes32(TEST_PK, pk));
    lt::Secp256k1OrderSigner signer(pk.data());

    uint8_t derived[20]{};
    REQUIRE(signer.get_signer_address(derived));

    uint8_t expected[20]{};
    REQUIRE(lt::parse_eth_address(EXPECTED_ADDR, expected));

    CHECK(std::memcmp(derived, expected, 20) == 0);
}

TEST_CASE("parse_eth_address rejects short hex") {
    uint8_t out[20]{};
    CHECK_FALSE(lt::parse_eth_address("0x1234", out));
    CHECK_FALSE(lt::parse_eth_address("abcdef", out));
}

}  // TEST_SUITE
