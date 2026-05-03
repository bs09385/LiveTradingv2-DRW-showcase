#include "scheduler/strategy_stub.h"

#include "common/clock.h"

namespace lt {

StrategyStub::StrategyStub(bool emit_intents) : emit_intents_(emit_intents) {}

IntentBatch StrategyStub::evaluate(const SchedulerEvent& event,
                                   const StrategyStateStub& /*state*/) {
    ++invocation_count_;

    IntentBatch batch;
    if (!emit_intents_) return batch;

    // M2 stub behavior: on market events with valid BBO, emit "would_quote" intents
    if (event.source == EventSource::MARKET_WS &&
        event.bbo.best_bid != kInvalidPrice &&
        event.bbo.best_ask != kInvalidPrice) {

        auto ts = SteadyClock::now();

        ExecutionIntent bid_intent;
        bid_intent.action = IntentAction::WOULD_PLACE_BID;
        bid_intent.asset_id = event.asset_id;
        bid_intent.price = event.bbo.best_bid;
        bid_intent.qty = 100;  // stub quantity
        bid_intent.created_ts = ts;
        bid_intent.intent_id = next_intent_id_++;
        batch.add(bid_intent);

        ExecutionIntent ask_intent;
        ask_intent.action = IntentAction::WOULD_PLACE_ASK;
        ask_intent.asset_id = event.asset_id;
        ask_intent.price = event.bbo.best_ask;
        ask_intent.qty = 100;  // stub quantity
        ask_intent.created_ts = ts;
        ask_intent.intent_id = next_intent_id_++;
        batch.add(ask_intent);
    }

    return batch;
}

}  // namespace lt
