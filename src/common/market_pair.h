#pragma once

#include <unordered_map>

#include "common/error.h"
#include "common/types.h"

namespace lt {

struct MarketPair {
    AssetId condition_id;
    AssetId token_id_up;
    AssetId token_id_down;
    bool neg_risk = false;
    uint16_t fee_rate_bps = 0;
};

// Fixed-point complement price for binary outcomes.
// complement(p) = 1.0000 - p, with p expressed on the 10000x fixed scale.
inline Expected<Price_t> get_complement_price(Price_t price) {
    if (price < kPriceMin || price > kPriceMax) {
        return ErrorCode::OUT_OF_RANGE;
    }
    return Expected<Price_t>(static_cast<Price_t>(kPriceScale - price));
}

class MarketPairRegistry {
public:
    bool add_pair(const MarketPair& pair) {
        if (frozen_) return false;  // No mutations after freeze
        if (pair.condition_id.len == 0 || pair.token_id_up.len == 0 || pair.token_id_down.len == 0) {
            return false;
        }
        if (pair.token_id_up == pair.token_id_down) {
            return false;
        }
        if (by_condition_.find(pair.condition_id) != by_condition_.end()) {
            return false;
        }
        if (condition_by_token_.find(pair.token_id_up) != condition_by_token_.end()) {
            return false;
        }
        if (condition_by_token_.find(pair.token_id_down) != condition_by_token_.end()) {
            return false;
        }

        by_condition_.emplace(pair.condition_id, pair);
        condition_by_token_.emplace(pair.token_id_up, pair.condition_id);
        condition_by_token_.emplace(pair.token_id_down, pair.condition_id);
        complement_by_token_.emplace(pair.token_id_up, pair.token_id_down);
        complement_by_token_.emplace(pair.token_id_down, pair.token_id_up);
        return true;
    }

    bool add_pair(const AssetId& condition_id, const AssetId& token_id_up,
                  const AssetId& token_id_down) {
        MarketPair pair;
        pair.condition_id = condition_id;
        pair.token_id_up = token_id_up;
        pair.token_id_down = token_id_down;
        return add_pair(pair);
    }

    const MarketPair* find_by_condition(const AssetId& condition_id) const {
        auto it = by_condition_.find(condition_id);
        if (it == by_condition_.end()) return nullptr;
        return &it->second;
    }

    const AssetId* condition_for_token(const AssetId& token_id) const {
        auto it = condition_by_token_.find(token_id);
        if (it == condition_by_token_.end()) return nullptr;
        return &it->second;
    }

    const AssetId* get_complement_token(const AssetId& token_id) const {
        auto it = complement_by_token_.find(token_id);
        if (it == complement_by_token_.end()) return nullptr;
        return &it->second;
    }

    std::size_t size() const { return by_condition_.size(); }
    bool empty() const { return by_condition_.empty(); }

    // Freeze the registry. After this, no new pairs can be added.
    void freeze() { frozen_ = true; }
    bool is_frozen() const { return frozen_; }

    // Read-only access to all pairs keyed by condition_id
    const std::unordered_map<AssetId, MarketPair, AssetIdHash>& condition_map() const {
        return by_condition_;
    }

private:
    bool frozen_ = false;
    std::unordered_map<AssetId, MarketPair, AssetIdHash> by_condition_;
    std::unordered_map<AssetId, AssetId, AssetIdHash> condition_by_token_;
    std::unordered_map<AssetId, AssetId, AssetIdHash> complement_by_token_;
};

}  // namespace lt

