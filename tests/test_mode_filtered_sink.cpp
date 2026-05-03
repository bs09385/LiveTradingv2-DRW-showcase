#include "doctest/doctest.h"

#include "scheduler/mode_filtered_sink.h"

using namespace lt;

namespace {

// Test sink that records what was forwarded
class RecordingSink : public ExecSink {
public:
    SinkResult accept(const ExecutionIntent& intent) override {
        if (count < kMax) {
            intents[count++] = intent;
        }
        return SinkResult::ACCEPTED;
    }

    static constexpr int kMax = 32;
    ExecutionIntent intents[kMax]{};
    int count = 0;
};

ExecutionIntent make_bid(Price_t price = 5000, Qty_t qty = 5) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.price = price;
    intent.qty = qty;
    return intent;
}

ExecutionIntent make_ask(Price_t price = 5200, Qty_t qty = 5) {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.price = price;
    intent.qty = qty;
    return intent;
}

ExecutionIntent make_cancel_bid() {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_CANCEL_BID;
    intent.exchange_order_id = OrderId("exch1");
    return intent;
}

ExecutionIntent make_cancel_ask() {
    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_CANCEL_ASK;
    intent.exchange_order_id = OrderId("exch2");
    return intent;
}

}  // namespace

TEST_SUITE("ModeFilteredSink") {

TEST_CASE("DRY_RUN: placements logged, not forwarded") {
    RecordingSink inner;
    Metrics metrics;
    ModeFilteredSink sink(&inner, &metrics, ExecutionMode::DRY_RUN);

    auto result = sink.accept(make_bid());
    CHECK(result == SinkResult::FILTERED);
    CHECK(inner.count == 0);
    CHECK(sink.dry_run_logged() == 1);
    CHECK(metrics.get(MetricId::STRAT_DRY_RUN_LOGGED) == 1);
}

TEST_CASE("DRY_RUN: cancels also blocked") {
    RecordingSink inner;
    Metrics metrics;
    ModeFilteredSink sink(&inner, &metrics, ExecutionMode::DRY_RUN);

    sink.accept(make_cancel_bid());
    CHECK(inner.count == 0);
    CHECK(sink.dry_run_logged() == 1);
}

TEST_CASE("LIVE: everything forwarded") {
    RecordingSink inner;
    Metrics metrics;
    ModeFilteredSink sink(&inner, &metrics, ExecutionMode::LIVE);

    sink.accept(make_bid(5000, 100));  // any size
    sink.accept(make_ask(0, 5));       // zero price
    sink.accept(make_cancel_bid());
    CHECK(inner.count == 3);
    CHECK(sink.forwarded() == 3);
}

TEST_CASE("set_mode changes behavior at runtime") {
    RecordingSink inner;
    Metrics metrics;
    ModeFilteredSink sink(&inner, &metrics, ExecutionMode::DRY_RUN);

    sink.accept(make_bid());
    CHECK(inner.count == 0);  // DRY_RUN

    sink.set_mode(ExecutionMode::LIVE);
    sink.accept(make_bid());
    CHECK(inner.count == 1);  // LIVE
}

TEST_CASE("null inner: returns NO_QUEUE in non-DRY_RUN modes") {
    Metrics metrics;
    ModeFilteredSink sink(nullptr, &metrics, ExecutionMode::LIVE);

    auto result = sink.accept(make_bid());
    CHECK(result == SinkResult::NO_QUEUE);
}

TEST_CASE("DRY_RUN: dry_run_logged counter increments") {
    RecordingSink inner;
    ModeFilteredSink sink(&inner, nullptr, ExecutionMode::DRY_RUN);

    for (int i = 0; i < 5; ++i) sink.accept(make_bid());
    CHECK(sink.dry_run_logged() == 5);
}

}  // TEST_SUITE
