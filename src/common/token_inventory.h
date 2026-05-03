#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "common/types.h"

namespace lt {

class InventoryView {
public:
    virtual ~InventoryView() = default;
    virtual Qty_t position_for(const AssetId& token_id) const = 0;
};

// Token inventory shared between fill ingestion (T1 writer), execution safety
// checks (T2/T3 readers), and inventory service (writer after ops).
//
// Thread safety:
//   - register_token() / set_position(new token) / freeze() take an exclusive
//     lock (called rarely from T7 during market discovery).
//   - position_for() / adjust_position() / set_position(existing) take a shared
//     lock (called frequently from T1/T2/T3 — near-zero contention).
//   - After freeze(), register_token() is a no-op and only the shared lock path
//     is exercised.
class TokenInventory final : public InventoryView {
public:
    // Register a token. Safe to call from any thread (takes exclusive lock).
    // No-op after freeze() or if token already registered.
    void register_token(const AssetId& token_id) {
        std::unique_lock lock(mu_);
        if (frozen_) return;
        if (positions_.find(token_id) == positions_.end()) {
            positions_[token_id] = std::make_unique<std::atomic<Qty_t>>(0);
        }
    }

    // Lock-free read (shared lock; near-zero overhead when uncontended).
    Qty_t position_for(const AssetId& token_id) const override {
        std::shared_lock lock(mu_);
        auto it = positions_.find(token_id);
        if (it == positions_.end()) return 0;
        return it->second->load(std::memory_order_relaxed);
    }

    // Atomic add (shared lock — map structure not modified).
    void adjust_position(const AssetId& token_id, Qty_t delta) {
        std::shared_lock lock(mu_);
        auto it = positions_.find(token_id);
        if (it != positions_.end()) {
            it->second->fetch_add(delta, std::memory_order_relaxed);
        }
    }

    // Set absolute position. Before freeze: auto-registers if needed (exclusive
    // lock). After freeze: only updates existing entries (shared lock).
    void set_position(const AssetId& token_id, Qty_t position) {
        // Fast path: token already exists — shared lock suffices.
        {
            std::shared_lock lock(mu_);
            auto it = positions_.find(token_id);
            if (it != positions_.end()) {
                it->second->store(position, std::memory_order_relaxed);
                return;
            }
            if (frozen_) return;  // Ignore new tokens after freeze
        }
        // Slow path: new token before freeze — exclusive lock.
        std::unique_lock lock(mu_);
        if (frozen_) return;
        positions_[token_id] = std::make_unique<std::atomic<Qty_t>>(position);
    }

    // Freeze the map structure. After this, register_token() is a no-op.
    void freeze() {
        std::unique_lock lock(mu_);
        frozen_ = true;
    }
    bool is_frozen() const {
        std::shared_lock lock(mu_);
        return frozen_;
    }

    std::size_t token_count() const {
        std::shared_lock lock(mu_);
        return positions_.size();
    }

    // USDC balance tracking (standalone atomic — no mutex needed).
    // Units: micro-USDC (fp6), same scale as Qty_t.
    void set_usdc_balance(int64_t balance) {
        usdc_balance_.store(balance, std::memory_order_relaxed);
    }
    int64_t usdc_balance() const {
        return usdc_balance_.load(std::memory_order_relaxed);
    }
    void adjust_usdc_balance(int64_t delta) {
        usdc_balance_.fetch_add(delta, std::memory_order_relaxed);
    }

    // Shared order ID tracking: T3 pre-registers predicted exchange_order_ids
    // BEFORE sending the REST request (derived from the EIP-712 signing hash).
    // T1 checks is_our_order() to determine maker vs taker for trade side
    // resolution. Pre-registration is essential for FAK orders: the exchange
    // matches instantly and the user WS trade event reaches T1 hundreds of
    // milliseconds before the REST response arrives at T3.
    void register_our_order(const OrderId& exchange_oid) {
        std::unique_lock lock(order_mu_);
        our_order_ids_.insert(std::string(exchange_oid.data, exchange_oid.len));
    }
    bool is_our_order(const OrderId& exchange_oid) const {
        std::shared_lock lock(order_mu_);
        return our_order_ids_.count(std::string(exchange_oid.data, exchange_oid.len)) > 0;
    }

private:
    mutable std::shared_mutex mu_;
    bool frozen_ = false;
    std::unordered_map<AssetId, std::unique_ptr<std::atomic<Qty_t>>, AssetIdHash> positions_;
    std::atomic<int64_t> usdc_balance_{0};

    // Separate mutex for order ID tracking (different access pattern from positions)
    mutable std::shared_mutex order_mu_;
    std::unordered_set<std::string> our_order_ids_;
};

}  // namespace lt
