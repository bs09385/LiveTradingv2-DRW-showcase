#pragma once

#include <cstdint>

#include "common/types.h"

namespace lt {

enum class InventoryOpType : uint8_t {
    SPLIT = 0,
    MERGE,
    REDEEM,
};

// Fixed-size request for non-hot-path inventory operations.
// quantity is scaled by kQtyScale (0 means "all available" for redeem).
struct InventoryOpRequest {
    InventoryOpType type = InventoryOpType::SPLIT;
    AssetId condition_id;
    AssetId token_id;  // optional (mainly for redeem)
    AssetId token_id_up;    // for split/merge/redeem: UP token to adjust in TokenInventory
    AssetId token_id_down;  // for split/merge/redeem: DOWN token to adjust in TokenInventory
    Qty_t quantity = 0;
    uint32_t request_id = 0;
    Timestamp_ns created_ts = 0;
};

class InventoryOpSink {
public:
    virtual ~InventoryOpSink() = default;
    virtual bool try_request(const InventoryOpRequest& request) = 0;
};

inline const char* inventory_op_name(InventoryOpType type) {
    switch (type) {
        case InventoryOpType::SPLIT: return "split";
        case InventoryOpType::MERGE: return "merge";
        case InventoryOpType::REDEEM: return "redeem";
    }
    return "split";
}

}  // namespace lt
