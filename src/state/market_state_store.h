#pragma once

#include <unordered_map>
#include <unordered_set>

#include "book/order_book.h"
#include "common/types.h"
#include "events/book_delta.h"
#include "events/event_variant.h"
#include "logger/metrics.h"
#include "queue/spsc_queue.h"
#include "ui_bridge/ui_types.h"

namespace lt {

struct alignas(64) AssetState {
    OrderBook book;
    TickSize_t tick_size = 100;
    BBO cached_bbo;
    Price_t last_trade_price = kInvalidPrice;
    Qty_t last_trade_size = 0;
    int64_t snapshot_count = 0;
    int64_t update_count = 0;
};

class MarketStateStore {
public:
    MarketStateStore(SpscQueue<MarketNotification>& strategy_queue, Metrics& metrics);

    // HOT PATH: apply a parsed market event
    void apply(const MarketEvent& event);

    // Access per-asset state (returns nullptr if not found)
    const AssetState* get_state(const AssetId& asset_id) const;

    // Number of tracked assets
    std::size_t asset_count() const { return states_.size(); }

    // Warmup helpers to pre-seed map entries before hot-path processing.
    void reserve_assets(std::size_t count) { states_.reserve(count); }
    void seed_asset(const AssetId& asset_id) { states_.try_emplace(asset_id); }
    void set_strict_assets(bool strict) { strict_assets_ = strict; }

    // Register a DOWN token so its book events are filtered (no book/BBO/tick updates).
    // LastTradePriceEvent and MarketResolvedEvent still pass through.
    void register_down_token(const AssetId& id) { down_tokens_.insert(id); }
    bool is_down_token(const AssetId& id) const { return down_tokens_.count(id) > 0; }

    // Set the UI book queue for T6. Rate-limited push.
    void set_ui_book_queue(SpscQueue<UiBookUpdate>* q, int rate_hz);

    // Set the book delta queue for T2 shadow books.
    void set_book_delta_queue(SpscQueue<BookDelta>* q) { book_delta_queue_ = q; }

private:
    void maybe_push_ui_books();
    AssetState* get_state_mut(const AssetId& asset_id);
    void emit_notification(NotificationKind kind, const AssetId& asset_id,
                           Timestamp_ns recv_ts, SeqNum_t seq, const BBO& bbo,
                           TickSize_t tick_size = 0,
                           Timestamp_ns exchange_ts = 0);

    // T0/thread owner: websocket+parser thread only.
    std::unordered_map<AssetId, AssetState, AssetIdHash> states_;
    std::unordered_set<AssetId, AssetIdHash> down_tokens_;
    SpscQueue<MarketNotification>& strategy_queue_;
    Metrics& metrics_;

    // Book delta queue (optional, T0->T2 shadow books)
    SpscQueue<BookDelta>* book_delta_queue_ = nullptr;

    // UI bridge (optional, set via set_ui_book_queue)
    SpscQueue<UiBookUpdate>* ui_book_queue_ = nullptr;
    Timestamp_ns last_ui_push_ts_ = 0;
    int64_t ui_push_interval_ns_ = 50'000'000;  // 50ms = 20Hz default
    bool strict_assets_ = false;
};

}  // namespace lt
