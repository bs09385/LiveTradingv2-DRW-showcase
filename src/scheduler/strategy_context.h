#pragma once

#include "common/types.h"
#include "events/scheduler_events.h"

namespace lt {

// Forward declarations
class StrategyBookStore;
class WorkingOrderTracker;
class MarketPairRegistry;
class StrategyStateStub;
class SlotTokenMap;
struct InventoryView;
class InventoryOpSink;

// Rich context passed to Strategy::evaluate(). Bundles all information the
// strategy needs to make quoting decisions:
//   - The triggering event (market/user/exec/control)
//   - Full shadow order books for all subscribed tokens (T2-owned copies)
//   - Token inventory (shared atomic reads)
//   - Working order state (T2-owned tracker)
//   - Market pair metadata (frozen after setup)
//   - Legacy StrategyStateStub for backward compatibility
//   - Current wall-clock timestamp
struct StrategyContext {
    const SchedulerEvent& event;
    const StrategyBookStore& books;
    const InventoryView* inventory;          // nullable
    InventoryOpSink* inventory_ops;          // nullable, non-hot-path side effects only
    const WorkingOrderTracker* tracker;      // nullable
    const MarketPairRegistry* pairs;         // nullable
    const StrategyStateStub& state;
    Timestamp_ns current_time;
    const SlotTokenMap* slot_map = nullptr;  // nullable, set when slot manager active
};

}  // namespace lt
