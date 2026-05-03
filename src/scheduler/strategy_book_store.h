#pragma once

#include <unordered_map>
#include <unordered_set>

#include "book/order_book.h"
#include "common/types.h"
#include "events/book_delta.h"

namespace lt {

// T2-owned shadow order book store. Maintains copies of T0's order books
// fed via BookDelta messages through a dedicated SPSC queue.
//
// Reuses the existing OrderBook class — same dense array<Qty_t, 10001>,
// same apply_snapshot() / apply_price_change() internal methods.
//
// Snapshot safety: tracks "pending snapshot" state per-asset. Between
// SNAPSHOT_BEGIN and SNAPSHOT_END, is_book_valid() returns false.
// Strategy should not quote on invalid books.
//
// T2-owned: no thread safety needed.
class StrategyBookStore {
public:
    // Apply a single BookDelta to the appropriate shadow book.
    void apply_delta(const BookDelta& delta);

    // Look up a shadow book by asset ID. Returns nullptr if not seeded.
    const OrderBook* book(const AssetId& id) const;

    // Look up BBO for an asset. Returns nullptr if not seeded.
    const BBO* bbo(const AssetId& id) const;

    // Iterate all books.
    template<typename Func>
    void for_each_book(Func&& cb) const {
        for (const auto& [id, book] : books_) {
            cb(id, book);
        }
    }

    // Pre-allocate map capacity.
    void reserve(size_t n) { books_.reserve(n); }

    // Seed a book entry (creates empty OrderBook if not present).
    void seed(const AssetId& id) { books_.try_emplace(id); }

    // Returns false if the book is mid-snapshot (SNAPSHOT_BEGIN received
    // but SNAPSHOT_END not yet received).
    bool is_book_valid(const AssetId& id) const;

    // Number of tracked books.
    size_t size() const { return books_.size(); }

private:
    std::unordered_map<AssetId, OrderBook, AssetIdHash> books_;
    std::unordered_set<AssetId, AssetIdHash> pending_snapshots_;
};

}  // namespace lt
