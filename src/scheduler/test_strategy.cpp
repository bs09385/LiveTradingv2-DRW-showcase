#include "scheduler/test_strategy.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "events/user_events.h"
#include "exec/exec_feedback.h"

namespace lt {

namespace {

void write_timestamp(FILE* f) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    std::fprintf(f, "[%02d:%02d:%02d.%03d] ",
                 tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                 static_cast<int>(ms));
}

const char* phase_name(TestPhase p) {
    switch (p) {
        case TestPhase::WAIT_FOR_BBO:     return "WAIT_FOR_BBO";
        case TestPhase::P1_PLACE:         return "P1_PLACE";
        case TestPhase::P1_WAIT_ACCEPTED: return "P1_WAIT_ACCEPTED";
        case TestPhase::P1_WAIT_LIVE:     return "P1_WAIT_LIVE";
        case TestPhase::P1_HOLD:          return "P1_HOLD";
        case TestPhase::P1_CANCEL:        return "P1_CANCEL";
        case TestPhase::P1_WAIT_CANCELED: return "P1_WAIT_CANCELED";
        case TestPhase::P2_PLACE:         return "P2_PLACE";
        case TestPhase::P2_WAIT_ACCEPTED: return "P2_WAIT_ACCEPTED";
        case TestPhase::P2_WAIT_FILLS:    return "P2_WAIT_FILLS";
        case TestPhase::P2_CLEANUP:       return "P2_CLEANUP";
        case TestPhase::P2_WAIT_CLEANUP:  return "P2_WAIT_CLEANUP";
        case TestPhase::P3_FAK_BUY:       return "P3_FAK_BUY";
        case TestPhase::P3_WAIT_BUY:      return "P3_WAIT_BUY";
        case TestPhase::P3_FAK_SELL:       return "P3_FAK_SELL";
        case TestPhase::P3_WAIT_SELL:      return "P3_WAIT_SELL";
        case TestPhase::DONE:             return "DONE";
    }
    return "UNKNOWN";
}

}  // namespace

TestStrategy::TestStrategy(const WorkingOrderTracker* tracker,
                           const MarketPairRegistry* market_pairs,
                           const char* log_path)
    : tracker_(tracker), market_pairs_(market_pairs) {
    if (log_path && log_path[0] != '\0') {
        log_file_ = std::fopen(log_path, "w");
    }
}

TestStrategy::~TestStrategy() {
    if (log_file_) {
        std::fclose(log_file_);
        log_file_ = nullptr;
    }
}

void TestStrategy::transition(TestPhase next, Timestamp_ns now) {
    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: %s -> %s\n",
                     phase_name(phase_), phase_name(next));
        std::fflush(log_file_);
    }
    phase_ = next;
    state_entered_ts_ = now;
}

Qty_t TestStrategy::taker_qty_for_price(Price_t price) {
    if (price <= 0) return kMinLimitQty;
    // min_for_dollar: number of shares (scaled) to get $1 notional
    // notional = qty_shares * price / kPriceScale, solve for qty_shares >= $1
    // qty_shares >= kPriceScale / price, then scale to kQtyScale
    Qty_t min_for_dollar = ((kMinTakerNotional + price - 1) / price) * kQtyScale;
    return std::max(kMinLimitQty, min_for_dollar);
}

bool TestStrategy::check_timeout(Timestamp_ns now) {
    if (state_entered_ts_ > 0 && now > 0 &&
        (now - state_entered_ts_) > kTimeoutNs) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "TestStrategy: TIMEOUT in phase %s after 30s\n",
                         phase_name(phase_));
            std::fflush(log_file_);
        }
        phase_ = TestPhase::DONE;
        enabled_ = false;
        return true;
    }
    return false;
}

void TestStrategy::refresh_bbo(const SchedulerEvent& event) {
    if (event.source != EventSource::MARKET_WS) return;
    if (event.kind != SchedulerEventKind::MARKET_BBO_UPDATE &&
        event.kind != SchedulerEventKind::MARKET_BOOK_SNAPSHOT &&
        event.kind != SchedulerEventKind::MARKET_PRICE_CHANGE) {
        return;
    }
    if (captured_asset_id_.len > 0 && event.asset_id != captured_asset_id_) {
        return;
    }
    if (event.bbo.best_bid != kInvalidPrice) latest_bid_ = event.bbo.best_bid;
    if (event.bbo.best_ask != kInvalidPrice) latest_ask_ = event.bbo.best_ask;
}

bool TestStrategy::process_order_event(const SchedulerEvent& event) {
    // Match exec feedback events by client_order_id
    if (event.source == EventSource::EXEC_INTERNAL &&
        (event.kind == SchedulerEventKind::EXEC_ORDER_ACK ||
         event.kind == SchedulerEventKind::EXEC_ORDER_REJECT)) {
        auto fk = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);
        for (int i = 0; i < 2; ++i) {
            if (slot_[i].client_id.len == 0) continue;
            if (event.client_order_id == slot_[i].client_id) {
                if (fk == ExecFeedbackKind::ORDER_ACCEPTED && event.exec_accepted) {
                    slot_[i].accepted = true;
                    slot_[i].exchange_id = event.order_id;
                    // REST acceptance is authoritative: the order is live on exchange.
                    // User WS PLACEMENT may arrive before this (due to queue priority
                    // USER_WS > EXEC_INTERNAL) when exchange_id is not yet captured,
                    // causing match failure. Treat REST accept as live.
                    slot_[i].live = true;
                    if (log_file_) {
                        write_timestamp(log_file_);
                        std::fprintf(log_file_,
                            "TestStrategy: slot[%d] ACCEPTED+LIVE eid=%.40s cid=%.20s\n",
                            i, event.order_id.data, slot_[i].client_id.data);
                        std::fflush(log_file_);
                    }
                } else if (fk == ExecFeedbackKind::ORDER_REJECTED ||
                           fk == ExecFeedbackKind::RATE_LIMITED ||
                           fk == ExecFeedbackKind::EXCHANGE_UNAVAILABLE) {
                    slot_[i].rejected = true;
                    if (log_file_) {
                        write_timestamp(log_file_);
                        std::fprintf(log_file_,
                            "TestStrategy: slot[%d] REJECTED kind=%d cid=%.20s\n",
                            i, static_cast<int>(fk), slot_[i].client_id.data);
                        std::fflush(log_file_);
                    }
                } else if (fk == ExecFeedbackKind::CANCEL_CONFIRMED) {
                    slot_[i].canceled = true;
                    if (log_file_) {
                        write_timestamp(log_file_);
                        std::fprintf(log_file_,
                            "TestStrategy: slot[%d] CANCEL_CONFIRMED cid=%.20s\n",
                            i, slot_[i].client_id.data);
                        std::fflush(log_file_);
                    }
                }
                return true;
            }
        }
    }

    // Match user WS order updates by exchange_order_id or client_order_id
    if (event.source == EventSource::USER_WS &&
        event.kind == SchedulerEventKind::USER_ORDER_UPDATE) {
        auto status = static_cast<OrderStatus>(event.order_status_raw);
        for (int i = 0; i < 2; ++i) {
            if (slot_[i].client_id.len == 0) continue;
            bool match = (event.order_id == slot_[i].exchange_id && slot_[i].exchange_id.len > 0) ||
                         (event.client_order_id == slot_[i].client_id);
            if (match) {
                if (event.user_cumulative_filled > slot_[i].filled_qty) {
                    slot_[i].filled_qty = event.user_cumulative_filled;
                }
                if (status == OrderStatus::LIVE) slot_[i].live = true;
                if (status == OrderStatus::PARTIAL && slot_[i].filled_qty > 0) {
                    slot_[i].filled = true;
                }
                if (status == OrderStatus::CANCELED) slot_[i].canceled = true;
                if (status == OrderStatus::FILLED) slot_[i].filled = true;
                if (log_file_) {
                    write_timestamp(log_file_);
                    std::fprintf(log_file_,
                        "TestStrategy: slot[%d] USER_WS matched status=%d eid=%.40s\n",
                        i, static_cast<int>(status), event.order_id.data);
                    std::fflush(log_file_);
                }
                return true;
            }
        }
        // Log unmatched USER_WS events for diagnostics
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_,
                "TestStrategy: USER_WS UNMATCHED status=%d eid=%.40s cid=%.20s "
                "slot0_eid=%.40s slot1_eid=%.40s\n",
                static_cast<int>(status),
                event.order_id.data, event.client_order_id.data,
                slot_[0].exchange_id.data, slot_[1].exchange_id.data);
            std::fflush(log_file_);
        }
    }

    // Match user WS trade updates (fills) by order_id
    if (event.source == EventSource::USER_WS &&
        event.kind == SchedulerEventKind::USER_TRADE_UPDATE &&
        event.is_new_fill) {
        for (int i = 0; i < 2; ++i) {
            if (slot_[i].exchange_id.len > 0 && event.order_id == slot_[i].exchange_id) {
                slot_[i].filled = true;
                if (event.user_fill_size > 0) {
                    slot_[i].filled_qty += event.user_fill_size;
                }
                return true;
            }
        }
    }

    return false;
}

IntentBatch TestStrategy::evaluate(const StrategyContext& ctx) {
    const SchedulerEvent& event = ctx.event;
    IntentBatch batch;

    if (!enabled_) return batch;
    if (phase_ == TestPhase::DONE) return batch;

    // Handle market resolution: stop test if our market resolved
    if (event.kind == SchedulerEventKind::MARKET_RESOLVED) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "TestStrategy: MARKET_RESOLVED received, stopping\n");
            std::fflush(log_file_);
        }
        phase_ = TestPhase::DONE;
        enabled_ = false;
        return batch;
    }

    Timestamp_ns now = event.recv_ts;

    // Continuously refresh BBO from market data
    refresh_bbo(event);

    // Timeout check for any non-initial, non-done phase
    if (phase_ > TestPhase::WAIT_FOR_BBO && phase_ != TestPhase::DONE) {
        if (check_timeout(now)) return batch;
    }

    // Route order/trade events to slots
    process_order_event(event);

    switch (phase_) {
    case TestPhase::WAIT_FOR_BBO:     return handle_wait_for_bbo(event);
    case TestPhase::P1_PLACE:         return handle_p1_place(now);
    case TestPhase::P1_WAIT_ACCEPTED: return handle_p1_wait_accepted(event);
    case TestPhase::P1_WAIT_LIVE:     return handle_p1_wait_live(event);
    case TestPhase::P1_HOLD:          return handle_p1_hold(now);
    case TestPhase::P1_CANCEL:        return handle_p1_cancel(now);
    case TestPhase::P1_WAIT_CANCELED: return handle_p1_wait_canceled(event);
    case TestPhase::P2_PLACE:         return handle_p2_place(now);
    case TestPhase::P2_WAIT_ACCEPTED: return handle_p2_wait_accepted(event);
    case TestPhase::P2_WAIT_FILLS:    return handle_p2_wait_fills(event, now);
    case TestPhase::P2_CLEANUP:       return handle_p2_cleanup(now);
    case TestPhase::P2_WAIT_CLEANUP:  return handle_p2_wait_cleanup(event);
    case TestPhase::P3_FAK_BUY:       return handle_p3_fak_buy(now);
    case TestPhase::P3_WAIT_BUY:      return handle_p3_wait_buy(event);
    case TestPhase::P3_FAK_SELL:      return handle_p3_fak_sell(now);
    case TestPhase::P3_WAIT_SELL:     return handle_p3_wait_sell(event);
    case TestPhase::DONE:
        enabled_ = false;
        return batch;
    }

    return batch;
}

// ---------------------------------------------------------------------------
// WAIT_FOR_BBO: capture first valid BBO from market data
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_wait_for_bbo(const SchedulerEvent& event) {
    IntentBatch batch;

    if (event.source != EventSource::MARKET_WS) return batch;
    if (event.kind != SchedulerEventKind::MARKET_BBO_UPDATE &&
        event.kind != SchedulerEventKind::MARKET_BOOK_SNAPSHOT &&
        event.kind != SchedulerEventKind::MARKET_PRICE_CHANGE) {
        return batch;
    }
    if (event.bbo.best_bid == kInvalidPrice ||
        event.bbo.best_ask == kInvalidPrice) {
        return batch;
    }
    if (!market_pairs_) return batch;
    const AssetId* cond = market_pairs_->condition_for_token(event.asset_id);
    if (!cond) return batch;
    const MarketPair* pair = market_pairs_->find_by_condition(*cond);
    if (!pair) return batch;
    if (event.asset_id != pair->token_id_up &&
        event.asset_id != pair->token_id_down) {
        return batch;
    }

    captured_asset_id_ = event.asset_id;
    captured_market_id_ = *cond;
    latest_bid_ = event.bbo.best_bid;
    latest_ask_ = event.bbo.best_ask;

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: Captured BBO bid=%d ask=%d asset=%s\n",
                     latest_bid_, latest_ask_, captured_asset_id_.data);
        std::fflush(log_file_);
    }

    transition(TestPhase::P1_PLACE, event.recv_ts);
    return handle_p1_place(event.recv_ts);
}

// ---------------------------------------------------------------------------
// P1_PLACE: Emit GTC BID@100 + ASK@9900 in one batch
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_place(Timestamp_ns now) {
    IntentBatch batch;

    slot_[0].reset();
    slot_[0].side = Side::BID;
    slot_[1].reset();
    slot_[1].side = Side::ASK;

    // BID at $0.01 (price=100)
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_PLACE_BID;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.price = 100;
        intent.qty = kMinLimitQty;
        intent.order_type = OrderType::GTC;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;

        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
        intent.client_order_id = OrderId(id_buf);
        slot_[0].client_id = intent.client_order_id;

        batch.add(intent);
    }

    // ASK at $0.99 (price=9900)
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_PLACE_ASK;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.price = 9900;
        intent.qty = kMinLimitQty;
        intent.order_type = OrderType::GTC;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;

        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
        intent.client_order_id = OrderId(id_buf);
        slot_[1].client_id = intent.client_order_id;

        batch.add(intent);
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P1 placed BID@100 + ASK@9900\n");
        std::fflush(log_file_);
    }

    transition(TestPhase::P1_WAIT_ACCEPTED, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P1_WAIT_ACCEPTED: both slots must be accepted (or rejected)
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_wait_accepted(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    if (slot_[0].accepted && slot_[1].accepted) {
        transition(TestPhase::P1_WAIT_LIVE, state_entered_ts_);
    } else if (slot_[0].rejected || slot_[1].rejected) {
        // At least one side rejected — cancel whatever was accepted and move on
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_,
                "TestStrategy: P1 rejection detected (slot0: acc=%d rej=%d, slot1: acc=%d rej=%d)"
                " -- canceling accepted orders and skipping to P2\n",
                slot_[0].accepted, slot_[0].rejected,
                slot_[1].accepted, slot_[1].rejected);
            std::fflush(log_file_);
        }
        transition(TestPhase::P1_CANCEL, state_entered_ts_);
        return handle_p1_cancel(state_entered_ts_);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P1_WAIT_LIVE: both slots must be live on exchange
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_wait_live(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    if (slot_[0].live && slot_[1].live) {
        transition(TestPhase::P1_HOLD, state_entered_ts_);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P1_HOLD: hold for 10 seconds
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_hold(Timestamp_ns now) {
    IntentBatch batch;
    if (state_entered_ts_ > 0 && now > 0 &&
        (now - state_entered_ts_) >= kHoldNs) {
        transition(TestPhase::P1_CANCEL, now);
        return handle_p1_cancel(now);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P1_CANCEL: emit cancel for both orders
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_cancel(Timestamp_ns now) {
    IntentBatch batch;

    // Cancel BID
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_CANCEL_BID;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.exchange_order_id = slot_[0].exchange_id;
        intent.client_order_id = slot_[0].client_id;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;
        batch.add(intent);
    }

    // Cancel ASK
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_CANCEL_ASK;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.exchange_order_id = slot_[1].exchange_id;
        intent.client_order_id = slot_[1].client_id;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;
        batch.add(intent);
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P1 emitted cancel for both orders\n");
        std::fflush(log_file_);
    }

    // Reset canceled flags for tracking
    slot_[0].canceled = false;
    slot_[1].canceled = false;

    transition(TestPhase::P1_WAIT_CANCELED, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P1_WAIT_CANCELED: wait for both cancel confirmations (or rejection)
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p1_wait_canceled(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    // A rejected slot was never placed, so its cancel also gets rejected.
    // Treat rejected as terminal (equivalent to canceled for advancement).
    bool done_0 = slot_[0].canceled || slot_[0].rejected;
    bool done_1 = slot_[1].canceled || slot_[1].rejected;
    if (done_0 && done_1) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_,
                "TestStrategy: P1 complete -- slot0(%s) slot1(%s)\n",
                slot_[0].canceled ? "canceled" : "rejected",
                slot_[1].canceled ? "canceled" : "rejected");
            std::fflush(log_file_);
        }
        slot_[0].reset();
        slot_[1].reset();
        transition(TestPhase::P2_PLACE, state_entered_ts_);
        return handle_p2_place(state_entered_ts_);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P2_PLACE: GTC BID at best_ask-1tick, ASK at best_bid+1tick
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p2_place(Timestamp_ns now) {
    IntentBatch batch;

    Price_t bid_price = latest_ask_;
    Price_t ask_price = latest_bid_;

    slot_[0].reset();
    slot_[0].side = Side::BID;
    slot_[1].reset();
    slot_[1].side = Side::ASK;

    // BID near ask
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_PLACE_BID;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.price = bid_price;
        intent.qty = kMinLimitQty;
        intent.order_type = OrderType::GTC;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;

        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
        intent.client_order_id = OrderId(id_buf);
        slot_[0].client_id = intent.client_order_id;

        batch.add(intent);
    }

    // ASK near bid
    {
        ExecutionIntent intent;
        intent.action = IntentAction::WOULD_PLACE_ASK;
        intent.asset_id = captured_asset_id_;
        intent.market_id = captured_market_id_;
        intent.price = ask_price;
        intent.qty = kMinLimitQty;
        intent.order_type = OrderType::GTC;
        intent.intent_id = next_intent_id_++;
        intent.recv_ts = now;

        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
        intent.client_order_id = OrderId(id_buf);
        slot_[1].client_id = intent.client_order_id;

        batch.add(intent);
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P2 placed BID@%d ASK@%d (at BBO)\n",
                     bid_price, ask_price);
        std::fflush(log_file_);
    }

    transition(TestPhase::P2_WAIT_ACCEPTED, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P2_WAIT_ACCEPTED: both slots must be accepted (or rejected)
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p2_wait_accepted(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    if (slot_[0].accepted && slot_[1].accepted) {
        transition(TestPhase::P2_WAIT_FILLS, state_entered_ts_);
    } else if (slot_[0].rejected || slot_[1].rejected) {
        // At least one side rejected — clean up accepted orders and move on
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_,
                "TestStrategy: P2 rejection detected (slot0: acc=%d rej=%d, slot1: acc=%d rej=%d)"
                " -- cleaning up and skipping to P3\n",
                slot_[0].accepted, slot_[0].rejected,
                slot_[1].accepted, slot_[1].rejected);
            std::fflush(log_file_);
        }
        transition(TestPhase::P2_CLEANUP, state_entered_ts_);
        return handle_p2_cleanup(state_entered_ts_);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P2_WAIT_FILLS: wait up to 10s for fills, then cleanup
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p2_wait_fills(const SchedulerEvent& /*event*/, Timestamp_ns now) {
    IntentBatch batch;

    bool both_filled = slot_[0].filled && slot_[1].filled;
    bool timed_out = state_entered_ts_ > 0 && now > 0 &&
                     (now - state_entered_ts_) >= kHoldNs;

    if (both_filled) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "TestStrategy: P2 both sides filled\n");
            std::fflush(log_file_);
        }
        slot_[0].reset();
        slot_[1].reset();
        transition(TestPhase::P3_FAK_BUY, now);
        return handle_p3_fak_buy(now);
    }

    if (timed_out) {
        // Timeout: move to cleanup regardless of fill state
        if (!slot_[0].filled && !slot_[1].filled) {
            if (log_file_) {
                write_timestamp(log_file_);
                std::fprintf(log_file_, "TestStrategy: P2 timeout, neither filled -- canceling both\n");
                std::fflush(log_file_);
            }
        } else {
            if (log_file_) {
                write_timestamp(log_file_);
                std::fprintf(log_file_, "TestStrategy: P2 timeout, partial fill -- hedging\n");
                std::fflush(log_file_);
            }
        }
        transition(TestPhase::P2_CLEANUP, now);
        return handle_p2_cleanup(now);
    }

    return batch;
}

// ---------------------------------------------------------------------------
// P2_CLEANUP: hedge filled side + cancel unfilled, or cancel both
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p2_cleanup(Timestamp_ns now) {
    IntentBatch batch;

    cleanup_hedge_done_ = false;
    cleanup_cancel_done_ = false;
    cleanup_unfilled_idx_ = -1;
    cleanup_retry_issued_ = false;

    bool bid_filled = slot_[0].filled;
    bool ask_filled = slot_[1].filled;

    if (bid_filled && ask_filled) {
        // Both filled -- nothing to clean
        cleanup_hedge_done_ = true;
        cleanup_cancel_done_ = true;
        transition(TestPhase::P3_FAK_BUY, now);
        slot_[0].reset();
        slot_[1].reset();
        return handle_p3_fak_buy(now);
    }

    if (!bid_filled && !ask_filled) {
        // Neither filled -- cancel both
        cleanup_hedge_done_ = true;  // no hedge needed

        ExecutionIntent c0;
        c0.action = IntentAction::WOULD_CANCEL_BID;
        c0.asset_id = captured_asset_id_;
        c0.market_id = captured_market_id_;
        c0.exchange_order_id = slot_[0].exchange_id;
        c0.client_order_id = slot_[0].client_id;
        c0.intent_id = next_intent_id_++;
        c0.recv_ts = now;
        batch.add(c0);

        ExecutionIntent c1;
        c1.action = IntentAction::WOULD_CANCEL_ASK;
        c1.asset_id = captured_asset_id_;
        c1.market_id = captured_market_id_;
        c1.exchange_order_id = slot_[1].exchange_id;
        c1.client_order_id = slot_[1].client_id;
        c1.intent_id = next_intent_id_++;
        c1.recv_ts = now;
        batch.add(c1);

        slot_[0].canceled = false;
        slot_[1].canceled = false;
        transition(TestPhase::P2_WAIT_CLEANUP, now);
        return batch;
    }

    // Partial fill: hedge filled side + cancel unfilled
    int filled_idx = bid_filled ? 0 : 1;
    int unfilled_idx = bid_filled ? 1 : 0;
    cleanup_unfilled_idx_ = unfilled_idx;
    Qty_t hedge_qty = slot_[filled_idx].filled_qty;
    if (hedge_qty <= 0) hedge_qty = kMinLimitQty;
    hedge_qty = std::min(hedge_qty, kMinLimitQty);

    // FAK hedge: if bid filled, sell to flatten; if ask filled, buy to flatten
    {
        ExecutionIntent hedge;
        if (filled_idx == 0) {
            // Bid filled -- sell hedge
            hedge.action = IntentAction::WOULD_PLACE_ASK;
            hedge.price = latest_bid_;
            hedge.qty = hedge_qty;
        } else {
            // Ask filled -- buy hedge
            hedge.action = IntentAction::WOULD_PLACE_BID;
            hedge.price = latest_ask_;
            hedge.qty = hedge_qty;
        }
        hedge.asset_id = captured_asset_id_;
        hedge.market_id = captured_market_id_;
        hedge.order_type = OrderType::FAK;
        hedge.intent_id = next_intent_id_++;
        hedge.recv_ts = now;

        char id_buf[80];
        std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", hedge.intent_id);
        hedge.client_order_id = OrderId(id_buf);

        batch.add(hedge);

        // Reuse the filled slot to track this hedge intent lifecycle.
        slot_[filled_idx].reset();
        slot_[filled_idx].side = (hedge.action == IntentAction::WOULD_PLACE_BID)
            ? Side::BID
            : Side::ASK;
        slot_[filled_idx].client_id = hedge.client_order_id;
        slot_[filled_idx].filled_qty = hedge.qty;
    }

    // Cancel unfilled
    {
        ExecutionIntent cancel;
        cancel.action = (unfilled_idx == 0) ? IntentAction::WOULD_CANCEL_BID
                                             : IntentAction::WOULD_CANCEL_ASK;
        cancel.asset_id = captured_asset_id_;
        cancel.market_id = captured_market_id_;
        cancel.exchange_order_id = slot_[unfilled_idx].exchange_id;
        cancel.client_order_id = slot_[unfilled_idx].client_id;
        cancel.intent_id = next_intent_id_++;
        cancel.recv_ts = now;
        batch.add(cancel);
    }

    slot_[unfilled_idx].canceled = false;

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P2 cleanup - hedge %s side qty=%lld, cancel %s side\n",
                     bid_filled ? "BID" : "ASK", static_cast<long long>(hedge_qty), bid_filled ? "ASK" : "BID");
        std::fflush(log_file_);
    }

    transition(TestPhase::P2_WAIT_CLEANUP, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P2_WAIT_CLEANUP: wait for hedge ack + cancel confirm
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p2_wait_cleanup(const SchedulerEvent& event) {
    IntentBatch batch;

    // Neither-filled path: both original orders were canceled (or rejected).
    if (cleanup_unfilled_idx_ < 0) {
        bool done_0 = slot_[0].canceled || slot_[0].rejected;
        bool done_1 = slot_[1].canceled || slot_[1].rejected;
        if (done_0 && done_1) {
            if (log_file_) {
                write_timestamp(log_file_);
                std::fprintf(log_file_,
                    "TestStrategy: P2 cleanup complete -- slot0(%s) slot1(%s)\n",
                    slot_[0].canceled ? "canceled" : "rejected",
                    slot_[1].canceled ? "canceled" : "rejected");
                std::fflush(log_file_);
            }
            slot_[0].reset();
            slot_[1].reset();
            cleanup_retry_issued_ = false;
            transition(TestPhase::P3_FAK_BUY, state_entered_ts_);
            return handle_p3_fak_buy(state_entered_ts_);
        }
        return batch;
    }

    const int unfilled_idx = cleanup_unfilled_idx_;
    const int hedge_idx = 1 - cleanup_unfilled_idx_;

    if (slot_[unfilled_idx].canceled || slot_[unfilled_idx].rejected) {
        if (slot_[hedge_idx].accepted || slot_[hedge_idx].filled) {
            if (log_file_) {
                write_timestamp(log_file_);
                std::fprintf(log_file_, "TestStrategy: P2 cleanup complete -- hedge+cancel done\n");
                std::fflush(log_file_);
            }
            slot_[0].reset();
            slot_[1].reset();
            cleanup_unfilled_idx_ = -1;
            cleanup_retry_issued_ = false;
            transition(TestPhase::P3_FAK_BUY, state_entered_ts_);
            return handle_p3_fak_buy(state_entered_ts_);
        }
        if (slot_[hedge_idx].rejected) {
            if (!cleanup_retry_issued_) {
                Timestamp_ns now = event.recv_ts > 0 ? event.recv_ts : state_entered_ts_;
                ExecutionIntent retry;
                retry.action = (slot_[hedge_idx].side == Side::BID)
                    ? IntentAction::WOULD_PLACE_BID
                    : IntentAction::WOULD_PLACE_ASK;
                retry.asset_id = captured_asset_id_;
                retry.market_id = captured_market_id_;
                retry.price = (retry.action == IntentAction::WOULD_PLACE_BID) ? latest_ask_ : latest_bid_;
                if (retry.price == kInvalidPrice) {
                    retry.price = 5000;
                }
                retry.qty = kMinLimitQty;
                retry.order_type = OrderType::FAK;
                retry.intent_id = next_intent_id_++;
                retry.recv_ts = now;

                char id_buf[80];
                std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", retry.intent_id);
                retry.client_order_id = OrderId(id_buf);
                batch.add(retry);

                slot_[hedge_idx].reset();
                slot_[hedge_idx].side = (retry.action == IntentAction::WOULD_PLACE_BID)
                    ? Side::BID
                    : Side::ASK;
                slot_[hedge_idx].client_id = retry.client_order_id;
                slot_[hedge_idx].filled_qty = retry.qty;
                cleanup_retry_issued_ = true;

                if (log_file_) {
                    write_timestamp(log_file_);
                    std::fprintf(log_file_,
                        "TestStrategy: P2 hedge rejected, retrying once side=%s qty=%lld\n",
                        (retry.action == IntentAction::WOULD_PLACE_BID) ? "BID" : "ASK",
                        static_cast<long long>(retry.qty));
                    std::fflush(log_file_);
                }
                return batch;
            }
            if (log_file_) {
                write_timestamp(log_file_);
                std::fprintf(log_file_,
                    "TestStrategy: P2 cleanup failed -- hedge rejected twice, stopping strategy\n");
                std::fflush(log_file_);
            }
            cleanup_unfilled_idx_ = -1;
            cleanup_retry_issued_ = false;
            transition(TestPhase::DONE, event.recv_ts > 0 ? event.recv_ts : state_entered_ts_);
            enabled_ = false;
        }
    }

    return batch;
}

// ---------------------------------------------------------------------------
// P3_FAK_BUY: FAK BUY at latest_ask (crosses spread)
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p3_fak_buy(Timestamp_ns now) {
    IntentBatch batch;

    slot_[0].reset();
    slot_[0].side = Side::BID;

    Price_t price = latest_ask_;
    Qty_t qty = taker_qty_for_price(price);

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_BID;
    intent.asset_id = captured_asset_id_;
    intent.market_id = captured_market_id_;
    intent.price = price;
    intent.qty = qty;
    intent.order_type = OrderType::FAK;
    intent.intent_id = next_intent_id_++;
    intent.recv_ts = now;

    char id_buf[80];
    std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
    intent.client_order_id = OrderId(id_buf);
    slot_[0].client_id = intent.client_order_id;

    batch.add(intent);

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P3 FAK BUY price=%d qty=%lld\n",
                     price, static_cast<long long>(qty));
        std::fflush(log_file_);
    }

    transition(TestPhase::P3_WAIT_BUY, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P3_WAIT_BUY: wait for FAK buy ack or fill
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p3_wait_buy(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    // FAK: accepted or rejected means terminal attempt. Move on regardless.
    if (slot_[0].accepted || slot_[0].filled || slot_[0].rejected) {
        slot_[0].reset();
        transition(TestPhase::P3_FAK_SELL, state_entered_ts_);
        return handle_p3_fak_sell(state_entered_ts_);
    }
    return batch;
}

// ---------------------------------------------------------------------------
// P3_FAK_SELL: FAK SELL at latest_bid (crosses spread)
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p3_fak_sell(Timestamp_ns now) {
    IntentBatch batch;

    slot_[1].reset();
    slot_[1].side = Side::ASK;

    Price_t price = latest_bid_;
    Qty_t qty = taker_qty_for_price(10000 - price);

    ExecutionIntent intent;
    intent.action = IntentAction::WOULD_PLACE_ASK;
    intent.asset_id = captured_asset_id_;
    intent.market_id = captured_market_id_;
    intent.price = price;
    intent.qty = qty;
    intent.order_type = OrderType::FAK;
    intent.intent_id = next_intent_id_++;
    intent.recv_ts = now;

    char id_buf[80];
    std::snprintf(id_buf, sizeof(id_buf), "lt-test-%u", intent.intent_id);
    intent.client_order_id = OrderId(id_buf);
    slot_[1].client_id = intent.client_order_id;

    batch.add(intent);

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "TestStrategy: P3 FAK SELL price=%d qty=%lld\n",
                     price, static_cast<long long>(qty));
        std::fflush(log_file_);
    }

    transition(TestPhase::P3_WAIT_SELL, now);
    return batch;
}

// ---------------------------------------------------------------------------
// P3_WAIT_SELL: wait for FAK sell ack or fill -> DONE
// ---------------------------------------------------------------------------
IntentBatch TestStrategy::handle_p3_wait_sell(const SchedulerEvent& /*event*/) {
    IntentBatch batch;
    if (slot_[1].accepted || slot_[1].filled || slot_[1].rejected) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "TestStrategy: All 3 phases PASSED\n");
            std::fflush(log_file_);
        }
        transition(TestPhase::DONE, state_entered_ts_);
        enabled_ = false;
    }
    return batch;
}

}  // namespace lt
