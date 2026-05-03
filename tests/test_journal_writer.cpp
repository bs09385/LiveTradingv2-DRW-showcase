#include "doctest/doctest.h"

#include <cstring>

#include "recorder/journal_types.h"
#include "recorder/journal_writer.h"
#include "queue/spsc_queue.h"
#include "logger/metrics.h"

using namespace lt;

TEST_SUITE("JournalTypes") {

TEST_CASE("JournalRecord size is exactly 256 bytes") {
    CHECK(sizeof(JournalRecord) == 256);
}

TEST_CASE("JournalRecord is trivially copyable") {
    CHECK(std::is_trivially_copyable_v<JournalRecord>);
}

TEST_CASE("JournalRecord is standard layout") {
    CHECK(std::is_standard_layout_v<JournalRecord>);
}

TEST_CASE("JournalId size is exactly 72 bytes") {
    CHECK(sizeof(JournalId) == 72);
}

TEST_CASE("JournalId from AssetId preserves data and hash") {
    AssetId asset("token_abc_123");
    JournalId jid(asset);

    CHECK(std::string(jid.data) == "token_abc_123");
    CHECK(jid.hash == fnv1a_hash(asset.data, asset.len));
}

TEST_CASE("JournalId from OrderId preserves data and hash") {
    OrderId order("0xdeadbeef1234");
    JournalId jid(order);

    CHECK(std::string(jid.data) == "0xdeadbeef1234");
    CHECK(jid.hash == fnv1a_hash(order.data, order.len));
}

TEST_CASE("JournalId from TradeId preserves data and hash") {
    TradeId trade("550e8400-e29b-41d4-a716-446655440000");
    JournalId jid(trade);

    CHECK(std::string(jid.data) == "550e8400-e29b-41d4-a716-446655440000");
    CHECK(jid.hash == fnv1a_hash(trade.data, trade.len));
}

TEST_CASE("JournalId truncates long IDs but preserves full hash") {
    // Create an ID longer than 63 chars (the JournalId data limit)
    std::string long_id(70, 'x');
    OrderId order(long_id);
    JournalId jid(order);

    // Data truncated to 63 chars
    CHECK(std::strlen(jid.data) == 63);
    // Hash covers the full original
    CHECK(jid.hash == fnv1a_hash(order.data, order.len));
}

TEST_CASE("Payload union sizes fit within 152 bytes") {
    CHECK(sizeof(JournalStrategyEval) <= 152);
    CHECK(sizeof(JournalRiskDecision) <= 152);
    CHECK(sizeof(JournalOrderSent) <= 152);
    CHECK(sizeof(JournalExecFeedback) <= 152);
    CHECK(sizeof(JournalOrderStatus) <= 152);
    CHECK(sizeof(JournalFill) <= 152);
    CHECK(sizeof(JournalModeChange) <= 152);
    CHECK(sizeof(JournalCancelAll) <= 152);
}

}  // TEST_SUITE JournalTypes

TEST_SUITE("JournalWriter") {

TEST_CASE("No-op when queue is nullptr") {
    Metrics metrics;
    JournalWriter writer(nullptr, &metrics, kJournalLevelFull, false);

    CHECK(writer.enabled() == false);

    // Should not crash
    SchedulerEvent ev;
    writer.record_strategy_eval(ev, 5000, 5100, 4900, 5200, 10, 2);
    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);
    writer.record_cancel_all(EventSource::CONTROL, 5);

    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_PUSHED) == 0);
    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_DROPPED) == 0);
}

TEST_CASE("Level gating: level 0 skips strategy and risk records") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelLifecycle, false);

    CHECK(writer.enabled() == true);

    // Level 1 records should be skipped
    SchedulerEvent ev;
    writer.record_strategy_eval(ev, 5000, 5100, 4900, 5200, 10, 2);

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.asset_id = AssetId("tok");
    writer.record_risk_decision(intent, RiskDecision::ALLOW, RiskDenyReason::NONE, 0, 0);

    CHECK(queue.front() == nullptr);
    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_PUSHED) == 0);

    // Level 0 records should pass
    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);
    CHECK(queue.front() != nullptr);
    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_PUSHED) == 1);
}

TEST_CASE("Level 1 pushes strategy and risk records") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    SchedulerEvent ev;
    ev.asset_id = AssetId("token123");
    ev.recv_ts = 1000;
    writer.record_strategy_eval(ev, 5000, 5100, 4900, 5200, 10, 2);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::STRATEGY_EVAL));
    CHECK(rec->payload.strategy_eval.bbo_bid == 5000);
    CHECK(rec->payload.strategy_eval.bbo_ask == 5100);
    CHECK(rec->payload.strategy_eval.desired_bid == 4900);
    CHECK(rec->payload.strategy_eval.desired_ask == 5200);
    CHECK(rec->payload.strategy_eval.qty == 10);
    CHECK(rec->payload.strategy_eval.intent_count == 2);
    CHECK(rec->recv_ts == 1000);
    queue.pop();

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.asset_id = AssetId("tok");
    intent.price = 4800;
    intent.qty = 5;
    writer.record_risk_decision(intent, RiskDecision::DENY,
                                 RiskDenyReason::POSITION_LIMIT, 100, 50000);

    rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::RISK_DECISION));
    CHECK(rec->payload.risk_decision.decision == static_cast<uint8_t>(RiskDecision::DENY));
    CHECK(rec->payload.risk_decision.deny_reason ==
          static_cast<uint8_t>(RiskDenyReason::POSITION_LIMIT));
    CHECK(rec->payload.risk_decision.price == 4800);
    CHECK(rec->payload.risk_decision.qty == 5);
    CHECK(rec->payload.risk_decision.position == 100);
    CHECK(rec->payload.risk_decision.notional == 50000);
    queue.pop();

    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_PUSHED) == 2);
}

TEST_CASE("Order sent record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.asset_id = AssetId("tok_up");
    intent.client_order_id = OrderId("client123");
    intent.price = 6000;
    intent.qty = 20;
    intent.level = 1;
    writer.record_order_sent(intent, 5900, 6100);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::ORDER_SENT));
    CHECK(rec->payload.order_sent.price == 6000);
    CHECK(rec->payload.order_sent.qty == 20);
    CHECK(rec->payload.order_sent.side == static_cast<uint8_t>(Side::ASK));
    CHECK(rec->payload.order_sent.level == 1);
    CHECK(rec->payload.order_sent.bbo_bid == 5900);
    CHECK(rec->payload.order_sent.bbo_ask == 6100);
}

TEST_CASE("Fill record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    SchedulerEvent ev;
    ev.asset_id = AssetId("tok");
    ev.trade_id = TradeId("trade-uuid-123");
    ev.user_price = 5200;
    ev.user_fill_size = 10;
    ev.user_side = Side::BID;
    ev.recv_ts = 2000;
    writer.record_fill(ev, 30);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::FILL));
    CHECK(rec->payload.fill.fill_price == 5200);
    CHECK(rec->payload.fill.fill_size == 10);
    CHECK(rec->payload.fill.net_position_after == 30);
    CHECK(rec->payload.fill.side == static_cast<uint8_t>(Side::BID));
}

TEST_CASE("Mode change record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::MODE_CHANGE));
    CHECK(rec->payload.mode_change.old_mode == static_cast<uint8_t>(ExecutionMode::DRY_RUN));
    CHECK(rec->payload.mode_change.new_mode == static_cast<uint8_t>(ExecutionMode::LIVE));
}

TEST_CASE("Cancel-all record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    writer.record_cancel_all(EventSource::CONTROL, 7);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::CANCEL_ALL));
    CHECK(rec->payload.cancel_all.trigger_source == static_cast<uint8_t>(EventSource::CONTROL));
    CHECK(rec->payload.cancel_all.working_count == 7);
}

TEST_CASE("Dry run flag set when configured") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, true);

    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::DRY_RUN);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & kJournalFlagDryRun) != 0);
}

TEST_CASE("Dry run flag NOT set when live") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    writer.record_mode_change(ExecutionMode::LIVE, ExecutionMode::LIVE);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK((rec->flags & kJournalFlagDryRun) == 0);
}

TEST_CASE("Queue full: drops + metric") {
    SpscQueue<JournalRecord> queue(2);  // tiny queue
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    // Fill the queue
    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);
    CHECK(metrics.get(MetricId::JOURNAL_RECORDS_PUSHED) == 1);

    // This should eventually overflow (rigtorp queue capacity is next power of 2)
    for (int i = 0; i < 10; ++i) {
        writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);
    }

    auto pushed = metrics.get(MetricId::JOURNAL_RECORDS_PUSHED);
    auto dropped = metrics.get(MetricId::JOURNAL_RECORDS_DROPPED);
    CHECK(pushed > 0);
    CHECK(dropped > 0);
    CHECK(pushed + dropped == 11);
}

TEST_CASE("Sequence number increments") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    writer.record_mode_change(ExecutionMode::DRY_RUN, ExecutionMode::LIVE);
    auto* rec1 = queue.front();
    REQUIRE(rec1 != nullptr);
    uint16_t seq1 = rec1->seq;
    queue.pop();

    writer.record_cancel_all(EventSource::CONTROL, 0);
    auto* rec2 = queue.front();
    REQUIRE(rec2 != nullptr);
    CHECK(rec2->seq == seq1 + 1);
}

TEST_CASE("Binary round-trip: write and read back") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, true);

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.asset_id = AssetId("my_token_id");
    intent.price = 4200;
    intent.qty = 15;
    writer.record_order_sent(intent, 3000, 3100);

    auto* original = queue.front();
    REQUIRE(original != nullptr);

    // Copy to a byte buffer and back (simulates disk round-trip)
    uint8_t buffer[256];
    std::memcpy(buffer, original, 256);

    JournalRecord restored;
    std::memcpy(&restored, buffer, 256);

    CHECK(restored.type == original->type);
    CHECK(restored.flags == original->flags);
    CHECK(restored.seq == original->seq);
    CHECK(restored.wall_clock_ms == original->wall_clock_ms);
    CHECK(restored.payload.order_sent.price == 4200);
    CHECK(restored.payload.order_sent.qty == 15);
    CHECK(restored.payload.order_sent.bbo_bid == 3000);
    CHECK(restored.payload.order_sent.bbo_ask == 3100);
    CHECK((restored.flags & kJournalFlagDryRun) != 0);
}

TEST_CASE("Exec feedback record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    SchedulerEvent ev;
    ev.asset_id = AssetId("tok");
    ev.exec_feedback_kind = 1;  // ORDER_ACCEPTED
    ev.exec_http_status = 200;
    ev.order_id = OrderId("exch_order_1");
    ev.client_order_id = OrderId("client_order_1");
    ev.recv_ts = 3000;
    writer.record_exec_feedback(ev);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::EXEC_FEEDBACK));
    CHECK(rec->payload.exec_feedback.feedback_kind == 1);
    CHECK(rec->payload.exec_feedback.http_status == 200);
}

TEST_CASE("Order status record") {
    SpscQueue<JournalRecord> queue(64);
    Metrics metrics;
    JournalWriter writer(&queue, &metrics, kJournalLevelFull, false);

    SchedulerEvent ev;
    ev.asset_id = AssetId("tok");
    ev.order_id = OrderId("order_abc");
    ev.order_status_raw = static_cast<uint8_t>(OrderStatus::LIVE);
    ev.user_side = Side::ASK;
    ev.user_price = 7000;
    ev.user_original_size = 50;
    ev.user_cumulative_filled = 10;
    ev.recv_ts = 4000;
    writer.record_order_status(ev);

    auto* rec = queue.front();
    REQUIRE(rec != nullptr);
    CHECK(rec->type == static_cast<uint8_t>(JournalRecordType::ORDER_STATUS));
    CHECK(rec->payload.order_status.status == static_cast<uint8_t>(OrderStatus::LIVE));
    CHECK(rec->payload.order_status.side == static_cast<uint8_t>(Side::ASK));
    CHECK(rec->payload.order_status.price == 7000);
    CHECK(rec->payload.order_status.original_size == 50);
    CHECK(rec->payload.order_status.filled_size == 10);
}

}  // TEST_SUITE JournalWriter
