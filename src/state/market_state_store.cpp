#include "state/market_state_store.h"

#include <algorithm>

#include "common/clock.h"

namespace lt {

namespace {

// Push a BookDelta to the queue. For snapshots, use push_spin() (rare,
// must not be dropped). For incrementals, use try_push() with drop metric.
void push_delta(SpscQueue<BookDelta>* q, const BookDelta& delta,
                Metrics& metrics, bool is_snapshot) {
    if (!q) return;
    if (is_snapshot) {
        // Snapshots are rare (connect-time only) and should not be dropped.
        // Use bounded spin to avoid hanging T0 if T2 is stalled.
        static constexpr int kSnapshotSpinLimit = 100'000;  // ~100ms
        bool pushed = false;
        for (int attempt = 0; attempt < kSnapshotSpinLimit; ++attempt) {
            if (q->try_push(delta)) { pushed = true; break; }
        }
        if (pushed) {
            metrics.inc(MetricId::BOOK_DELTA_PUSHES);
            metrics.inc(MetricId::BOOK_DELTA_SNAPSHOT_CHUNKS);
        } else {
            metrics.inc(MetricId::BOOK_DELTA_DROPS);
        }
    } else {
        if (q->try_push(delta)) {
            metrics.inc(MetricId::BOOK_DELTA_PUSHES);
        } else {
            metrics.inc(MetricId::BOOK_DELTA_DROPS);
        }
    }
}

}  // namespace

MarketStateStore::MarketStateStore(SpscQueue<MarketNotification>& strategy_queue, Metrics& metrics)
    : strategy_queue_(strategy_queue), metrics_(metrics) {
    // Pre-allocate map buckets. First event per asset triggers heap insertion;
    // steady-state apply() is allocation-free after warmup.
    states_.reserve(16);
}

void MarketStateStore::apply(const MarketEvent& event) {
    std::visit(
        [&](const auto& payload) {
            using T = std::decay_t<decltype(payload)>;

            if constexpr (std::is_same_v<T, BookSnapshot>) {
                if (is_down_token(payload.asset_id)) {
                    metrics_.inc(MetricId::BOOK_DOWN_FILTERED);
                    return;
                }
                auto* state = get_state_mut(payload.asset_id);
                if (!state) {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                    return;
                }
                auto t0 = SteadyClock::now();
                auto err = state->book.apply_snapshot(payload);
                auto dt = SteadyClock::now() - t0;
                metrics_.record_latency(MetricId::BOOK_LATENCY_NS, MetricId::BOOK_LATENCY_COUNT,
                                        dt);

                if (err == ErrorCode::OK) {
                    state->cached_bbo = {state->book.bbo().best_bid, state->book.bbo().best_ask,
                                         state->book.bbo().bid_size, state->book.bbo().ask_size};
                    state->snapshot_count++;
                    metrics_.inc(MetricId::BOOK_SNAPSHOTS);

                    // Push snapshot as chunked BookDeltas to T2 shadow books
                    if (book_delta_queue_) {
                        // SNAPSHOT_BEGIN
                        BookDelta begin_delta;
                        begin_delta.asset_id = payload.asset_id;
                        begin_delta.kind = BookDeltaKind::SNAPSHOT_BEGIN;
                        begin_delta.change_count = 0;
                        // Pack first chunk of levels into BEGIN message
                        auto pack_levels = [&](const auto& levels, uint16_t count, Side side,
                                                BookDelta& delta) {
                            for (uint16_t li = 0; li < count; ++li) {
                                if (delta.change_count >= kMaxDeltaChanges) {
                                    // Flush current delta
                                    push_delta(book_delta_queue_, delta, metrics_, true);
                                    delta = BookDelta{};
                                    delta.asset_id = payload.asset_id;
                                    delta.kind = BookDeltaKind::SNAPSHOT_CHUNK;
                                    delta.change_count = 0;
                                }
                                delta.changes[delta.change_count++] = {
                                    levels[li].price, side, levels[li].size};
                            }
                        };
                        pack_levels(payload.bids, payload.bid_count, Side::BID, begin_delta);
                        pack_levels(payload.asks, payload.ask_count, Side::ASK, begin_delta);

                        // Change last delta kind to SNAPSHOT_END
                        if (begin_delta.kind == BookDeltaKind::SNAPSHOT_BEGIN) {
                            // Everything fit in one message — mark as both begin and end
                            // by sending BEGIN then an empty END
                            push_delta(book_delta_queue_, begin_delta, metrics_, true);
                            BookDelta end_delta;
                            end_delta.asset_id = payload.asset_id;
                            end_delta.kind = BookDeltaKind::SNAPSHOT_END;
                            end_delta.change_count = 0;
                            push_delta(book_delta_queue_, end_delta, metrics_, true);
                        } else {
                            // Flush remaining chunk as SNAPSHOT_END
                            begin_delta.kind = BookDeltaKind::SNAPSHOT_END;
                            push_delta(book_delta_queue_, begin_delta, metrics_, true);
                        }
                    }

                    emit_notification(NotificationKind::BOOK_SNAPSHOT, payload.asset_id,
                                      event.recv_ts, event.seq,
                                      state->book.bbo(), 0, payload.exchange_ts);
                } else {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                }
            } else if constexpr (std::is_same_v<T, PriceChangeEvent>) {
                for (uint16_t i = 0; i < payload.asset_count; ++i) {
                    const auto& ac = payload.asset_changes[i];
                    if (is_down_token(ac.asset_id)) {
                        metrics_.inc(MetricId::BOOK_DOWN_FILTERED);
                        continue;
                    }
                    auto* state = get_state_mut(ac.asset_id);
                    if (!state) {
                        metrics_.inc(MetricId::BOOK_ERRORS);
                        continue;
                    }
                    auto t0 = SteadyClock::now();
                    auto err = state->book.apply_price_change(ac);
                    auto dt = SteadyClock::now() - t0;
                    metrics_.record_latency(MetricId::BOOK_LATENCY_NS,
                                            MetricId::BOOK_LATENCY_COUNT, dt);

                    if (err == ErrorCode::OK) {
                        state->cached_bbo = {state->book.bbo().best_bid, state->book.bbo().best_ask,
                                             state->book.bbo().bid_size, state->book.bbo().ask_size};
                        state->update_count++;
                        metrics_.inc(MetricId::BOOK_UPDATES);

                        // Push incremental delta to T2 shadow books
                        if (book_delta_queue_) {
                            // May need multiple deltas if >64 changes
                            BookDelta delta;
                            delta.asset_id = ac.asset_id;
                            delta.kind = BookDeltaKind::INCREMENTAL;
                            delta.change_count = 0;
                            for (uint16_t ci = 0; ci < ac.change_count; ++ci) {
                                if (delta.change_count >= kMaxDeltaChanges) {
                                    push_delta(book_delta_queue_, delta, metrics_, false);
                                    delta.change_count = 0;
                                }
                                delta.changes[delta.change_count++] = {
                                    ac.changes[ci].price, ac.changes[ci].side,
                                    ac.changes[ci].size};
                            }
                            if (delta.change_count > 0) {
                                push_delta(book_delta_queue_, delta, metrics_, false);
                            }
                        }

                        emit_notification(NotificationKind::PRICE_CHANGE, ac.asset_id,
                                          event.recv_ts, event.seq,
                                          state->book.bbo(), 0, payload.exchange_ts);
                    } else {
                        metrics_.inc(MetricId::BOOK_ERRORS);
                    }
                }
            } else if constexpr (std::is_same_v<T, BestBidAskEvent>) {
                if (is_down_token(payload.asset_id)) return;
                auto* state = get_state_mut(payload.asset_id);
                if (!state) {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                    return;
                }
                // Compare server BBO with our book BBO — log divergence
                const auto& book_bbo = state->book.bbo();
                if (book_bbo.best_bid != payload.best_bid ||
                    book_bbo.best_ask != payload.best_ask) {
                    metrics_.inc(MetricId::BBO_DIVERGENCE);
                }
                // Do NOT update cached_bbo or emit notification — wait for price_change
            } else if constexpr (std::is_same_v<T, TickSizeChangeEvent>) {
                if (is_down_token(payload.asset_id)) return;
                if (payload.new_tick_size <= 0) {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                    return;
                }
                auto* state = get_state_mut(payload.asset_id);
                if (!state) {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                    return;
                }
                state->tick_size = payload.new_tick_size;
                state->book.set_tick_size(payload.new_tick_size);

                // Push tick size change delta to T2 shadow books
                if (book_delta_queue_) {
                    BookDelta delta;
                    delta.asset_id = payload.asset_id;
                    delta.kind = BookDeltaKind::INCREMENTAL;
                    delta.change_count = 0;
                    delta.tick_size = payload.new_tick_size;
                    push_delta(book_delta_queue_, delta, metrics_, false);
                }

                BBO bbo = {state->book.bbo().best_bid, state->book.bbo().best_ask,
                           state->book.bbo().bid_size, state->book.bbo().ask_size};
                emit_notification(NotificationKind::TICK_SIZE_CHANGE, payload.asset_id,
                                  event.recv_ts, event.seq, bbo, payload.new_tick_size);
            } else if constexpr (std::is_same_v<T, LastTradePriceEvent>) {
                auto* state = get_state_mut(payload.asset_id);
                if (!state) {
                    metrics_.inc(MetricId::BOOK_ERRORS);
                    return;
                }
                state->last_trade_price = payload.price;
                state->last_trade_size = payload.size;
                BBO bbo = {state->book.bbo().best_bid, state->book.bbo().best_ask,
                           state->book.bbo().bid_size, state->book.bbo().ask_size};
                emit_notification(NotificationKind::LAST_TRADE, payload.asset_id,
                                  event.recv_ts, event.seq, bbo, 0);
            } else if constexpr (std::is_same_v<T, NewMarketEvent>) {
                metrics_.inc(MetricId::NEW_MARKETS_RECEIVED);
                // Log only — no state change needed
            } else if constexpr (std::is_same_v<T, MarketResolvedEvent>) {
                metrics_.inc(MetricId::MARKETS_RESOLVED);
                // Mark both tokens' books as stale
                for (uint8_t i = 0; i < payload.asset_count; ++i) {
                    auto* state = get_state_mut(payload.assets[i]);
                    if (state) state->book.set_status(BookStatus::STALE);
                }
                // Emit MARKET_RESOLVED notification with condition_id and winning asset
                MarketNotification notif;
                notif.kind = NotificationKind::MARKET_RESOLVED;
                notif.asset_id = payload.market_id;
                notif.resolved_winning_asset_id = payload.winning_asset_id;
                notif.recv_ts = event.recv_ts;
                notif.seq = event.seq;
                if (strategy_queue_.try_push(notif)) {
                    metrics_.inc(MetricId::QUEUE_PUSHES);
                } else {
                    metrics_.inc(MetricId::QUEUE_OVERFLOWS);
                }
            }
        },
        event.payload);

    maybe_push_ui_books();
}

const AssetState* MarketStateStore::get_state(const AssetId& asset_id) const {
    auto it = states_.find(asset_id);
    if (it == states_.end()) return nullptr;
    return &it->second;
}

AssetState* MarketStateStore::get_state_mut(const AssetId& asset_id) {
    auto it = states_.find(asset_id);
    if (it != states_.end()) return &it->second;
    if (strict_assets_) return nullptr;
    auto [inserted_it, _] = states_.try_emplace(asset_id);
    return &inserted_it->second;
}

void MarketStateStore::emit_notification(NotificationKind kind, const AssetId& asset_id,
                                         Timestamp_ns recv_ts, SeqNum_t seq, const BBO& bbo,
                                         TickSize_t tick_size,
                                         Timestamp_ns exchange_ts) {
    MarketNotification notif;
    notif.kind = kind;
    notif.asset_id = asset_id;
    notif.recv_ts = recv_ts;
    notif.seq = seq;
    notif.bbo = {bbo.best_bid, bbo.best_ask, bbo.bid_size, bbo.ask_size};
    notif.tick_size = tick_size;
    notif.exchange_ts = exchange_ts;

    if (strategy_queue_.try_push(notif)) {
        metrics_.inc(MetricId::QUEUE_PUSHES);
    } else {
        metrics_.inc(MetricId::QUEUE_OVERFLOWS);
    }
}

void MarketStateStore::set_ui_book_queue(SpscQueue<UiBookUpdate>* q, int rate_hz) {
    ui_book_queue_ = q;
    if (rate_hz > 0) {
        ui_push_interval_ns_ = 1'000'000'000LL / rate_hz;
    }
}

void MarketStateStore::maybe_push_ui_books() {
    if (!ui_book_queue_) return;

    auto now = SteadyClock::now();
    if (now - last_ui_push_ts_ < ui_push_interval_ns_) return;
    last_ui_push_ts_ = now;

    for (auto& [asset_id, state] : states_) {
        if (is_down_token(asset_id)) continue;
        UiBookUpdate upd;
        upd.asset_id = asset_id;
        upd.bbo = state.book.bbo();
        upd.timestamp = now;

        // Extract top-N bids (descending price) with early break
        upd.bid_count = 0;
        state.book.for_each_bid_n(kMaxUiBookDepth, [&](Price_t price, Qty_t qty) {
            upd.bids[upd.bid_count] = {price, qty};
            ++upd.bid_count;
        });

        // Extract top-N asks (ascending price) with early break
        upd.ask_count = 0;
        state.book.for_each_ask_n(kMaxUiBookDepth, [&](Price_t price, Qty_t qty) {
            upd.asks[upd.ask_count] = {price, qty};
            ++upd.ask_count;
        });

        if (ui_book_queue_->try_push(upd)) {
            metrics_.inc(MetricId::UI_BOOK_PUSHES);
        } else {
            metrics_.inc(MetricId::UI_BOOK_DROPS);
        }
    }
}

}  // namespace lt
