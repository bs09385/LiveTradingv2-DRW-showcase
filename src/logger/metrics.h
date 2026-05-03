#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace lt {

// ---------------------------------------------------------------------------
// LatencyTracker: fixed-size circular buffer for computing percentiles.
// Single producer writes samples. T6 reads snapshots for percentile computation.
// No heap allocation. ~8KB per tracker.
// ---------------------------------------------------------------------------

enum class LatencyTrackerId : uint8_t {
    ORDER_RTT = 0,         // T3 writes (order placement RTT)
    CANCEL_RTT,            // T3 writes (cancel RTT)
    FULL_PIPELINE,         // T3 writes (market event → REST send)
    ENGINE_SPEED,          // T2 writes (strategy evaluate duration)
    WS_TO_PROCESS,         // T2 writes (WS frame recv → T2 processing)
    EXCHANGE_TO_RECV,      // T2 writes (exchange timestamp → local wall clock)
    BINANCE_MD_EXCH_TO_RECV, // T2 writes (Binance E/T field → local wall at recv);
                             // measures network/exchange latency. Only sampled
                             // for trade/aggTrade frames — bookTicker has no
                             // exchange timestamp.
    TRACKER_COUNT,
};

inline constexpr int kLatencyTrackerCount = static_cast<int>(LatencyTrackerId::TRACKER_COUNT);
inline constexpr int kLatencyRingSize = 8192;

class LatencyTracker {
public:
    void record(int64_t ns) {
        samples_[write_pos_ & kMask] = ns;
        ++write_pos_;
    }

    // Copy recent samples into caller-provided buffer. Returns count copied.
    // Caller sorts output to compute percentiles.
    int snapshot(int64_t* out, int max_out) const {
        uint32_t pos = write_pos_;
        int count = static_cast<int>(pos);
        if (count > kLatencyRingSize) count = kLatencyRingSize;
        if (count > max_out) count = max_out;
        for (int i = 0; i < count; ++i) {
            out[i] = samples_[(pos - count + i) & kMask];
        }
        return count;
    }

private:
    static constexpr uint32_t kMask = kLatencyRingSize - 1;
    static_assert((kLatencyRingSize & (kLatencyRingSize - 1)) == 0, "must be power of 2");
    int64_t samples_[kLatencyRingSize]{};
    uint32_t write_pos_ = 0;
};

// Computed percentiles from a LatencyTracker snapshot.
struct LatencyPercentiles {
    int64_t avg_ns = 0;
    int64_t p50_ns = 0;
    int64_t p95_ns = 0;
    int64_t p99_ns = 0;
    int count = 0;
};

// Compute percentiles from a buffer of latency samples. Sorts buf in-place.
LatencyPercentiles compute_percentiles(int64_t* buf, int count);

// Cross-thread probe result (T2 writes, T6 reads).
struct ProbeResult {
    std::atomic<int64_t> order_rtt_ns{0};
    std::atomic<int64_t> cancel_rtt_ns{0};
    std::atomic<int64_t> roundtrip_ns{0};
    std::atomic<uint8_t> status{0};  // 0=READY, 1=RUNNING, 2=DONE, 3=FAILED
};

// Hot metrics are written by multiple threads (T0/T1/T2/T3). Keep each
// counter on its own cache line to avoid cross-thread false sharing.
enum class MetricId : uint8_t {
    // WebSocket
    WS_FRAMES_RECEIVED = 0,
    WS_BYTES_RECEIVED,
    WS_RECONNECTS,
    WS_ERRORS,
    // Parser
    PARSE_OK,
    PARSE_ERRORS,
    PARSE_BOOK,
    PARSE_PRICE_CHANGE,
    PARSE_BEST_BID_ASK,
    PARSE_TICK_SIZE_CHANGE,
    PARSE_LAST_TRADE_PRICE,
    PARSE_PONG,
    PARSE_UNKNOWN,
    // Book
    BOOK_SNAPSHOTS,
    BOOK_UPDATES,
    BOOK_ERRORS,
    BOOK_DOWN_FILTERED,
    // Queue
    QUEUE_PUSHES,
    QUEUE_POPS,
    QUEUE_OVERFLOWS,
    // Scheduler (M1)
    SCHED_EVENTS,
    // Latency (cumulative ns for averaging)
    PARSE_LATENCY_NS,
    PARSE_LATENCY_COUNT,
    BOOK_LATENCY_NS,
    BOOK_LATENCY_COUNT,
    E2E_LATENCY_NS,
    E2E_LATENCY_COUNT,

    // Scheduler M2 metrics
    SCHED_CYCLES,
    SCHED_EVENTS_MARKET,
    SCHED_EVENTS_USER,
    SCHED_EVENTS_EXEC,
    SCHED_EVENTS_CONTROL,
    SCHED_EMPTY_POLLS,
    SCHED_STRATEGY_CALLS,
    SCHED_RISK_CHECKS,
    SCHED_INTENTS_PRODUCED,
    SCHED_INTENTS_ALLOWED,
    SCHED_QUEUE_OVERFLOWS_M2,
    // Scheduler M2 latency
    SCHED_RECV_TO_PROC_NS,
    SCHED_RECV_TO_PROC_COUNT,
    SCHED_STRAT_LATENCY_NS,
    SCHED_STRAT_LATENCY_COUNT,
    SCHED_LOOP_NS,
    SCHED_LOOP_COUNT,
    SCHED_MAX_BACKLOG,
    SCHED_QUOTE_CONVERSIONS,

    // User WS M3 metrics
    USER_WS_MESSAGES,
    USER_WS_PARSE_OK,
    USER_WS_PARSE_FAIL,
    USER_WS_ORDER_UPDATES,
    USER_WS_TRADE_UPDATES,
    USER_WS_FILLS,
    USER_WS_POSITION_DELTAS,
    USER_WS_RECONNECTS,
    USER_WS_STALE_DETECTED,
    USER_WS_DUPLICATES,
    USER_WS_QUEUE_OVERFLOW,
    USER_WS_CROSS_BOOK_FILL,      // fill matched on different book than our order's token
    USER_WS_TRADE_UNATTRIBUTED,   // trade skipped: no maker/taker match
    USER_WS_TAKER_FALLBACK,       // fill attributed via taker_order_id (no maker match)

    // Execution Gateway M4 metrics
    EXEC_REST_REQUESTS_ORDER,
    EXEC_REST_REQUESTS_CANCEL,
    EXEC_REST_REQUESTS_HEARTBEAT,
    EXEC_REST_REQUESTS_BATCH,
    EXEC_BATCH_ORDERS_SENT,
    EXEC_REST_SUCCESS,
    EXEC_REST_ERRORS,
    EXEC_HTTP_429,
    EXEC_HTTP_503,
    EXEC_HTTP_5XX,
    EXEC_HTTP_425,
    EXEC_HTTP_4XX,
    EXEC_RTT_NS,
    EXEC_RTT_COUNT,
    EXEC_RATE_THROTTLED,
    EXEC_HEARTBEAT_OK,
    EXEC_HEARTBEAT_FAIL,
    EXEC_GATEWAY_DEGRADED_COUNT,
    EXEC_INTENT_QUEUE_PUSHES,
    EXEC_INTENT_QUEUE_OVERFLOW,
    EXEC_LOCAL_INVENTORY_REJECTS,
    EXEC_TIMEOUT_COUNT,
    EXEC_AMBIGUOUS_COUNT,
    EXEC_CORRELATION_MATCHED,

    // Strategy / Risk M5 metrics
    STRAT_DESIRED_QUOTES,
    STRAT_ACTUAL_WORKING,
    STRAT_REPLACES,
    STRAT_CHURN_THROTTLED,
    STRAT_DRY_RUN_LOGGED,
    STRAT_MODE_BLOCKED,
    STRAT_RISK_DENIED,
    STRAT_CANCEL_ALL_TRIGGERED,
    STRAT_INVENTORY_REJECTIONS,
    STRAT_DEGRADED_CYCLES,
    STRAT_INTENT_LATENCY_NS,
    STRAT_INTENT_LATENCY_COUNT,
    STRAT_WORKING_ORDERS,
    STRAT_AMBIGUOUS_ORDERS,
    STRAT_TRACKER_DROPS,
    STRAT_NO_TRADE_ZONE,
    STRAT_TIER1_ACTIVATIONS,
    STRAT_TIER2_FAK_FIRES,
    STRAT_STOP_LOSS_FIRES,
    STRAT_CASCADE_BREAKER_FIRES,
    STRAT_TIME_GUARD_KILLS,
    STRAT_SLOT_ACTIVATE_SKIP,        // on_slot_activated early return (null ctx)
    STRAT_SLOT_ACTIVATE_NO_PAIR,     // on_slot_activated pair not found (race)
    STRAT_SPLIT_WAIT_SKIPS,          // quoting skipped while waiting for initial split

    // UI Bridge M6 metrics
    UI_SNAPSHOTS_SENT,
    UI_SNAPSHOTS_DROPPED,
    UI_COMMANDS_RECEIVED,
    UI_COMMANDS_INVALID,
    UI_WS_CONNECTED,
    UI_WS_DISCONNECTED,
    UI_BOOK_PUSHES,
    UI_COMMANDS_DROPPED,
    UI_BOOK_DROPS,
    UI_STATE_DROPS,

    // M7 Latency instrumentation
    STRAT_TO_EXEC_ENQUEUE_NS,
    STRAT_TO_EXEC_ENQUEUE_COUNT,
    EXEC_ENQUEUE_TO_SEND_NS,
    EXEC_ENQUEUE_TO_SEND_COUNT,
    FULL_PIPELINE_NS,
    FULL_PIPELINE_COUNT,

    // M7 Queue depth monitoring
    Q_MARKET_DEPTH,
    Q_USER_DEPTH,
    Q_EXEC_DEPTH,
    Q_CONTROL_DEPTH,
    Q_STRATEGY_TO_EXEC_DEPTH,
    Q_BINANCE_MD_DEPTH,
    Q_MARKET_HIGH_WATER,
    Q_USER_HIGH_WATER,
    Q_EXEC_HIGH_WATER,
    Q_BINANCE_MD_HIGH_WATER,

    // Market WS hardening metrics
    BBO_DIVERGENCE,
    NEW_MARKETS_RECEIVED,
    MARKETS_RESOLVED,

    // User WS hardening metrics
    USER_WS_AUTH_FAILURES,
    USER_WS_SERVER_ERRORS,
    USER_WS_UNKNOWN_EVENT_TYPES,
    USER_WS_FIELD_TRUNCATIONS,
    USER_WS_CLOSE_FRAMES,

    // Book delta queue (T0->T2 shadow books)
    BOOK_DELTA_PUSHES,
    BOOK_DELTA_DROPS,
    BOOK_DELTA_SNAPSHOT_CHUNKS,

    // RTDS (Real-Time Data Socket) metrics
    RTDS_MESSAGES,
    RTDS_PARSE_OK,
    RTDS_PARSE_FAIL,
    RTDS_PONG,
    RTDS_UNKNOWN,
    RTDS_RECONNECTS,
    RTDS_QUEUE_PUSHES,
    RTDS_QUEUE_DROPS,
    RTDS_RECORD_PUSHES,
    RTDS_RECORD_DROPS,

    // Binance market-data WebSocket metrics
    BINANCE_MD_MESSAGES,
    BINANCE_MD_PARSE_OK,
    BINANCE_MD_PARSE_FAIL,
    BINANCE_MD_UNKNOWN,
    BINANCE_MD_RECONNECTS,
    BINANCE_MD_QUEUE_PUSHES,
    BINANCE_MD_QUEUE_DROPS,
    BINANCE_MD_BOOK_TICKERS,
    BINANCE_MD_TRADES,

    MARKET_RECORD_PUSHES,
    MARKET_RECORD_DROPS,
    USER_RECORD_PUSHES,
    USER_RECORD_DROPS,

    // User WS recv-to-process latency (T1->T2)
    USER_RECV_TO_PROC_NS,
    USER_RECV_TO_PROC_COUNT,

    // Trade journal recording
    JOURNAL_RECORDS_PUSHED,
    JOURNAL_RECORDS_DROPPED,

    // Split order vs cancel RTT
    EXEC_ORDER_RTT_NS,
    EXEC_ORDER_RTT_COUNT,
    EXEC_CANCEL_RTT_NS,
    EXEC_CANCEL_RTT_COUNT,

    // Sentinel
    METRIC_COUNT,
};

inline constexpr int kMetricCount = static_cast<int>(MetricId::METRIC_COUNT);

class Metrics {
public:
    Metrics();

    void inc(MetricId id) {
        counters_[static_cast<int>(id)].value.fetch_add(1, std::memory_order_relaxed);
    }

    void add(MetricId id, int64_t value) {
        counters_[static_cast<int>(id)].value.fetch_add(value, std::memory_order_relaxed);
    }

    void record_latency(MetricId ns_id, MetricId count_id, int64_t latency_ns) {
        counters_[static_cast<int>(ns_id)].value.fetch_add(latency_ns, std::memory_order_relaxed);
        counters_[static_cast<int>(count_id)].value.fetch_add(1, std::memory_order_relaxed);
    }

    void record_latency_with_max(MetricId ns_id, MetricId count_id, int64_t latency_ns) {
        counters_[static_cast<int>(ns_id)].value.fetch_add(latency_ns, std::memory_order_relaxed);
        counters_[static_cast<int>(count_id)].value.fetch_add(1, std::memory_order_relaxed);
        // CAS loop for max tracking (relaxed — no contention on single-writer metrics)
        auto& cell = counters_[static_cast<int>(ns_id)];
        auto current = cell.max_ns.load(std::memory_order_relaxed);
        while (latency_ns > current &&
               !cell.max_ns.compare_exchange_weak(current, latency_ns, std::memory_order_relaxed)) {}
    }

    int64_t get_max(MetricId id) const {
        return counters_[static_cast<int>(id)].max_ns.load(std::memory_order_relaxed);
    }

    int64_t get(MetricId id) const {
        return counters_[static_cast<int>(id)].value.load(std::memory_order_relaxed);
    }

    // Get and reset (for periodic snapshots)
    int64_t get_and_reset(MetricId id) {
        return counters_[static_cast<int>(id)].value.exchange(0, std::memory_order_relaxed);
    }

    // Format all metrics as a string
    std::string dump() const;

    // Dump to a file (append)
    void dump_to_file(const std::string& path) const;

    static const char* metric_name(MetricId id);

    // Latency tracker access (ring buffers for percentile computation)
    LatencyTracker& tracker(LatencyTrackerId id) {
        return trackers_[static_cast<int>(id)];
    }
    const LatencyTracker& tracker(LatencyTrackerId id) const {
        return trackers_[static_cast<int>(id)];
    }

    // Probe result (T2 writes, T6 reads)
    ProbeResult probe_result;

private:
    struct alignas(64) CounterCell {
        std::atomic<int64_t> value{0};
        std::atomic<int64_t> max_ns{0};
    };
    static_assert(sizeof(CounterCell) >= 64, "CounterCell must occupy at least one cache line");
    std::array<CounterCell, kMetricCount> counters_;
    std::array<LatencyTracker, kLatencyTrackerCount> trackers_;
};

}  // namespace lt
