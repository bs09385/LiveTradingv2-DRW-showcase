#pragma once

#include "common/token_inventory.h"
#include "exec/exec_intent.h"

namespace lt {

struct InventoryCheckResult {
    bool allowed = true;
    Qty_t available = 0;
};

inline InventoryCheckResult check_inventory_for_intent(const ExecIntent& intent,
                                                       const InventoryView* inventory_view) {
    InventoryCheckResult out;

    if (intent.side != Side::ASK) {
        out.allowed = true;
        out.available = inventory_view ? inventory_view->position_for(intent.asset_id) : 0;
        return out;
    }

    out.available = inventory_view ? inventory_view->position_for(intent.asset_id) : 0;
    out.allowed = out.available >= intent.size;
    return out;
}

}  // namespace lt

