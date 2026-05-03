#include "scheduler/strategy_book_store.h"

#include "events/market_events.h"

namespace lt {

void StrategyBookStore::apply_delta(const BookDelta& delta) {
    auto it = books_.find(delta.asset_id);
    if (it == books_.end()) {
        // Unknown asset — ignore (not seeded)
        return;
    }
    OrderBook& book = it->second;

    switch (delta.kind) {
        case BookDeltaKind::SNAPSHOT_BEGIN:
            // Clear book and mark as pending
            book.clear();
            pending_snapshots_.insert(delta.asset_id);
            // Fall through to apply changes in this chunk
            [[fallthrough]];

        case BookDeltaKind::SNAPSHOT_CHUNK:
            // Apply levels without recomputing BBO (deferred to SNAPSHOT_END)
            for (uint16_t i = 0; i < delta.change_count; ++i) {
                const auto& ch = delta.changes[i];
                // Directly set level in the book via price change path
                AssetPriceChange apc;
                apc.asset_id = delta.asset_id;
                apc.changes[0] = {ch.price, ch.side, ch.new_size};
                apc.change_count = 1;
                apc.best_bid = kInvalidPrice;  // don't override BBO
                apc.best_ask = kInvalidPrice;
                book.apply_price_change(apc);
            }
            break;

        case BookDeltaKind::SNAPSHOT_END:
            // Apply final chunk levels
            for (uint16_t i = 0; i < delta.change_count; ++i) {
                const auto& ch = delta.changes[i];
                AssetPriceChange apc;
                apc.asset_id = delta.asset_id;
                apc.changes[0] = {ch.price, ch.side, ch.new_size};
                apc.change_count = 1;
                apc.best_bid = kInvalidPrice;
                apc.best_ask = kInvalidPrice;
                book.apply_price_change(apc);
            }
            // Mark complete
            pending_snapshots_.erase(delta.asset_id);
            break;

        case BookDeltaKind::INCREMENTAL:
            // Handle tick size change
            if (delta.tick_size > 0) {
                book.set_tick_size(delta.tick_size);
            }
            // Apply incremental price changes
            for (uint16_t i = 0; i < delta.change_count; ++i) {
                const auto& ch = delta.changes[i];
                AssetPriceChange apc;
                apc.asset_id = delta.asset_id;
                apc.changes[0] = {ch.price, ch.side, ch.new_size};
                apc.change_count = 1;
                apc.best_bid = kInvalidPrice;
                apc.best_ask = kInvalidPrice;
                book.apply_price_change(apc);
            }
            break;
    }
}

const OrderBook* StrategyBookStore::book(const AssetId& id) const {
    auto it = books_.find(id);
    if (it == books_.end()) return nullptr;
    return &it->second;
}

const BBO* StrategyBookStore::bbo(const AssetId& id) const {
    auto it = books_.find(id);
    if (it == books_.end()) return nullptr;
    return &it->second.bbo();
}

bool StrategyBookStore::is_book_valid(const AssetId& id) const {
    auto it = books_.find(id);
    if (it == books_.end()) return false;
    return pending_snapshots_.find(id) == pending_snapshots_.end();
}

}  // namespace lt
