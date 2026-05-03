#pragma once

#include <cstdint>
#include <unordered_map>

#include "common/types.h"
#include "events/scheduler_events.h"

namespace lt {

// ---------------------------------------------------------------------------
// Per-asset state tracked by T2 scheduler.
// T2-owned: no thread safety needed.
// ---------------------------------------------------------------------------
struct alignas(64) AssetSchedulerState {
    Timestamp_ns last_seen_ts = 0;
    int64_t event_count = 0;
    int64_t trigger_count = 0;
    BBOSnapshot last_bbo;
};

// ---------------------------------------------------------------------------
// Event trace entry for testing: records processing order.
// ---------------------------------------------------------------------------
struct EventTraceEntry {
    EventSource source;
    SchedulerEventKind kind;
    SeqNum_t seq;
};

// ---------------------------------------------------------------------------
// StrategyStateStub: T2-owned aggregate state for strategy evaluation.
//
// Tracks event counters, per-asset timestamps, trigger counts, and cycle count.
// Updated on every scheduler event. No network access, no file I/O.
//
// Optional event tracing: when enabled, records processing order for
// deterministic ordering tests. Trace buffer is fixed-size (no allocation).
//
// Future milestones will extend this with position tracking, P&L, etc.
// ---------------------------------------------------------------------------
class StrategyStateStub {
public:
    // Called by scheduler on every event
    void on_event(const SchedulerEvent& ev);

    // Global counters
    int64_t total_events() const { return total_events_; }
    int64_t cycle_count() const { return cycle_count_; }
    void increment_cycle() { ++cycle_count_; }
    int64_t trigger_count() const { return trigger_count_; }
    void increment_triggers() { ++trigger_count_; }

    // Per-source counts
    int64_t events_by_source(EventSource src) const {
        auto idx = static_cast<int>(src);
        if (idx < 0 || idx >= kEventSourceCount) return 0;
        return source_counts_[idx];
    }

    // Per-kind counts
    int64_t events_by_kind(SchedulerEventKind kind) const {
        auto idx = static_cast<int>(kind);
        if (idx < 0 || idx >= kSchedulerEventKindCount) return 0;
        return kind_counts_[idx];
    }

    // Per-asset state (returns nullptr if asset not yet seen)
    const AssetSchedulerState* get_asset_state(const AssetId& id) const;
    std::size_t asset_count() const { return asset_states_.size(); }

    // Pre-reserve asset map capacity to avoid hot-path allocation spikes
    void reserve_assets(std::size_t count) { asset_states_.reserve(count); }
    void seed_asset(const AssetId& id) { asset_states_.try_emplace(id); }
    void set_strict_assets(bool strict) { strict_assets_ = strict; }

    // Event trace (for testing ordering guarantees)
    void enable_tracing() { trace_enabled_ = true; }
    int trace_count() const { return trace_len_; }
    const EventTraceEntry& trace_entry(int idx) const { return trace_buf_[idx]; }

private:
    int64_t total_events_ = 0;
    int64_t cycle_count_ = 0;
    int64_t trigger_count_ = 0;
    int64_t source_counts_[kEventSourceCount]{};
    int64_t kind_counts_[kSchedulerEventKindCount]{};

    // Per-asset state. Map grows only when new assets first appear (not steady-state).
    std::unordered_map<AssetId, AssetSchedulerState, AssetIdHash> asset_states_;

    // Event trace buffer (fixed-size, no allocation)
    static constexpr int kMaxTraceEntries = 1024;
    EventTraceEntry trace_buf_[kMaxTraceEntries]{};
    int trace_len_ = 0;
    bool trace_enabled_ = false;
    bool strict_assets_ = false;
};

}  // namespace lt
