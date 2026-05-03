#pragma once

#include <cstdint>
#include <type_traits>

#include "common/types.h"
#include "book/book_types.h"
#include "scheduler/execution_mode.h"

namespace lt {

// Maximum book depth sent to UI per side
inline constexpr int kMaxUiBookDepth = 10;

// Maximum working orders in a UI state snapshot
inline constexpr int kMaxUiWorkingOrders = 64;

// Maximum closed orders in a UI state snapshot (bounded recent history)
inline constexpr int kMaxUiClosedOrders = 128;

// Maximum trade history entries in a UI state snapshot (bounded recent history)
inline constexpr int kMaxUiTrades = 128;

// Maximum position entries in a UI state snapshot
inline constexpr int kMaxUiPositions = 64;

struct UiTokenPosition {
    AssetId token_id;
    Qty_t position = 0;
};

// --- POD types for SPSC transport (T0->T6, T2->T6) ---

struct UiBookLevel {
    Price_t price = 0;
    Qty_t size = 0;
};

// Pushed by T0 after book updates, consumed by T6.
struct UiBookUpdate {
    AssetId asset_id;
    BBO bbo;
    UiBookLevel bids[kMaxUiBookDepth];
    UiBookLevel asks[kMaxUiBookDepth];
    int bid_count = 0;
    int ask_count = 0;
    Timestamp_ns timestamp = 0;
};

struct UiWorkingOrder {
    OrderId client_order_id;
    OrderId exchange_order_id;
    AssetId asset_id;
    AssetId market_id;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t filled_size = 0;
    bool is_live = false;
    bool is_pending = false;
    uint8_t lifecycle_state = 0;  // cast of UiOrderLifecycleState
    Timestamp_ns last_update_ts = 0;
};

enum class UiOrderLifecycleState : uint8_t {
    WORKING = 0,
    FILLED = 1,
    CANCELED_NO_FILL = 2,
    CANCELED_WITH_FILL = 3,
    REJECTED = 4,
};

struct UiClosedOrder {
    OrderId client_order_id;
    OrderId exchange_order_id;
    AssetId asset_id;
    AssetId market_id;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t filled_size = 0;
    uint8_t lifecycle_state = 0;  // cast of UiOrderLifecycleState
    Timestamp_ns last_update_ts = 0;
};

struct UiTrade {
    TradeId trade_id;
    OrderId order_id;
    AssetId asset_id;
    AssetId market_id;
    Side side = Side::BID;
    Price_t price = 0;
    Qty_t size = 0;
    uint8_t trade_status = 0;  // cast of TradeStatus
    Timestamp_ns last_update_ts = 0;
};

// Pushed by T2 once per cycle, consumed by T6.
struct UiStateSnapshot {
    UiWorkingOrder working_orders[kMaxUiWorkingOrders];
    int working_order_count = 0;

    UiClosedOrder closed_orders[kMaxUiClosedOrders];
    int closed_order_count = 0;

    UiTrade trades[kMaxUiTrades];
    int trade_count = 0;

    // Strategy params
    bool strategy_enabled = false;
    int spread_ticks = 0;
    Qty_t quote_size = 0;

    // Execution mode
    uint8_t execution_mode = 0;  // cast of ExecutionMode

    // Trading session state
    uint8_t session_state = 0;      // cast of SessionState
    int64_t session_end_time_s = 0; // 0 = indefinite
    int32_t session_markets_entered = 0;

    // Risk counters (snapshot from RiskGate)
    int64_t risk_checks = 0;
    int64_t risk_allows = 0;
    int64_t risk_denies = 0;

    // Position data (routed through T2 from inventory)
    UiTokenPosition positions[kMaxUiPositions];
    int position_count = 0;

    Timestamp_ns timestamp = 0;
};

static_assert(std::is_trivially_copyable_v<UiBookLevel>, "UiBookLevel must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiBookUpdate>, "UiBookUpdate must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiWorkingOrder>, "UiWorkingOrder must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiClosedOrder>, "UiClosedOrder must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiTrade>, "UiTrade must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiTokenPosition>, "UiTokenPosition must be trivially copyable");
static_assert(std::is_trivially_copyable_v<UiStateSnapshot>, "UiStateSnapshot must be trivially copyable");

}  // namespace lt
