#pragma once

#include <cstdint>
#include <string>

#include "common/error.h"

namespace lt {

// Fetch USDC balance from Polygon via eth_call to the USDC ERC-20 contract.
// Returns balance in micro-USDC (fp6) — the raw on-chain value (USDC has 6 decimals).
// Blocking call — suitable for startup and non-hot-path polling only.
Expected<int64_t> fetch_usdc_balance(const std::string& polygon_rpc_url,
                                     const std::string& address);

// Fetch native POL/MATIC balance via eth_getBalance.
// Returns balance in POL (double, e.g. 9.7 = 9.7 POL).
// Blocking call — suitable for non-hot-path polling only.
Expected<double> fetch_pol_balance(const std::string& polygon_rpc_url,
                                   const std::string& address);

}  // namespace lt
