#pragma once

#include <string>
#include <vector>

#include "common/token_inventory.h"

namespace lt {

// Fetch current positions from the Polymarket Data API and seed TokenInventory.
// Queries GET /v1/market-positions per condition_id, filtered by user_address.
// Blocking call — suitable for startup only (before threads start).
// Returns the number of non-zero positions seeded.
int bootstrap_positions(const std::string& user_address,
                        const std::vector<std::string>& condition_ids,
                        TokenInventory& inventory,
                        int timeout_ms = 10000);

}  // namespace lt
