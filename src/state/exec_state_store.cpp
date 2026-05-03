#include "state/exec_state_store.h"

#include <cstdio>
#include <limits>

#include "common/clock.h"

namespace lt {

namespace {

// Terminal order states cannot be regressed
bool is_terminal_order_status(OrderStatus s) {
    return s == OrderStatus::FILLED || s == OrderStatus::CANCELED || s == OrderStatus::FAILED;
}

// Trade status progression rank (higher = later in lifecycle)
int trade_status_rank(TradeStatus s) {
    switch (s) {
        case TradeStatus::UNKNOWN: return 0;
        case TradeStatus::MATCHED: return 1;
        case TradeStatus::RETRYING: return 2;
        case TradeStatus::MINED: return 3;
        case TradeStatus::CONFIRMED: return 4;
        case TradeStatus::FAILED: return 4;  // same rank as CONFIRMED (both terminal)
    }
    return 0;
}

bool is_terminal_trade_status(TradeStatus s) {
    return s == TradeStatus::CONFIRMED || s == TradeStatus::FAILED;
}

}  // namespace

ExecStateStore::ExecStateStore(SpscQueue<SchedulerEvent>& user_queue, Metrics& metrics,
                               AsyncLogger* logger,
                               std::atomic<bool>* fatal_flag, TokenInventory* inventory,
                               PnlTracker* pnl_tracker,
                               const ExecStateStoreConfig& config)
    : user_queue_(user_queue),
      metrics_(metrics),
      log_handle_(logger ? logger->create_producer("T1-ExecState") : ProducerHandle{}),
      fatal_flag_(fatal_flag),
      inventory_(inventory),
      pnl_tracker_(pnl_tracker),
      config_(config) {}

ApplyResult ExecStateStore::apply_order_update(const UserOrderUpdate& upd,
                                               Timestamp_ns recv_ts, SeqNum_t seq) {
    metrics_.inc(MetricId::USER_WS_ORDER_UPDATES);
    maybe_sweep(recv_ts);

    auto it = orders_.find(upd.order_id);
    if (it != orders_.end()) {
        auto& tracked = it->second;

        // Guard: reject if asset_id or side differs from tracked order
        // (prevents fill mis-attribution from malformed or replayed messages)
        if (!(tracked.asset_id == upd.asset_id) || tracked.side != upd.side) {
            metrics_.inc(MetricId::USER_WS_DUPLICATES);
            return ApplyResult::DUPLICATE;
        }

        // Guard: terminal states cannot be regressed by late PLACEMENT/UPDATE
        if (is_terminal_order_status(tracked.status) &&
            upd.event_type != OrderEventType::CANCELLATION) {
            // CANCELLATION can still arrive after FILLED (exchange race), accept it.
            // But PLACEMENT/UPDATE cannot regress FILLED/CANCELED.
            if (upd.event_type == OrderEventType::PLACEMENT) {
                metrics_.inc(MetricId::USER_WS_DUPLICATES);
                return ApplyResult::DUPLICATE;
            }
            // UPDATE on terminal: only accept if size_matched is strictly greater
            // (exchange might send a final fill confirmation)
            if (upd.size_matched <= tracked.size_matched) {
                metrics_.inc(MetricId::USER_WS_DUPLICATES);
                return ApplyResult::DUPLICATE;
            }
        }

        // Idempotency: skip if size_matched unchanged for UPDATE events
        if (upd.event_type == OrderEventType::UPDATE &&
            upd.size_matched == tracked.size_matched) {
            metrics_.inc(MetricId::USER_WS_DUPLICATES);
            return ApplyResult::DUPLICATE;
        }

        // Reject stale size_matched regression: out-of-order UPDATE with lower
        // size_matched than tracked is stale — skip to prevent double-counting
        if (upd.size_matched < tracked.size_matched) {
            metrics_.inc(MetricId::USER_WS_DUPLICATES);
            return ApplyResult::DUPLICATE;
        }

        // Update tracked state (only forward progression)
        // Note: fills are NOT recorded here — trade MATCHED is the sole
        // authoritative fill source (prevents cross-channel double-counting).
        tracked.size_matched = upd.size_matched;
        tracked.last_update_ts = recv_ts;
        tracked.last_seq = seq;

        // Determine new status (only forward transitions)
        switch (upd.event_type) {
            case OrderEventType::PLACEMENT:
                // Late PLACEMENT on existing order: only upgrade from UNKNOWN
                if (tracked.status == OrderStatus::UNKNOWN) {
                    tracked.status = OrderStatus::LIVE;
                }
                break;
            case OrderEventType::UPDATE:
                if (upd.size_matched >= upd.original_size && upd.original_size > 0) {
                    tracked.status = OrderStatus::FILLED;
                } else if (upd.size_matched > 0) {
                    // Only advance to PARTIAL if not already in a later state
                    if (tracked.status == OrderStatus::LIVE ||
                        tracked.status == OrderStatus::UNKNOWN) {
                        tracked.status = OrderStatus::PARTIAL;
                    }
                }
                break;
            case OrderEventType::CANCELLATION:
                tracked.status = OrderStatus::CANCELED;
                break;
        }

    } else {
        // New order
        TrackedOrder tracked;
        tracked.order_id = upd.order_id;
        tracked.client_order_id = upd.client_order_id;
        tracked.asset_id = upd.asset_id;
        tracked.side = upd.side;
        tracked.price = upd.price;
        tracked.original_size = upd.original_size;
        tracked.size_matched = upd.size_matched;
        tracked.first_seen_ts = recv_ts;
        tracked.last_update_ts = recv_ts;
        tracked.last_seq = seq;

        switch (upd.event_type) {
            case OrderEventType::PLACEMENT:
                tracked.status = OrderStatus::LIVE;
                break;
            case OrderEventType::UPDATE:
                if (upd.size_matched >= upd.original_size && upd.original_size > 0) {
                    tracked.status = OrderStatus::FILLED;
                } else if (upd.size_matched > 0) {
                    tracked.status = OrderStatus::PARTIAL;
                } else {
                    tracked.status = OrderStatus::LIVE;
                }
                break;
            case OrderEventType::CANCELLATION:
                tracked.status = OrderStatus::CANCELED;
                break;
        }

        // Note: initial size_matched is tracked for status but fills are NOT
        // recorded here — trade MATCHED is the sole fill source.

        orders_.emplace(upd.order_id, tracked);

        if (log_handle_.queue) {
            const char* evt_str = upd.event_type == OrderEventType::PLACEMENT ? "PLACE"
                                : upd.event_type == OrderEventType::UPDATE ? "UPDATE" : "CANCEL";
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "ORDER_NEW %s %s p=%d sz=%lld matched=%lld oid=%.20s",
                evt_str, upd.side == Side::BID ? "BUY" : "SELL",
                upd.price, static_cast<long long>(upd.original_size),
                static_cast<long long>(upd.size_matched),
                std::string(upd.order_id.data, upd.order_id.len).c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }
    }

    // Emit SchedulerEvent to user_queue (spin-yield to avoid silent drop)
    auto ev = SchedulerEvent::from_order_update(upd, recv_ts, seq);
    auto status_it = orders_.find(upd.order_id);
    if (status_it != orders_.end()) {
        ev.order_status_raw = static_cast<uint8_t>(status_it->second.status);
    }
    if (!user_queue_.push_spin(ev)) {
        metrics_.inc(MetricId::USER_WS_QUEUE_OVERFLOW);
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
        return ApplyResult::QUEUE_OVERFLOW;
    }

    return ApplyResult::APPLIED;
}

ApplyResult ExecStateStore::apply_trade_update(const UserTradeUpdate& upd,
                                               Timestamp_ns recv_ts, SeqNum_t seq) {
    metrics_.inc(MetricId::USER_WS_TRADE_UPDATES);
    maybe_sweep(recv_ts);

    // --- Fill attribution: find our order, determine fill amount and perspective ---
    // Three-phase lookup: (1) maker_orders, (2) taker_order_id, (3) unattributed.
    // When we find our order in the orders_ map, always use the order's own
    // asset_id and side — that's what we actually sent to the exchange.
    // The exchange WS may report the fill from the matching-engine's book
    // perspective (e.g., our SELL-UP fills on the NO book, reported as BUY-DOWN).
    Qty_t effective_fill = 0;
    const char* fill_src = "none";
    bool trade_involves_us = false;
    AssetId effective_asset = upd.asset_id;
    Side our_side = upd.side;
    Price_t effective_price = upd.fill_price;

    // Phase 1: Check maker_orders for our order
    for (uint8_t i = 0; i < upd.maker_entry_count; ++i) {
        const auto& me = upd.maker_entries[i];
        auto order_it = orders_.find(me.order_id);
        if (order_it != orders_.end()) {
            effective_fill = me.matched_amount;
            fill_src = "matched_amount";
            trade_involves_us = true;
            // Always use our order's asset/side — that's what we sent.
            effective_asset = order_it->second.asset_id;
            our_side = order_it->second.side;
            if (!(order_it->second.asset_id == upd.asset_id)) {
                // Fill reported on a different book — convert price to our token's perspective.
                effective_price = kPriceScale - upd.fill_price;
                metrics_.inc(MetricId::USER_WS_CROSS_BOOK_FILL);
            }
            break;
        }
        if (inventory_ && inventory_->is_our_order(me.order_id)) {
            // Found in shared set but not orders_ map — can't look up asset/side.
            // Use trade event data as-is (best effort).
            effective_fill = me.matched_amount;
            fill_src = "matched_amount_shared";
            trade_involves_us = true;
            break;
        }
    }

    // Phase 2: Check if we are the TAKER (FAK fills)
    if (!trade_involves_us && upd.taker_order_id.len > 0) {
        auto taker_it = orders_.find(upd.taker_order_id);
        if (taker_it != orders_.end()) {
            effective_fill = upd.fill_size;  // taker total IS our total
            fill_src = "taker_orders";
            trade_involves_us = true;
            // Always use our order's asset/side — that's what we sent.
            effective_asset = taker_it->second.asset_id;
            our_side = taker_it->second.side;
            if (!(taker_it->second.asset_id == upd.asset_id)) {
                effective_price = kPriceScale - upd.fill_price;
                metrics_.inc(MetricId::USER_WS_CROSS_BOOK_FILL);
            }
            metrics_.inc(MetricId::USER_WS_TAKER_FALLBACK);
        } else if (inventory_ && inventory_->is_our_order(upd.taker_order_id)) {
            effective_fill = upd.fill_size;
            fill_src = "taker_shared";
            trade_involves_us = true;
            metrics_.inc(MetricId::USER_WS_TAKER_FALLBACK);
        }
    }

    // Phase 3: No match — cannot attribute fill, record zero to prevent phantom position
    if (!trade_involves_us) {
        metrics_.inc(MetricId::USER_WS_TRADE_UNATTRIBUTED);
    }

    if (log_handle_.queue) {
        bool cross_book = !(effective_asset == upd.asset_id);
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf),
            "TRADE %s side=%s src=%s fill=%lld raw_size=%lld p=%d raw_p=%d xbook=%d makers=%d tid=%.20s",
            upd.status == TradeStatus::MATCHED ? "MATCHED" :
            upd.status == TradeStatus::MINED ? "MINED" :
            upd.status == TradeStatus::CONFIRMED ? "CONFIRMED" :
            upd.status == TradeStatus::FAILED ? "FAILED" : "OTHER",
            our_side == Side::BID ? "BUY" : "SELL",
            fill_src,
            static_cast<long long>(effective_fill),
            static_cast<long long>(upd.fill_size),
            effective_price, upd.fill_price,
            cross_book ? 1 : 0,
            static_cast<int>(upd.maker_entry_count),
            std::string(upd.trade_id.data, upd.trade_id.len).c_str());
        AsyncLogger::log(log_handle_, cross_book ? LogLevel::WARN : LogLevel::INFO, buf);
        if (!trade_involves_us) {
            std::snprintf(buf, sizeof(buf),
                "TRADE_UNATTRIBUTED: no maker/taker match, skipping fill tid=%.20s",
                std::string(upd.trade_id.data, upd.trade_id.len).c_str());
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        }
    }

    bool is_new_fill = false;

    auto it = trades_.find(upd.trade_id);
    if (it != trades_.end()) {
        auto& tracked = it->second;

        // Idempotency: skip if status is unchanged (repeated status-only update)
        if (tracked.status == upd.status) {
            metrics_.inc(MetricId::USER_WS_DUPLICATES);
            return ApplyResult::DUPLICATE;
        }

        // Guard: don't regress status to an earlier lifecycle stage
        if (trade_status_rank(upd.status) < trade_status_rank(tracked.status)) {
            // Exception: MATCHED arriving after MINED/CONFIRMED is an out-of-order
            // delivery — we still need to count the fill if not yet counted
            if (upd.status == TradeStatus::MATCHED) {
                if (seen_trade_fills_.find(upd.trade_id) == seen_trade_fills_.end()) {
                    seen_trade_fills_.emplace(upd.trade_id, recv_ts);
                    tracked.pnl_delta = record_fill(tracked.asset_id, tracked.side, tracked.fill_size, tracked.fill_price);
                    is_new_fill = true;
                } else {
                    // Fill already counted, no new information
                    metrics_.inc(MetricId::USER_WS_DUPLICATES);
                    return ApplyResult::DUPLICATE;
                }
                // Don't regress status, but do emit the event with is_new_fill
            } else {
                metrics_.inc(MetricId::USER_WS_DUPLICATES);
                return ApplyResult::DUPLICATE;
            }
        } else {
            // Forward status progression
            tracked.status = upd.status;
            tracked.last_update_ts = recv_ts;

            // Count fill on reaching MATCHED status (normal forward path)
            if (upd.status == TradeStatus::MATCHED) {
                if (seen_trade_fills_.find(upd.trade_id) == seen_trade_fills_.end()) {
                    seen_trade_fills_.emplace(upd.trade_id, recv_ts);
                    tracked.pnl_delta = record_fill(tracked.asset_id, tracked.side, tracked.fill_size, tracked.fill_price);
                    is_new_fill = true;
                }
            }

            // Reverse fill if trade FAILED after fill was already counted
            if (upd.status == TradeStatus::FAILED) {
                if (seen_trade_fills_.find(upd.trade_id) != seen_trade_fills_.end()) {
                    reverse_fill(tracked.asset_id, tracked.side, tracked.fill_size,
                                 tracked.fill_price, tracked.pnl_delta);
                }
            }
        }
    } else {
        // New trade — store with OUR resolved asset/side/price, not raw event's
        TrackedTrade tracked;
        tracked.trade_id = upd.trade_id;
        tracked.taker_order_id = upd.taker_order_id;
        tracked.asset_id = effective_asset;
        tracked.status = upd.status;
        tracked.side = our_side;
        tracked.fill_price = effective_price;
        tracked.fill_size = effective_fill;
        tracked.first_seen_ts = recv_ts;
        tracked.last_update_ts = recv_ts;

        auto [trade_it, _inserted] = trades_.emplace(upd.trade_id, tracked);

        // Count fill if this is a MATCHED event and we have a non-zero fill
        if (upd.status == TradeStatus::MATCHED && effective_fill > 0) {
            if (seen_trade_fills_.find(upd.trade_id) == seen_trade_fills_.end()) {
                seen_trade_fills_.emplace(upd.trade_id, recv_ts);
                trade_it->second.pnl_delta = record_fill(effective_asset, our_side, effective_fill, effective_price);
                is_new_fill = true;
            }
        }
        // If first-seen status is not MATCHED (e.g., MINED arrives first),
        // fill will be counted when MATCHED eventually arrives via the
        // existing-trade path above.
    }

    // Emit SchedulerEvent with corrected asset/side/price/fill so T2 sees
    // consistent UP-token perspective.
    UserTradeUpdate corrected_upd = upd;
    corrected_upd.asset_id = effective_asset;
    corrected_upd.side = our_side;
    corrected_upd.fill_price = effective_price;
    corrected_upd.fill_size = effective_fill;
    auto ev = SchedulerEvent::from_trade_update(corrected_upd, recv_ts, seq, is_new_fill);
    if (!user_queue_.push_spin(ev)) {
        metrics_.inc(MetricId::USER_WS_QUEUE_OVERFLOW);
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
        return ApplyResult::QUEUE_OVERFLOW;
    }

    return ApplyResult::APPLIED;
}

const TrackedOrder* ExecStateStore::get_order(const OrderId& id) const {
    auto it = orders_.find(id);
    return it != orders_.end() ? &it->second : nullptr;
}

const TrackedTrade* ExecStateStore::get_trade(const TradeId& id) const {
    auto it = trades_.find(id);
    return it != trades_.end() ? &it->second : nullptr;
}

const PositionDelta* ExecStateStore::get_position(const AssetId& id) const {
    auto it = positions_.find(id);
    return it != positions_.end() ? &it->second : nullptr;
}

void ExecStateStore::maybe_sweep(Timestamp_ns now) {
    bool over_cap =
        (config_.max_orders > 0 && orders_.size() > config_.max_orders) ||
        (config_.max_trades > 0 && trades_.size() > config_.max_trades) ||
        (config_.max_seen_trade_fills > 0 &&
         seen_trade_fills_.size() > config_.max_seen_trade_fills);
    if (config_.sweep_interval_ms <= 0 && !over_cap) return;
    if (!over_cap && next_sweep_ts_ != 0 && now < next_sweep_ts_) return;

    evict_terminal_orders(now);
    evict_terminal_trades(now);
    evict_seen_trade_fills(now);
    if (config_.sweep_interval_ms > 0) {
        next_sweep_ts_ = now + config_.sweep_interval_ms * 1'000'000LL;
    }
}

void ExecStateStore::evict_terminal_orders(Timestamp_ns now) {
    const int64_t ttl_ns = config_.terminal_order_ttl_ms > 0
                               ? config_.terminal_order_ttl_ms * 1'000'000LL
                               : 0;
    if (ttl_ns > 0) {
        for (auto it = orders_.begin(); it != orders_.end();) {
            if (is_terminal_order_status(it->second.status) &&
                now - it->second.last_update_ts >= ttl_ns) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto erase_oldest = [&](bool terminal_only) -> bool {
        auto victim = orders_.end();
        Timestamp_ns oldest = std::numeric_limits<Timestamp_ns>::max();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (terminal_only && !is_terminal_order_status(it->second.status)) continue;
            if (it->second.last_update_ts < oldest) {
                oldest = it->second.last_update_ts;
                victim = it;
            }
        }
        if (victim == orders_.end()) return false;
        orders_.erase(victim);
        return true;
    };

    if (config_.max_orders > 0) {
        while (orders_.size() > config_.max_orders) {
            if (!erase_oldest(true) && !erase_oldest(false)) break;
        }
    }
}

void ExecStateStore::evict_terminal_trades(Timestamp_ns now) {
    const int64_t ttl_ns = config_.terminal_trade_ttl_ms > 0
                               ? config_.terminal_trade_ttl_ms * 1'000'000LL
                               : 0;
    if (ttl_ns > 0) {
        for (auto it = trades_.begin(); it != trades_.end();) {
            if (is_terminal_trade_status(it->second.status) &&
                now - it->second.last_update_ts >= ttl_ns) {
                it = trades_.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto erase_oldest = [&](bool terminal_only) -> bool {
        auto victim = trades_.end();
        Timestamp_ns oldest = std::numeric_limits<Timestamp_ns>::max();
        for (auto it = trades_.begin(); it != trades_.end(); ++it) {
            if (terminal_only && !is_terminal_trade_status(it->second.status)) continue;
            if (it->second.last_update_ts < oldest) {
                oldest = it->second.last_update_ts;
                victim = it;
            }
        }
        if (victim == trades_.end()) return false;
        trades_.erase(victim);
        return true;
    };

    if (config_.max_trades > 0) {
        while (trades_.size() > config_.max_trades) {
            if (!erase_oldest(true) && !erase_oldest(false)) break;
        }
    }
}

void ExecStateStore::evict_seen_trade_fills(Timestamp_ns now) {
    const int64_t ttl_ns = config_.seen_trade_fill_ttl_ms > 0
                               ? config_.seen_trade_fill_ttl_ms * 1'000'000LL
                               : 0;
    if (ttl_ns > 0) {
        for (auto it = seen_trade_fills_.begin(); it != seen_trade_fills_.end();) {
            if (now - it->second >= ttl_ns) {
                it = seen_trade_fills_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (config_.max_seen_trade_fills == 0) return;
    while (seen_trade_fills_.size() > config_.max_seen_trade_fills) {
        auto victim = seen_trade_fills_.end();
        Timestamp_ns oldest = std::numeric_limits<Timestamp_ns>::max();
        for (auto it = seen_trade_fills_.begin(); it != seen_trade_fills_.end(); ++it) {
            if (it->second < oldest) {
                oldest = it->second;
                victim = it;
            }
        }
        if (victim == seen_trade_fills_.end()) break;
        seen_trade_fills_.erase(victim);
    }
}

int64_t ExecStateStore::record_fill(const AssetId& asset_id, Side side, Qty_t size, Price_t fill_price) {
    metrics_.inc(MetricId::USER_WS_FILLS);

    auto& pos = positions_[asset_id];

    if (log_handle_.queue) {
        Qty_t new_net = pos.net_position + (side == Side::BID ? size : -size);
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf),
            "FILL %s p=%d sz=%lld net=%lld->%lld asset=%.20s",
            side == Side::BID ? "BUY" : "SELL",
            fill_price, static_cast<long long>(size),
            static_cast<long long>(pos.net_position),
            static_cast<long long>(new_net),
            std::string(asset_id.data, asset_id.len).c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }
    if (side == Side::BID) {
        pos.net_position += size;
        pos.total_bought += size;
        if (inventory_) inventory_->adjust_position(asset_id, size);
    } else {
        pos.net_position -= size;
        pos.total_sold += size;
        if (inventory_) inventory_->adjust_position(asset_id, -size);
    }
    pos.fill_count++;

    // Adjust USDC balance: BUY spends USDC, SELL receives USDC.
    // usdc_delta = fill_price * size / kPriceScale (fp4 * fp6 / fp4 = fp6 = micro-USDC)
    if (inventory_ && fill_price > 0) {
        int64_t usdc_delta = static_cast<int64_t>(fill_price) * size / kPriceScale;
        inventory_->adjust_usdc_balance(side == Side::BID ? -usdc_delta : usdc_delta);
    }

    // FIFO PnL tracking — returns the PnL delta for reversal tracking
    int64_t pnl_delta = 0;
    if (pnl_tracker_ && fill_price > 0) {
        pnl_delta = pnl_tracker_->record_fill(asset_id, side, fill_price, size);
    }

    metrics_.inc(MetricId::USER_WS_POSITION_DELTAS);
    return pnl_delta;
}

void ExecStateStore::reverse_fill(const AssetId& asset_id, Side side, Qty_t size,
                                  Price_t fill_price, int64_t pnl_delta) {
    metrics_.inc(MetricId::USER_WS_FILLS);  // track reversal events too

    auto& pos = positions_[asset_id];

    if (log_handle_.queue) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf),
            "REVERSE_FILL %s p=%d sz=%lld net=%lld pnl_undo=%lld asset=%.20s",
            side == Side::BID ? "BUY" : "SELL",
            fill_price, static_cast<long long>(size),
            static_cast<long long>(pos.net_position),
            static_cast<long long>(pnl_delta),
            std::string(asset_id.data, asset_id.len).c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }
    if (side == Side::BID) {
        pos.net_position -= size;
        pos.total_bought = (pos.total_bought > size) ? pos.total_bought - size : 0;
        if (inventory_) inventory_->adjust_position(asset_id, -size);
    } else {
        pos.net_position += size;
        pos.total_sold = (pos.total_sold > size) ? pos.total_sold - size : 0;
        if (inventory_) inventory_->adjust_position(asset_id, size);
    }
    if (pos.fill_count > 0) pos.fill_count--;

    // Reverse USDC adjustment
    if (inventory_ && fill_price > 0) {
        int64_t usdc_delta = static_cast<int64_t>(fill_price) * size / kPriceScale;
        inventory_->adjust_usdc_balance(side == Side::BID ? usdc_delta : -usdc_delta);
    }

    // Reverse exact PnL delta — no FIFO interaction, no phantom positions
    if (pnl_tracker_) {
        pnl_tracker_->reverse_fill(pnl_delta);
    }

    metrics_.inc(MetricId::USER_WS_POSITION_DELTAS);
}

}  // namespace lt
