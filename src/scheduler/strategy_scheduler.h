#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "common/types.h"
#include "common/market_pair.h"
#include "common/token_inventory.h"
#include "rotation/rotation_coordinator.h"
#include "slot/slot_token_map.h"
#include "ui_bridge/ui_types.h"
#include "events/event_variant.h"
#include "events/scheduler_events.h"
#include "logger/async_logger.h"
#include "logger/metrics.h"
#include "exec/exec_intent.h"
#include "inventory/inventory_sink.h"
#include "queue/spsc_queue.h"
#include "events/book_delta.h"
#include "binance/binance_types.h"
#include "rtds/rtds_types.h"
#include "recorder/journal_writer.h"
#include "scheduler/dry_run_simulator.h"
#include "scheduler/trading_session.h"
#include "scheduler/exec_intent_stub_sink.h"
#include "scheduler/execution_mode.h"
#include "scheduler/mode_filtered_sink.h"
#include "scheduler/quote_planner.h"
#include "scheduler/risk_gate.h"
#include "scheduler/risk_gate_stub.h"
#include "scheduler/strategy.h"
#include "scheduler/strategy_book_store.h"
#include "scheduler/strategy_state_stub.h"
#include "scheduler/strategy_stub.h"
#include "scheduler/working_order_tracker.h"

namespace lt {

// ---------------------------------------------------------------------------
// SchedulerConfig: runtime configuration for the T2 strategy scheduler.
// Populated from EngineConfig at startup.
// ---------------------------------------------------------------------------
struct SchedulerConfig {
    int max_user_events_per_pass = 256;
    int max_exec_events_per_pass = 256;
    int max_market_events_per_pass = 256;
    int max_control_events_per_pass = 64;
    int max_binance_md_events_per_pass = 256;
    int64_t stats_interval_ms = 5000;
    int poll_strategy = 2;      // 0=spin, 1=sleep, 2=hybrid
    int64_t sleep_us = 100;
    bool strategy_stub_emit_intents = false;
    bool exec_feedback_loop_enabled = true;  // M2 self-feedback; disable when real T3 exists

    // Validate and clamp fields to safe ranges. Returns false if config was
    // modified (caller should log a warning).
    bool validate() {
        bool clean = true;
        auto clamp = [&](int& v, int lo, int hi, const char*) {
            if (v < lo) { v = lo; clean = false; }
            if (v > hi) { v = hi; clean = false; }
        };
        clamp(max_user_events_per_pass, 1, 4096, "max_user_events_per_pass");
        clamp(max_exec_events_per_pass, 1, 4096, "max_exec_events_per_pass");
        clamp(max_market_events_per_pass, 1, 4096, "max_market_events_per_pass");
        clamp(max_control_events_per_pass, 1, 4096, "max_control_events_per_pass");
        clamp(max_binance_md_events_per_pass, 1, 4096, "max_binance_md_events_per_pass");
        if (poll_strategy < 0 || poll_strategy > 2) {
            poll_strategy = 2; clean = false;
        }
        if (sleep_us < 0) { sleep_us = 100; clean = false; }
        if (stats_interval_ms < 100) { stats_interval_ms = 100; clean = false; }
        return clean;
    }
};

// ---------------------------------------------------------------------------
// StrategyScheduler: unified T2 event loop.
//
// Consumes events from 4 input queues with deterministic priority ordering:
//   1. USER_WS      (highest priority — user order/trade updates)
//   2. EXEC_INTERNAL (execution feedback from T3/stub)
//   3. MARKET_WS     (market data from M1 pipeline)
//   4. CONTROL       (manual/UI commands, lowest priority)
//
// Processing flow per event:
//   pop -> wrap to SchedulerEvent -> update StrategyStateStub
//   -> determine if trigger -> call StrategyStub -> RiskGateStub
//   -> ExecIntentStubSink -> record metrics
//
// Fairness: each queue has a configurable per-pass work limit so that
// lower-priority queues are not starved under sustained high-priority load.
//
// Deterministic: given the same queued event ordering, processing order
// is reproducible (priority-ordered drain, FIFO within each queue).
//
// Hot-path constraints:
//   - No blocking I/O
//   - No synchronous logging (uses AsyncLogger)
//   - No locks on T2-owned data (single-writer design)
//   - No heap allocations in steady-state processing
//
// Thread ownership: T2 (dedicated scheduler thread).
// Queue ownership:
//   - market_queue: produced by T0, consumed by T2
//   - user_queue:   produced by future User WS thread (M3), consumed by T2
//   - exec_queue:   produced by T3/ExecIntentStubSink feedback, consumed by T2
//   - control_queue: produced by control/UI thread, consumed by T2
//
// Future integration points:
//   - M3: User WebSocket will push real events to user_queue
//   - M4: REST execution will replace ExecIntentStubSink, exec_queue
//         will carry real order acks/fills
// ---------------------------------------------------------------------------
class StrategyScheduler {
public:
    StrategyScheduler(SpscQueue<MarketNotification>& market_queue,
                      SpscQueue<SchedulerEvent>& user_queue,
                      SpscQueue<SchedulerEvent>& exec_queue,
                      SpscQueue<SchedulerEvent>& control_queue,
                      Metrics& metrics, AsyncLogger& logger,
                      const SchedulerConfig& config,
                      std::atomic<bool>* fatal_flag = nullptr,
                      ExecSink* external_sink = nullptr,
                      const MarketPairRegistry* market_pairs = nullptr,
                      const InventoryView* inventory_view = nullptr,
                      Strategy* strategy_ptr = nullptr,
                      RiskGate* risk_gate_ptr = nullptr,
                      ModeFilteredSink* mode_sink = nullptr,
                      WorkingOrderTracker* working_tracker = nullptr,
                      InventoryOpSink* inventory_op_sink = nullptr);

    // Main run loop — blocks until request_shutdown() is called.
    // Must be called from the T2 thread.
    void run();

    // Thread-safe shutdown request (can be called from any thread).
    void request_shutdown();

    // Enable event tracing (delegates to StrategyStateStub)
    void enable_tracing() { state_.enable_tracing(); }

    // Read-only accessors for testing and stats
    const StrategyStateStub& state() const { return state_; }
    const StrategyStub& strategy() const { return strategy_; }
    const RiskGateStub& risk_gate() const { return risk_gate_; }
    const ExecIntentStubSink& exec_sink() const { return exec_sink_; }
    ExecSink* active_sink() const { return active_sink_; }

    // M5 component accessors
    Strategy* strategy_ptr() const { return strategy_ptr_; }
    RiskGate* risk_gate_ptr() const { return risk_gate_ptr_; }
    ModeFilteredSink* mode_sink() const { return mode_sink_; }
    WorkingOrderTracker* working_tracker() const { return working_tracker_; }

    // M6: Set the UI state queue for T6 push. Rate-limited.
    void set_ui_state_queue(SpscQueue<UiStateSnapshot>* q, int rate_hz);

    // M7: Set T2->T3 exec intent queue for depth monitoring
    void set_exec_intent_queue(SpscQueue<ExecIntent>* q) { strategy_to_exec_queue_ = q; }

    // Set journal writer for trade journal recording (T2 producer)
    void set_journal_writer(JournalWriter* j) { journal_ = j; }

    // Set T0->T2 book delta queue for shadow books
    void set_book_delta_queue(SpscQueue<BookDelta>* q) { book_delta_queue_ = q; }

    // Set T_rtds->T2 crypto price queue
    void set_rtds_queue(SpscQueue<CryptoPriceUpdate>* q) { rtds_queue_ = q; }

    // Set T_binance_md->T2 Binance market-data queue
    void set_binance_md_queue(SpscQueue<BinanceMarketUpdate>* q) { binance_md_queue_ = q; }

    // Rotation: wire the coordinator for T7-T2 coordination (legacy)
    void set_rotation_coordinator(RotationCoordinator* coord);

    // Slot manager: wire T7->T2 slot event queue (replaces rotation coordinator)
    void set_slot_queue(SpscQueue<SchedulerEvent>* q) { slot_queue_ = q; }
    const SlotTokenMap& slot_token_map() const { return slot_token_map_; }

    // M7: Pre-reserve asset state capacity to reduce warmup allocations
    void reserve_asset_state(std::size_t n) { state_.reserve_assets(n); }
    void seed_asset_state(const AssetId& id) { state_.seed_asset(id); }
    void set_strict_asset_state(bool strict) { state_.set_strict_assets(strict); }

    // Shadow book store: pre-allocate and seed for all subscribed tokens
    void reserve_strategy_books(std::size_t n) { strategy_books_.reserve(n); }
    void seed_strategy_book(const AssetId& id) { strategy_books_.seed(id); }

    int64_t total_events() const { return state_.total_events(); }
    int64_t empty_polls() const { return empty_polls_; }
    int64_t max_backlog_observed() const { return max_backlog_observed_; }

private:
    // Process a single scheduler event through the full pipeline
    void process_event(const SchedulerEvent& event);
    void dump_stats();
    void maybe_push_ui_state();

    // UI account-view mirrors (T2-owned)
    void on_user_order_event_for_ui(const SchedulerEvent& event);
    void on_user_trade_event_for_ui(const SchedulerEvent& event);
    void on_exec_feedback_for_ui(const SchedulerEvent& event);

    // Send cancel intents for all working orders (bypasses IntentBatch cap)
    void send_all_cancels(ExecSink* sink);

    // Send cancel intents for all working orders in a specific market
    // (bypasses IntentBatch cap of 32 when tracker has up to 256 orders)
    void send_market_cancels(const AssetId& condition_id, ExecSink* sink);

    // Input queues
    SpscQueue<MarketNotification>& market_queue_;
    SpscQueue<SchedulerEvent>& user_queue_;
    SpscQueue<SchedulerEvent>& exec_queue_;
    SpscQueue<SchedulerEvent>& control_queue_;

    // Shared systems
    Metrics& metrics_;
    AsyncLogger& logger_;
    ProducerHandle log_handle_;

    // Config
    SchedulerConfig config_;

    // T2-owned components (no thread safety needed)
    StrategyStateStub state_;
    StrategyStub strategy_;
    QuotePlanner quote_planner_;
    RiskGateStub risk_gate_;
    ExecIntentStubSink exec_sink_;
    ExecSink* active_sink_ = nullptr;  // points to exec_sink_ or external sink

    // M5 optional components (externally owned, nullable for backward compat)
    Strategy* strategy_ptr_ = nullptr;
    RiskGate* risk_gate_ptr_ = nullptr;
    ModeFilteredSink* mode_sink_ = nullptr;
    WorkingOrderTracker* working_tracker_ = nullptr;

    // DRY_RUN synthetic feedback (T2-owned, zero-allocation)
    DryRunSimulator dry_run_sim_;

    // Trading session state machine (T2-owned)
    TradingSession session_;

    // Trade journal writer (T2-owned, nullable)
    JournalWriter* journal_ = nullptr;

    // M6 audit: stored for T2-safe position snapshot (avoids direct T6 reads)
    const InventoryView* inventory_view_ = nullptr;
    const MarketPairRegistry* market_pairs_ = nullptr;
    InventoryOpSink* inventory_op_sink_ = nullptr;
    uint32_t next_inventory_request_id_ = 1;

    // Control
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool>* fatal_flag_ = nullptr;  // set on unhandled exception

    // M7: T2->T3 exec intent queue for depth monitoring (optional, not owned)
    SpscQueue<ExecIntent>* strategy_to_exec_queue_ = nullptr;

    // T0->T2 book delta queue for shadow books (optional, not owned)
    SpscQueue<BookDelta>* book_delta_queue_ = nullptr;

    // T_rtds->T2 crypto price queue (optional, not owned)
    SpscQueue<CryptoPriceUpdate>* rtds_queue_ = nullptr;

    // T_binance_md->T2 Binance market-data queue (optional, not owned)
    SpscQueue<BinanceMarketUpdate>* binance_md_queue_ = nullptr;

    // T2-owned shadow order books (fed by book_delta_queue_)
    StrategyBookStore strategy_books_;

    // Rotation coordinator (optional, not owned) — legacy
    RotationCoordinator* rotation_coordinator_ = nullptr;
    std::atomic<int>* rotation_phase_ = nullptr;

    // Slot manager queue (optional, not owned) — new multi-timeframe path
    SpscQueue<SchedulerEvent>* slot_queue_ = nullptr;
    SlotTokenMap slot_token_map_;

    // --- Hot path (every cycle) ---
    // Batched per-cycle counters — flushed once per cycle to avoid per-event atomics
    struct CycleCounters {
        int64_t events = 0;
        int64_t events_market = 0;
        int64_t events_user = 0;
        int64_t events_exec = 0;
        int64_t events_control = 0;
        int64_t strategy_calls = 0;
        int64_t risk_checks = 0;
        int64_t intents_produced = 0;
        int64_t intents_allowed = 0;
        int64_t queue_pops_market = 0;
        int64_t queue_pops_user = 0;
        int64_t queue_pops_exec = 0;
        int64_t queue_pops_control = 0;
        int64_t queue_pops_binance_md = 0;

        void flush(Metrics& m) {
            if (events) m.add(MetricId::SCHED_EVENTS, events);
            if (events_market) m.add(MetricId::SCHED_EVENTS_MARKET, events_market);
            if (events_user) m.add(MetricId::SCHED_EVENTS_USER, events_user);
            if (events_exec) m.add(MetricId::SCHED_EVENTS_EXEC, events_exec);
            if (events_control) m.add(MetricId::SCHED_EVENTS_CONTROL, events_control);
            if (strategy_calls) m.add(MetricId::SCHED_STRATEGY_CALLS, strategy_calls);
            if (risk_checks) m.add(MetricId::SCHED_RISK_CHECKS, risk_checks);
            if (intents_produced) m.add(MetricId::SCHED_INTENTS_PRODUCED, intents_produced);
            if (intents_allowed) m.add(MetricId::SCHED_INTENTS_ALLOWED, intents_allowed);
            if (queue_pops_market) m.add(MetricId::QUEUE_POPS, queue_pops_market);
            // Note: per-queue pop metrics for user/exec/control could be added in future
            reset();
        }
        void reset() { *this = {}; }
    };
    CycleCounters cycle_counters_;

    // Latency tracking (T2-only, no atomics)
    int64_t recv_to_proc_max_ns_ = 0;

    // --- Cold path (periodic) ---
    Timestamp_ns last_stats_time_ = 0;
    int64_t empty_polls_ = 0;
    int64_t max_backlog_observed_ = 0;

    // M7: Per-queue high water marks (T2-local, flushed via delta to atomics)
    int64_t hw_market_ = 0;
    int64_t hw_user_ = 0;
    int64_t hw_exec_ = 0;
    int64_t hw_binance_md_ = 0;
    int64_t hw_backlog_ = 0;
    int64_t last_depth_market_ = 0;
    int64_t last_depth_user_ = 0;
    int64_t last_depth_exec_ = 0;
    int64_t last_depth_control_ = 0;
    int64_t last_depth_s2e_ = 0;
    int64_t last_depth_binance_md_ = 0;

    // M6 UI state push
    SpscQueue<UiStateSnapshot>* ui_state_queue_ = nullptr;
    Timestamp_ns last_ui_state_push_ts_ = 0;
    int64_t ui_state_push_interval_ns_ = 50'000'000;  // 50ms = 20Hz default

    struct UiOrderRecord {
        OrderId exchange_order_id;
        OrderId client_order_id;
        AssetId asset_id;
        AssetId market_id;
        Side side = Side::BID;
        Price_t price = 0;
        Qty_t original_size = 0;
        Qty_t filled_size = 0;
        uint8_t lifecycle_state = static_cast<uint8_t>(UiOrderLifecycleState::WORKING);
        bool is_live = false;
        bool is_pending = false;
        Timestamp_ns last_update_ts = 0;
        uint64_t update_seq = 0;
    };

    struct UiTradeRecord {
        TradeId trade_id;
        OrderId order_id;
        AssetId asset_id;
        AssetId market_id;
        Side side = Side::BID;
        Price_t price = 0;
        Qty_t size = 0;
        uint8_t trade_status = 0;  // cast of TradeStatus
        Timestamp_ns last_update_ts = 0;
        uint64_t update_seq = 0;
    };

    std::vector<UiOrderRecord> ui_account_working_orders_;
    std::vector<UiOrderRecord> ui_closed_orders_;
    std::vector<UiTradeRecord> ui_trade_history_;
    uint64_t ui_order_update_seq_ = 0;
    uint64_t ui_trade_update_seq_ = 0;

    // Pre-allocated scratch buffer for maybe_push_ui_state() — avoids
    // per-cycle heap allocation on the hot path.
    std::vector<UiOrderRecord> ui_working_scratch_;

    // --- Latency probe state machine (T2-owned) ---
    enum class ProbePhase : uint8_t {
        IDLE = 0,
        PLACING,          // Intent sent to T3, waiting for ORDER_ACCEPTED
        WAITING_CANCEL,   // Order ACK'd, waiting 1s before cancel
        CANCELLING,       // Cancel sent, waiting for CANCEL_CONFIRMED
        DONE,
        FAILED,
    };

    struct ProbeState {
        ProbePhase phase = ProbePhase::IDLE;
        OrderId client_order_id;
        OrderId exchange_order_id;
        uint32_t place_intent_id = 0;
        uint32_t cancel_intent_id = 0;
        Timestamp_ns place_sent_ts = 0;
        Timestamp_ns place_ack_ts = 0;
        Timestamp_ns cancel_sent_ts = 0;
        Timestamp_ns cancel_ack_ts = 0;
        Timestamp_ns probe_start_ts = 0;
        int64_t order_rtt_ns = 0;
        int64_t cancel_rtt_ns = 0;
        int64_t roundtrip_ns = 0;
        AssetId asset_id;
        AssetId market_id;
        void reset() { *this = {}; }
    };

    ProbeState probe_;
    uint32_t next_probe_intent_id_ = 900000;

    void handle_probe_feedback(const SchedulerEvent& event);
    void check_probe_cancel_timer();
    void check_probe_timeout();
};

}  // namespace lt
