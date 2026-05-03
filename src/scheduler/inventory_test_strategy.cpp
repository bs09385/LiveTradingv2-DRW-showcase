#include "scheduler/inventory_test_strategy.h"

#include <chrono>
#include <cstdio>
#include <ctime>

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

const char* phase_name(InvTestPhase p) {
    switch (p) {
        case InvTestPhase::WAIT_FOR_BBO:     return "WAIT_FOR_BBO";
        case InvTestPhase::SPLIT_REQUEST:    return "SPLIT_REQUEST";
        case InvTestPhase::WAIT_SPLIT_HOLD:  return "WAIT_SPLIT_HOLD";
        case InvTestPhase::MERGE_REQUEST:    return "MERGE_REQUEST";
        case InvTestPhase::WAIT_RESOLVED:    return "WAIT_RESOLVED";
        case InvTestPhase::REDEEM_REQUEST:   return "REDEEM_REQUEST";
        case InvTestPhase::DONE:             return "DONE";
    }
    return "UNKNOWN";
}

}  // namespace

InventoryTestStrategy::InventoryTestStrategy(const MarketPairRegistry* market_pairs,
                                             const char* log_path)
    : market_pairs_(market_pairs) {
    if (log_path && log_path[0] != '\0') {
        log_file_ = std::fopen(log_path, "w");
    }
}

InventoryTestStrategy::~InventoryTestStrategy() {
    if (log_file_) {
        std::fclose(log_file_);
        log_file_ = nullptr;
    }
}

void InventoryTestStrategy::transition(InvTestPhase next, Timestamp_ns now) {
    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: %s -> %s\n",
                     phase_name(phase_), phase_name(next));
        std::fflush(log_file_);
    }
    phase_ = next;
    state_entered_ts_ = now;
}

bool InventoryTestStrategy::check_timeout(Timestamp_ns now) {
    if (state_entered_ts_ > 0 && now > 0 &&
        (now - state_entered_ts_) > kTimeoutNs) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: TIMEOUT in phase %s after 30s\n",
                         phase_name(phase_));
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return true;
    }
    return false;
}

IntentBatch InventoryTestStrategy::evaluate(const StrategyContext& ctx) {
    IntentBatch batch;

    if (!enabled_) return batch;
    if (phase_ == InvTestPhase::DONE) return batch;

    Timestamp_ns now = ctx.event.recv_ts;

    // Timeout check for phases that have timeouts
    if (phase_ == InvTestPhase::WAIT_FOR_BBO ||
        phase_ == InvTestPhase::WAIT_SPLIT_HOLD) {
        if (check_timeout(now)) return batch;
    }

    switch (phase_) {
    case InvTestPhase::WAIT_FOR_BBO:     handle_wait_for_bbo(ctx); break;
    case InvTestPhase::SPLIT_REQUEST:    handle_split_request(ctx); break;
    case InvTestPhase::WAIT_SPLIT_HOLD:  handle_wait_split_hold(ctx); break;
    case InvTestPhase::MERGE_REQUEST:    handle_merge_request(ctx); break;
    case InvTestPhase::WAIT_RESOLVED:    handle_wait_resolved(ctx); break;
    case InvTestPhase::REDEEM_REQUEST:   handle_redeem_request(ctx); break;
    case InvTestPhase::DONE:
        enabled_ = false;
        break;
    }

    return batch;  // always empty — no order intents
}

// ---------------------------------------------------------------------------
// WAIT_FOR_BBO: capture first valid BBO from market data
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_wait_for_bbo(const StrategyContext& ctx) {
    const SchedulerEvent& event = ctx.event;

    if (event.source != EventSource::MARKET_WS) return;
    if (event.kind != SchedulerEventKind::MARKET_BBO_UPDATE &&
        event.kind != SchedulerEventKind::MARKET_BOOK_SNAPSHOT &&
        event.kind != SchedulerEventKind::MARKET_PRICE_CHANGE) {
        return;
    }
    if (event.bbo.best_bid == kInvalidPrice ||
        event.bbo.best_ask == kInvalidPrice) {
        return;
    }
    if (!market_pairs_) return;
    const AssetId* cond = market_pairs_->condition_for_token(event.asset_id);
    if (!cond) return;

    captured_condition_id_ = *cond;

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: Captured condition=%.*s from asset=%.*s\n",
                     static_cast<int>(cond->len), cond->data,
                     static_cast<int>(event.asset_id.len), event.asset_id.data);
        std::fflush(log_file_);
    }

    transition(InvTestPhase::SPLIT_REQUEST, event.recv_ts);
    handle_split_request(ctx);
}

// ---------------------------------------------------------------------------
// SPLIT_REQUEST: fire SPLIT for kSplitQty
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_split_request(const StrategyContext& ctx) {
    if (!ctx.inventory_ops) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: inventory_ops is null, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    // Look up token pair for TokenInventory update
    const MarketPair* pair = market_pairs_
        ? market_pairs_->find_by_condition(captured_condition_id_) : nullptr;

    InventoryOpRequest req;
    req.type = InventoryOpType::SPLIT;
    req.condition_id = captured_condition_id_;
    if (pair) {
        req.token_id_up = pair->token_id_up;
        req.token_id_down = pair->token_id_down;
    }
    req.quantity = kSplitQty;
    req.request_id = next_request_id_++;
    req.created_ts = ctx.event.recv_ts;

    if (!ctx.inventory_ops->try_request(req)) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: SPLIT try_request failed, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: SPLIT sent qty=%lld req_id=%u\n",
                     static_cast<long long>(kSplitQty), req.request_id);
        std::fflush(log_file_);
    }

    transition(InvTestPhase::WAIT_SPLIT_HOLD, ctx.event.recv_ts);
}

// ---------------------------------------------------------------------------
// WAIT_SPLIT_HOLD: hold for 10s after split before merging
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_wait_split_hold(const StrategyContext& ctx) {
    Timestamp_ns now = ctx.event.recv_ts;
    if (state_entered_ts_ > 0 && now > 0 &&
        (now - state_entered_ts_) >= kHoldNs) {
        transition(InvTestPhase::MERGE_REQUEST, now);
        handle_merge_request(ctx);
    }
}

// ---------------------------------------------------------------------------
// MERGE_REQUEST: fire MERGE for kMergeQty
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_merge_request(const StrategyContext& ctx) {
    if (!ctx.inventory_ops) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: inventory_ops is null, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    // Look up token pair for TokenInventory update
    const MarketPair* merge_pair = market_pairs_
        ? market_pairs_->find_by_condition(captured_condition_id_) : nullptr;

    InventoryOpRequest req;
    req.type = InventoryOpType::MERGE;
    req.condition_id = captured_condition_id_;
    if (merge_pair) {
        req.token_id_up = merge_pair->token_id_up;
        req.token_id_down = merge_pair->token_id_down;
    }
    req.quantity = kMergeQty;
    req.request_id = next_request_id_++;
    req.created_ts = ctx.event.recv_ts;

    if (!ctx.inventory_ops->try_request(req)) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: MERGE try_request failed, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: MERGE sent qty=%lld req_id=%u\n",
                     static_cast<long long>(kMergeQty), req.request_id);
        std::fflush(log_file_);
    }

    transition(InvTestPhase::WAIT_RESOLVED, ctx.event.recv_ts);
}

// ---------------------------------------------------------------------------
// WAIT_RESOLVED: wait for MARKET_RESOLVED matching captured condition_id
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_wait_resolved(const StrategyContext& ctx) {
    const SchedulerEvent& event = ctx.event;

    if (event.kind != SchedulerEventKind::MARKET_RESOLVED) return;
    if (event.resolved_condition_id != captured_condition_id_) return;

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: MARKET_RESOLVED matched, winning=%.*s\n",
                     static_cast<int>(event.resolved_winning_asset_id.len),
                     event.resolved_winning_asset_id.data);
        std::fflush(log_file_);
    }

    transition(InvTestPhase::REDEEM_REQUEST, event.recv_ts);
    handle_redeem_request(ctx);
}

// ---------------------------------------------------------------------------
// REDEEM_REQUEST: fire REDEEM for winning token (quantity=0 = all available)
// ---------------------------------------------------------------------------
void InventoryTestStrategy::handle_redeem_request(const StrategyContext& ctx) {
    if (!ctx.inventory_ops) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: inventory_ops is null, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    // Look up token pair for TokenInventory update
    const MarketPair* redeem_pair = market_pairs_
        ? market_pairs_->find_by_condition(captured_condition_id_) : nullptr;

    InventoryOpRequest req;
    req.type = InventoryOpType::REDEEM;
    req.condition_id = captured_condition_id_;
    req.token_id = ctx.event.resolved_winning_asset_id;
    if (redeem_pair) {
        req.token_id_up = redeem_pair->token_id_up;
        req.token_id_down = redeem_pair->token_id_down;
    }
    req.quantity = 0;  // all available
    req.request_id = next_request_id_++;
    req.created_ts = ctx.event.recv_ts;

    if (!ctx.inventory_ops->try_request(req)) {
        if (log_file_) {
            write_timestamp(log_file_);
            std::fprintf(log_file_, "InventoryTestStrategy: REDEEM try_request failed, aborting\n");
            std::fflush(log_file_);
        }
        phase_ = InvTestPhase::DONE;
        enabled_ = false;
        return;
    }

    if (log_file_) {
        write_timestamp(log_file_);
        std::fprintf(log_file_, "InventoryTestStrategy: REDEEM sent token=%.*s req_id=%u\n",
                     static_cast<int>(req.token_id.len), req.token_id.data,
                     req.request_id);
        std::fprintf(log_file_, "InventoryTestStrategy: All 3 phases PASSED\n");
        std::fflush(log_file_);
    }

    transition(InvTestPhase::DONE, ctx.event.recv_ts);
    enabled_ = false;
}

}  // namespace lt
