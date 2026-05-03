#include "scheduler/working_order_tracker.h"

#include <limits>

#include "exec/exec_feedback.h"
#include "events/user_events.h"

namespace lt {

bool WorkingOrderTracker::on_intent_sent(const ExecutionIntent& intent) {
    // Only track placements
    if (intent.action != IntentAction::WOULD_PLACE_BID &&
        intent.action != IntentAction::WOULD_PLACE_ASK) {
        return true;
    }

    int slot = find_free_slot();
    if (slot < 0) return false;
    auto& wo = orders_[slot];
    wo.occupied = true;
    wo.client_order_id = intent.client_order_id;
    wo.exchange_order_id = OrderId{};
    wo.asset_id = intent.asset_id;
    wo.market_id = intent.market_id;
    wo.side = (intent.action == IntentAction::WOULD_PLACE_BID) ? Side::BID : Side::ASK;
    wo.price = intent.price;
    wo.original_size = intent.qty;
    wo.filled_size = 0;
    wo.sent_ts = intent.created_ts;
    wo.is_live = false;
    wo.is_pending = true;
    wo.is_terminal = false;
    wo.level = intent.level;
    return true;
}

void WorkingOrderTracker::on_exec_feedback(const SchedulerEvent& event) {
    if (event.source != EventSource::EXEC_INTERNAL) return;

    auto fb_kind = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);

    if (fb_kind == ExecFeedbackKind::ORDER_ACCEPTED) {
        // Find by client_order_id from the feedback
        int slot = find_slot_by_client_id(event.client_order_id);
        if (slot < 0) return;
        orders_[slot].is_pending = false;
        orders_[slot].is_live = true;
        // Set exchange_order_id if provided
        if (event.order_id.len > 0) {
            orders_[slot].exchange_order_id = event.order_id;
        }
    } else if (fb_kind == ExecFeedbackKind::ORDER_REJECTED ||
               fb_kind == ExecFeedbackKind::RATE_LIMITED ||
               fb_kind == ExecFeedbackKind::EXCHANGE_UNAVAILABLE) {
        // Order was definitively not placed -- remove from tracker.
        int slot = find_slot_by_client_id(event.client_order_id);
        if (slot < 0) return;
        remove_slot(slot);
    } else if (fb_kind == ExecFeedbackKind::TIMEOUT) {
        // Ambiguous: order may or may not have been placed.
        // Keep in tracker as pending -- will be resolved by user WS update
        // or removed by GC if no confirmation arrives within timeout.
    } else if (fb_kind == ExecFeedbackKind::CANCEL_CONFIRMED) {
        // Try exchange_order_id first, then client_order_id
        int slot = -1;
        if (event.order_id.len > 0) {
            slot = find_slot_by_exchange_id(event.order_id);
        }
        if (slot < 0 && event.client_order_id.len > 0) {
            slot = find_slot_by_client_id(event.client_order_id);
        }
        if (slot >= 0) remove_slot(slot);
    }
}

void WorkingOrderTracker::on_user_update(const SchedulerEvent& event) {
    if (event.source != EventSource::USER_WS) return;

    if (event.kind == SchedulerEventKind::USER_ORDER_UPDATE) {
        auto status = static_cast<OrderStatus>(event.order_status_raw);

        // Try to find order by exchange_order_id or client_order_id
        int slot = -1;
        if (event.order_id.len > 0) {
            slot = find_slot_by_exchange_id(event.order_id);
        }
        if (slot < 0 && event.client_order_id.len > 0) {
            slot = find_slot_by_client_id(event.client_order_id);
        }
        if (slot < 0) {
            // Fallback for late ID correlation: when user updates arrive before
            // tracker has exchange/client mapping, match by asset+side+price.
            slot = find_slot_for_user_update_fallback(event);
        }
        if (slot < 0) return;

        // Capture exchange ID whenever available (fixes late mapping races).
        if (event.order_id.len > 0) {
            orders_[slot].exchange_order_id = event.order_id;
        }

        if (status == OrderStatus::FILLED ||
            status == OrderStatus::CANCELED ||
            status == OrderStatus::FAILED) {
            remove_slot(slot);
        } else if (status == OrderStatus::PARTIAL) {
            if (event.user_cumulative_filled > orders_[slot].filled_size) {
                orders_[slot].filled_size = event.user_cumulative_filled;
            }
            orders_[slot].is_live = true;
            orders_[slot].is_pending = false;
        } else if (status == OrderStatus::LIVE) {
            orders_[slot].is_live = true;
            orders_[slot].is_pending = false;
        }
    } else if (event.kind == SchedulerEventKind::USER_TRADE_UPDATE) {
        // FAILED trade: check if order should be removed
        auto trade_status = static_cast<TradeStatus>(event.trade_status_raw);
        if (trade_status == TradeStatus::FAILED) {
            int slot = -1;
            if (event.order_id.len > 0) {
                slot = find_slot_by_exchange_id(event.order_id);
            }
            // Don't remove on FAILED — the order itself may still be live.
            // Only order-level events (FILLED/CANCELED) should remove.
            (void)slot;
        }
    }
}

int WorkingOrderTracker::working_count() const {
    int count = 0;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal) ++count;
    }
    return count;
}

int WorkingOrderTracker::working_count_for_market(const AssetId& market_id) const {
    int count = 0;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].market_id == market_id) {
            ++count;
        }
    }
    return count;
}

Price_t WorkingOrderTracker::working_bid_price(const AssetId& market_id) const {
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].market_id == market_id &&
            orders_[i].side == Side::BID) {
            return orders_[i].price;
        }
    }
    return kInvalidPrice;
}

Price_t WorkingOrderTracker::working_ask_price(const AssetId& market_id) const {
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].market_id == market_id &&
            orders_[i].side == Side::ASK) {
            return orders_[i].price;
        }
    }
    return kInvalidPrice;
}

int64_t WorkingOrderTracker::total_working_notional() const {
    int64_t total = 0;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal) {
            Qty_t remaining = orders_[i].original_size - orders_[i].filled_size;
            total += static_cast<int64_t>(orders_[i].price) * remaining;
        }
    }
    return total;
}

int WorkingOrderTracker::gc_stale_pending(Timestamp_ns now, int64_t max_age_ns) {
    int removed = 0;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (!orders_[i].occupied || orders_[i].is_terminal) continue;
        if (!orders_[i].is_pending) continue;
        if (orders_[i].sent_ts > 0 && now > orders_[i].sent_ts &&
            (now - orders_[i].sent_ts) > max_age_ns) {
            remove_slot(i);
            ++removed;
        }
    }
    return removed;
}

IntentBatch WorkingOrderTracker::cancel_all_intents() const {
    IntentBatch batch;
    for (int i = 0; i < kMaxWorking && batch.count < IntentBatch::kMaxIntents; ++i) {
        if (!orders_[i].occupied || orders_[i].is_terminal) continue;

        ExecutionIntent cancel;
        cancel.action = (orders_[i].side == Side::BID)
                            ? IntentAction::WOULD_CANCEL_BID
                            : IntentAction::WOULD_CANCEL_ASK;
        cancel.asset_id = orders_[i].asset_id;
        cancel.market_id = orders_[i].market_id;
        cancel.exchange_order_id = orders_[i].exchange_order_id;
        cancel.client_order_id = orders_[i].client_order_id;
        cancel.price = orders_[i].price;
        cancel.qty = orders_[i].original_size - orders_[i].filled_size;
        batch.add(cancel);
    }
    return batch;
}

IntentBatch WorkingOrderTracker::cancel_intents_for_market(const AssetId& condition_id) const {
    IntentBatch batch;
    for (int i = 0; i < kMaxWorking && batch.count < IntentBatch::kMaxIntents; ++i) {
        if (!orders_[i].occupied || orders_[i].is_terminal) continue;
        if (orders_[i].market_id != condition_id) continue;

        ExecutionIntent cancel;
        cancel.action = (orders_[i].side == Side::BID)
                            ? IntentAction::WOULD_CANCEL_BID
                            : IntentAction::WOULD_CANCEL_ASK;
        cancel.asset_id = orders_[i].asset_id;
        cancel.market_id = orders_[i].market_id;
        cancel.exchange_order_id = orders_[i].exchange_order_id;
        cancel.client_order_id = orders_[i].client_order_id;
        cancel.price = orders_[i].price;
        cancel.qty = orders_[i].original_size - orders_[i].filled_size;
        batch.add(cancel);
    }
    return batch;
}

const WorkingOrder* WorkingOrderTracker::find_by_client_id(const OrderId& client_order_id) const {
    int slot = find_slot_by_client_id(client_order_id);
    return slot >= 0 ? &orders_[slot] : nullptr;
}

const WorkingOrder* WorkingOrderTracker::find_by_exchange_id(const OrderId& exchange_order_id) const {
    int slot = find_slot_by_exchange_id(exchange_order_id);
    return slot >= 0 ? &orders_[slot] : nullptr;
}

const WorkingOrder* WorkingOrderTracker::find_by_market_side(const AssetId& market_id, Side side) const {
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].market_id == market_id &&
            orders_[i].side == side) {
            return &orders_[i];
        }
    }
    return nullptr;
}

const WorkingOrder* WorkingOrderTracker::find_by_market_side_level(
    const AssetId& market_id, Side side, int level) const {
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].market_id == market_id &&
            orders_[i].side == side &&
            orders_[i].level == static_cast<uint8_t>(level)) {
            return &orders_[i];
        }
    }
    return nullptr;
}

Price_t WorkingOrderTracker::working_price_at_level(
    const AssetId& market_id, Side side, int level) const {
    const WorkingOrder* wo = find_by_market_side_level(market_id, side, level);
    return wo ? wo->price : kInvalidPrice;
}

Qty_t WorkingOrderTracker::pending_exposure_for_token(const AssetId& token_id) const {
    Qty_t exposure = 0;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (!orders_[i].occupied || orders_[i].is_terminal) continue;
        if (orders_[i].asset_id != token_id) continue;
        Qty_t remaining = orders_[i].original_size - orders_[i].filled_size;
        if (orders_[i].side == Side::BID) {
            exposure += remaining;  // pending buy
        } else {
            exposure -= remaining;  // pending sell
        }
    }
    return exposure;
}

int WorkingOrderTracker::find_slot_by_client_id(const OrderId& id) const {
    if (id.len == 0) return -1;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].client_order_id == id) {
            return i;
        }
    }
    return -1;
}

int WorkingOrderTracker::find_slot_by_exchange_id(const OrderId& id) const {
    if (id.len == 0) return -1;
    for (int i = 0; i < kMaxWorking; ++i) {
        if (orders_[i].occupied && !orders_[i].is_terminal &&
            orders_[i].exchange_order_id == id) {
            return i;
        }
    }
    return -1;
}

int WorkingOrderTracker::find_slot_for_user_update_fallback(const SchedulerEvent& event) const {
    if (event.asset_id.len == 0) return -1;

    int best_slot = -1;
    Timestamp_ns oldest_sent = std::numeric_limits<Timestamp_ns>::max();

    for (int i = 0; i < kMaxWorking; ++i) {
        const auto& wo = orders_[i];
        if (!wo.occupied || wo.is_terminal) continue;
        if (wo.exchange_order_id.len > 0) continue;  // already correlated
        if (wo.asset_id != event.asset_id) continue;
        if (wo.side != event.user_side) continue;
        if (event.user_price > 0 && wo.price != event.user_price) continue;

        if (wo.sent_ts <= oldest_sent) {
            oldest_sent = wo.sent_ts;
            best_slot = i;
        }
    }

    return best_slot;
}

int WorkingOrderTracker::find_free_slot() const {
    for (int i = 0; i < kMaxWorking; ++i) {
        if (!orders_[i].occupied) return i;
    }
    return -1;
}

void WorkingOrderTracker::remove_slot(int idx) {
    orders_[idx] = WorkingOrder{};  // zero out
}

void WorkingOrderTracker::clear_all() {
    for (int i = 0; i < kMaxWorking; ++i) {
        orders_[i] = WorkingOrder{};
    }
}

}  // namespace lt
