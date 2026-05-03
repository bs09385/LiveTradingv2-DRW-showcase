#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "common/types.h"

namespace lt {

// ---------------------------------------------------------------------------
// JournalId: compact 72-byte ID for binary journal records.
// Stores a truncated string (64 chars) plus FNV-1a hash of the full string
// for collision-free correlation when the full ID exceeds 64 chars.
// ---------------------------------------------------------------------------
struct JournalId {
    char data[64]{};
    uint64_t hash = 0;

    JournalId() = default;

    explicit JournalId(const AssetId& id) {
        auto copy_len = static_cast<uint8_t>(id.len < 64 ? id.len : 63);
        std::memcpy(data, id.data, copy_len);
        data[copy_len] = '\0';
        hash = fnv1a_hash(id.data, id.len);
    }

    explicit JournalId(const OrderId& id) {
        auto copy_len = static_cast<uint8_t>(id.len < 64 ? id.len : 63);
        std::memcpy(data, id.data, copy_len);
        data[copy_len] = '\0';
        hash = fnv1a_hash(id.data, id.len);
    }

    explicit JournalId(const TradeId& id) {
        auto copy_len = static_cast<uint8_t>(id.len < 64 ? id.len : 63);
        std::memcpy(data, id.data, copy_len);
        data[copy_len] = '\0';
        hash = fnv1a_hash(id.data, id.len);
    }
};
static_assert(sizeof(JournalId) == 72);
static_assert(std::is_trivially_copyable_v<JournalId>);

// ---------------------------------------------------------------------------
// JournalRecordType: the 8 record types in the trade journal.
// ---------------------------------------------------------------------------
enum class JournalRecordType : uint8_t {
    STRATEGY_EVAL  = 0,   // After strategy->evaluate() (level 1)
    RISK_DECISION  = 1,   // After risk_gate->evaluate() (level 1)
    ORDER_SENT     = 2,   // After sink->accept() succeeds (level 0)
    EXEC_FEEDBACK  = 3,   // T3 feedback processed (level 0)
    ORDER_STATUS   = 4,   // User WS order update (level 0)
    FILL           = 5,   // User WS trade with is_new_fill (level 0)
    MODE_CHANGE    = 6,   // Execution mode transition (level 0)
    CANCEL_ALL     = 7,   // Cancel-all triggered (level 0)
};

// ---------------------------------------------------------------------------
// RiskDenyReason: reason why risk gate denied an intent.
// Used by both journal records and risk_gate return values.
// ---------------------------------------------------------------------------
enum class RiskDenyReason : uint8_t {
    NONE             = 0,
    POSITION_LIMIT   = 1,
    EXPOSURE_LIMIT   = 2,
    NOTIONAL_LIMIT   = 3,
};

// ---------------------------------------------------------------------------
// JournalRecord: 256-byte fixed-size tagged union, trivially copyable POD.
//
// Layout:
//   Header  (32B):  wall_clock_ms | recv_ts | proc_ts | seq | type | flags | pad
//   Common  (72B):  asset_id (JournalId)
//   Payload (152B): union of type-specific structs
// ---------------------------------------------------------------------------

// --- Payload structs ---

struct JournalStrategyEval {
    Price_t bbo_bid = 0;
    Price_t bbo_ask = 0;
    Price_t desired_bid = 0;
    Price_t desired_ask = 0;
    Qty_t qty = 0;
    uint8_t intent_count = 0;
    uint8_t trigger_source = 0;   // cast of EventSource
    uint8_t trigger_kind = 0;     // cast of SchedulerEventKind
    uint8_t pad_[5]{};
};
static_assert(sizeof(JournalStrategyEval) <= 152);

struct JournalRiskDecision {
    uint8_t action = 0;           // cast of IntentAction
    uint8_t decision = 0;         // cast of RiskDecision
    uint8_t deny_reason = 0;      // cast of RiskDenyReason
    uint8_t side = 0;             // cast of Side
    Price_t price = 0;
    Qty_t qty = 0;
    Qty_t position = 0;
    int64_t notional = 0;
    uint8_t pad_[4]{};
};
static_assert(sizeof(JournalRiskDecision) <= 152);

struct JournalOrderSent {
    uint8_t action = 0;           // cast of IntentAction
    uint8_t side = 0;
    uint8_t level = 0;
    uint8_t pad1_ = 0;
    Price_t price = 0;
    Qty_t qty = 0;
    Price_t bbo_bid = 0;
    Price_t bbo_ask = 0;
    JournalId client_order_id;
};
static_assert(sizeof(JournalOrderSent) <= 152);

struct JournalExecFeedback {
    uint8_t feedback_kind = 0;    // cast of ExecFeedbackKind
    uint8_t pad1_[3]{};
    int http_status = 0;
    int64_t latency_ns = 0;
    JournalId order_id;           // exchange or client order ID (whichever is available)
};
static_assert(sizeof(JournalExecFeedback) <= 152);

struct JournalOrderStatus {
    JournalId order_id;
    uint8_t status = 0;           // cast of OrderStatus
    uint8_t side = 0;
    uint8_t pad_[2]{};
    Price_t price = 0;
    Qty_t original_size = 0;
    Qty_t filled_size = 0;
};
static_assert(sizeof(JournalOrderStatus) <= 152);

struct JournalFill {
    JournalId trade_id;
    Price_t fill_price = 0;
    Qty_t fill_size = 0;
    Qty_t net_position_after = 0;
    uint8_t side = 0;
    uint8_t pad_[3]{};
};
static_assert(sizeof(JournalFill) <= 152);

struct JournalModeChange {
    uint8_t old_mode = 0;
    uint8_t new_mode = 0;
    uint8_t pad_[6]{};
};
static_assert(sizeof(JournalModeChange) <= 152);

struct JournalCancelAll {
    uint8_t trigger_source = 0;   // cast of EventSource
    uint8_t pad_[3]{};
    int working_count = 0;
};
static_assert(sizeof(JournalCancelAll) <= 152);

// --- Main record ---

struct alignas(8) JournalRecord {
    // Header (32B)
    int64_t wall_clock_ms = 0;    // system time (ms since epoch)
    int64_t recv_ts = 0;          // monotonic receive timestamp (ns)
    int64_t proc_ts = 0;          // monotonic processing timestamp (ns)
    uint16_t seq = 0;             // per-session sequence (wraps)
    uint8_t type = 0;             // cast of JournalRecordType
    uint8_t flags = 0;            // bit 0: is_dry_run
    uint8_t pad_hdr_[4]{};

    // Common (72B)
    JournalId asset_id;

    // Payload (152B)
    union {
        JournalStrategyEval strategy_eval;
        JournalRiskDecision risk_decision;
        JournalOrderSent order_sent;
        JournalExecFeedback exec_feedback;
        JournalOrderStatus order_status;
        JournalFill fill;
        JournalModeChange mode_change;
        JournalCancelAll cancel_all;
        uint8_t raw[152];
    } payload{};
};

static_assert(sizeof(JournalRecord) == 256, "JournalRecord must be exactly 256 bytes");
static_assert(std::is_trivially_copyable_v<JournalRecord>, "JournalRecord must be trivially copyable");
static_assert(std::is_standard_layout_v<JournalRecord>, "JournalRecord must be standard layout");

// Flag bits
inline constexpr uint8_t kJournalFlagDryRun = 0x01;

// Journal level: 0 = lifecycle only, 1 = full (includes strategy/risk evals)
inline constexpr int kJournalLevelLifecycle = 0;
inline constexpr int kJournalLevelFull = 1;

}  // namespace lt
