#include "scheduler/strategy_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>

#include "common/clock.h"
#include "exec/exec_feedback.h"
#include "scheduler/quoter_v2_strategy.h"
#include "events/user_events.h"
#include "scheduler/trading_session.h"
#include "slot/market_slot_types.h"

namespace lt {

namespace {

uint8_t classify_lifecycle(OrderStatus status, Qty_t filled_size) {
    switch (status) {
        case OrderStatus::LIVE:
        case OrderStatus::PARTIAL:
        case OrderStatus::UNKNOWN:
            return static_cast<uint8_t>(UiOrderLifecycleState::WORKING);
        case OrderStatus::FILLED:
            return static_cast<uint8_t>(UiOrderLifecycleState::FILLED);
        case OrderStatus::CANCELED:
            return static_cast<uint8_t>(
                filled_size > 0 ? UiOrderLifecycleState::CANCELED_WITH_FILL
                                : UiOrderLifecycleState::CANCELED_NO_FILL);
        case OrderStatus::FAILED:
            return static_cast<uint8_t>(UiOrderLifecycleState::REJECTED);
    }
    return static_cast<uint8_t>(UiOrderLifecycleState::WORKING);
}

bool ids_match(const OrderId& exchange_a, const OrderId& client_a,
               const OrderId& exchange_b, const OrderId& client_b) {
    auto same_non_empty = [](const OrderId& lhs, const OrderId& rhs) {
        return lhs.len > 0 && rhs.len > 0 && lhs == rhs;
    };
    return same_non_empty(exchange_a, exchange_b) ||
           same_non_empty(client_a, client_b) ||
           same_non_empty(exchange_a, client_b) ||
           same_non_empty(client_a, exchange_b);
}

}  // namespace

StrategyScheduler::StrategyScheduler(SpscQueue<MarketNotification>& market_queue,
                                     SpscQueue<SchedulerEvent>& user_queue,
                                     SpscQueue<SchedulerEvent>& exec_queue,
                                     SpscQueue<SchedulerEvent>& control_queue,
                                     Metrics& metrics, AsyncLogger& logger,
                                     const SchedulerConfig& config,
                                     std::atomic<bool>* fatal_flag,
                                     ExecSink* external_sink,
                                     const MarketPairRegistry* market_pairs,
                                     const InventoryView* inventory_view,
                                     Strategy* strategy_ptr,
                                     RiskGate* risk_gate_ptr,
                                     ModeFilteredSink* mode_sink,
                                     WorkingOrderTracker* working_tracker,
                                     InventoryOpSink* inventory_op_sink)
    : market_queue_(market_queue),
      user_queue_(user_queue),
      exec_queue_(exec_queue),
      control_queue_(control_queue),
      metrics_(metrics),
      logger_(logger),
      log_handle_(logger.create_producer("scheduler2")),
      config_(config),
      strategy_(config.strategy_stub_emit_intents),
      quote_planner_(market_pairs, inventory_view, &metrics),
      exec_sink_(config.exec_feedback_loop_enabled ? &exec_queue : nullptr),
      active_sink_(external_sink ? external_sink : &exec_sink_),
      strategy_ptr_(strategy_ptr),
      risk_gate_ptr_(risk_gate_ptr),
      mode_sink_(mode_sink),
      working_tracker_(working_tracker),
      inventory_view_(inventory_view),
      market_pairs_(market_pairs),
      inventory_op_sink_(inventory_op_sink),
      fatal_flag_(fatal_flag) {
    // Wire DRY_RUN simulator into mode sink
    if (mode_sink_) {
        mode_sink_->set_dry_run_simulator(&dry_run_sim_);
        mode_sink_->set_logger(&logger, logger.create_producer("dry-sim"));
        if (mode_sink_->mode() == ExecutionMode::DRY_RUN) {
            dry_run_sim_.set_enabled(true);
        }
    }

    // Wire planner pointer into strategy for live tick size lookup
    if (strategy_ptr_) {
        strategy_ptr_->set_planner(&quote_planner_);
    }

    // Validate config (clamp out-of-range values)
    if (!config_.validate()) {
        AsyncLogger::log(log_handle_, LogLevel::WARN,
                         "SchedulerConfig had out-of-range values; clamped to safe defaults");
    }

    ui_account_working_orders_.reserve(kMaxUiWorkingOrders * 2);
}

void StrategyScheduler::run() {
    running_.store(true);
    last_stats_time_ = SteadyClock::now();

    int spin_count = 0;
    constexpr int kSpinLimit = 1000;
    constexpr int kYieldLimit = 100;

    AsyncLogger::log(log_handle_, LogLevel::INFO, "StrategyScheduler started");

    try {
        while (!stop_requested_.load(std::memory_order_relaxed)) {
            // Rotation pause protocol: T7 requests T2 to pause for market rotation
            if (rotation_phase_ &&
                rotation_phase_->load(std::memory_order_acquire) ==
                    static_cast<int>(RotationPhase::PAUSE_REQUESTED)) {
                // Cancel all working orders before pausing
                ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                            : active_sink_;
                send_all_cancels(sink);
                if (strategy_ptr_) strategy_ptr_->set_enabled(false);

                // Acknowledge pause
                rotation_phase_->store(static_cast<int>(RotationPhase::PAUSED),
                                       std::memory_order_release);
                AsyncLogger::log(log_handle_, LogLevel::INFO,
                                 "Rotation: T2 paused for market rotation");

                // Spin until resume requested (or shutdown)
                while (!stop_requested_.load(std::memory_order_relaxed)) {
                    auto phase = rotation_phase_->load(std::memory_order_acquire);
                    if (phase == static_cast<int>(RotationPhase::RESUME_REQUESTED)) break;
                    std::this_thread::yield();
                }

                if (stop_requested_.load(std::memory_order_relaxed)) break;

                // Apply new timing context and resume
                if (rotation_coordinator_ && strategy_ptr_) {
                    strategy_ptr_->set_rotation_timing(
                        rotation_coordinator_->timing_context());
                    strategy_ptr_->set_enabled(true);
                }

                rotation_phase_->store(static_cast<int>(RotationPhase::NORMAL),
                                       std::memory_order_release);
                AsyncLogger::log(log_handle_, LogLevel::INFO,
                                 "Rotation: T2 resumed after market rotation");
            }

            auto cycle_start = SteadyClock::now();
            int events_this_cycle = 0;

            // -----------------------------------------------------------
            // Phase 0: drain ALL pending book deltas (no per-pass limit)
            // -----------------------------------------------------------
            if (book_delta_queue_) {
                while (auto* delta = book_delta_queue_->front()) {
                    strategy_books_.apply_delta(*delta);
                    book_delta_queue_->pop();
                }
            }

            // -----------------------------------------------------------
            // Phase 1: USER_WS (highest priority)
            // -----------------------------------------------------------
            for (int i = 0; i < config_.max_user_events_per_pass; ++i) {
                auto* ev = user_queue_.front();
                if (!ev) break;
                SchedulerEvent se = *ev;
                user_queue_.pop();
                ++cycle_counters_.queue_pops_user;
                process_event(se);
                ++events_this_cycle;
            }

            // -----------------------------------------------------------
            // Phase 2: EXEC_INTERNAL
            // -----------------------------------------------------------
            for (int i = 0; i < config_.max_exec_events_per_pass; ++i) {
                auto* ev = exec_queue_.front();
                if (!ev) break;
                SchedulerEvent se = *ev;
                exec_queue_.pop();
                ++cycle_counters_.queue_pops_exec;
                process_event(se);
                ++events_this_cycle;
            }

            // -----------------------------------------------------------
            // Phase 2.5: DRY_RUN synthetic feedback (T2-local, no queue)
            // -----------------------------------------------------------
            if (dry_run_sim_.enabled()) {
                SchedulerEvent sim_ev;
                while (dry_run_sim_.pop(sim_ev)) {
                    process_event(sim_ev);
                    ++events_this_cycle;
                    ++cycle_counters_.events_exec;
                }
            }

            // -----------------------------------------------------------
            // Phase 3: MARKET_WS
            // -----------------------------------------------------------
            for (int i = 0; i < config_.max_market_events_per_pass; ++i) {
                auto* notif = market_queue_.front();
                if (!notif) break;
                SchedulerEvent se = SchedulerEvent::from_market(*notif);
                market_queue_.pop();
                ++cycle_counters_.queue_pops_market;
                process_event(se);
                ++events_this_cycle;
            }

            // -----------------------------------------------------------
            // Phase 3.5: RTDS crypto prices (between market and control)
            // -----------------------------------------------------------
            if (rtds_queue_ && strategy_ptr_) {
                // Drain all pending crypto prices — small messages, no per-pass limit
                while (auto* price = rtds_queue_->front()) {
                    strategy_ptr_->on_crypto_price(*price);
                    rtds_queue_->pop();
                    ++events_this_cycle;
                }
            }

            // Binance Spot market-data (T_binance_md -> T2)
            // Bounded drain — Binance bookTicker can burst at hundreds of msg/s
            // and we must not starve the higher-priority USER/EXEC/MARKET phases
            // during a backlog. Cap matches the convention used by other data
            // queues. Sentinel frames (CONNECTED / DISCONNECTED) ride the same
            // queue and count toward the cap; they're rare so this is fine.
            //
            // Latency tracking: BINANCE_MD_EXCH_TO_RECV records the network
            // round trip from when Binance announced the trade (exchange_ts_ms,
            // the T or E field) to when our WS client got the bytes off the
            // wire (recv_wall_ms, stamped in the on_message lambda). This is
            // the network/exchange latency, not the local pipeline delay.
            // Only trade/aggTrade frames carry an exchange timestamp; bookTicker
            // does not, so it is excluded. Negative deltas (clock skew) are
            // discarded so the displayed avg/p95 reflect real positive latency.
            if (binance_md_queue_ && strategy_ptr_) {
                for (int i = 0; i < config_.max_binance_md_events_per_pass; ++i) {
                    auto* upd = binance_md_queue_->front();
                    if (!upd) break;
                    if (upd->kind == static_cast<uint8_t>(BinanceUpdateKind::DATA) &&
                        upd->exchange_ts_ms > 0 && upd->recv_wall_ms > 0) {
                        int64_t delta_ms = upd->recv_wall_ms - upd->exchange_ts_ms;
                        if (delta_ms >= 0) {
                            metrics_.tracker(LatencyTrackerId::BINANCE_MD_EXCH_TO_RECV)
                                .record(delta_ms * 1'000'000LL);
                        }
                    }
                    strategy_ptr_->on_binance_update(*upd);
                    binance_md_queue_->pop();
                    ++cycle_counters_.queue_pops_binance_md;
                    ++events_this_cycle;
                }
            }

            // -----------------------------------------------------------
            // Phase 3.75: Slot lifecycle events (T7 -> T2 via slot_queue)
            // -----------------------------------------------------------
            if (slot_queue_) {
                while (auto* ev = slot_queue_->front()) {
                    SchedulerEvent se = *ev;
                    slot_queue_->pop();
                    process_event(se);
                    ++events_this_cycle;
                }
            }

            // -----------------------------------------------------------
            // Phase 4: CONTROL (lowest priority)
            // -----------------------------------------------------------
            for (int i = 0; i < config_.max_control_events_per_pass; ++i) {
                auto* ev = control_queue_.front();
                if (!ev) break;
                SchedulerEvent se = *ev;
                control_queue_.pop();
                ++cycle_counters_.queue_pops_control;
                process_event(se);
                ++events_this_cycle;

                // Handle CONTROL_SHUTDOWN
                if (se.kind == SchedulerEventKind::CONTROL_SHUTDOWN) {
                    AsyncLogger::log(log_handle_, LogLevel::INFO,
                                     "Received CONTROL_SHUTDOWN event");
                    stop_requested_.store(true, std::memory_order_relaxed);
                    break;
                }
            }

            // Track cycle
            state_.increment_cycle();

            // Flush batched counters to atomics once per cycle
            cycle_counters_.flush(metrics_);
            metrics_.inc(MetricId::SCHED_CYCLES);

            // M6: Push state snapshot to UI bridge (timer-gated)
            maybe_push_ui_state();
            check_probe_cancel_timer();
            check_probe_timeout();

            if (events_this_cycle > 0) {
                spin_count = 0;
                auto cycle_dt = SteadyClock::now() - cycle_start;
                metrics_.record_latency(MetricId::SCHED_LOOP_NS,
                                        MetricId::SCHED_LOOP_COUNT, cycle_dt);
            } else {
                ++empty_polls_;
                metrics_.inc(MetricId::SCHED_EMPTY_POLLS);

                // Backoff strategy
                if (config_.poll_strategy == 0) {
                    // Pure spin
#if defined(__x86_64__) || defined(__i386__)
                    __builtin_ia32_pause();
#endif
                } else if (config_.poll_strategy == 1) {
                    // Pure sleep
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(config_.sleep_us));
                } else {
                    // Hybrid: spin -> yield -> sleep
                    ++spin_count;
                    if (spin_count < kSpinLimit) {
#if defined(__x86_64__) || defined(__i386__)
                        __builtin_ia32_pause();
#endif
                    } else if (spin_count < kSpinLimit + kYieldLimit) {
                        std::this_thread::yield();
                    } else {
                        std::this_thread::sleep_for(
                            std::chrono::microseconds(config_.sleep_us));
                    }
                }
            }

            // M7: Per-queue depth sampling and high water marks
            {
                auto mkt_depth = static_cast<int64_t>(market_queue_.size());
                auto usr_depth = static_cast<int64_t>(user_queue_.size());
                auto exc_depth = static_cast<int64_t>(exec_queue_.size());
                auto ctl_depth = static_cast<int64_t>(control_queue_.size());

                // Store latest depth (overwrite semantics via local delta cache)
                auto store_depth = [&](MetricId id, int64_t depth, int64_t& last_depth) {
                    if (depth != last_depth) {
                        metrics_.add(id, depth - last_depth);
                        last_depth = depth;
                    }
                };
                store_depth(MetricId::Q_MARKET_DEPTH, mkt_depth, last_depth_market_);
                store_depth(MetricId::Q_USER_DEPTH, usr_depth, last_depth_user_);
                store_depth(MetricId::Q_EXEC_DEPTH, exc_depth, last_depth_exec_);
                store_depth(MetricId::Q_CONTROL_DEPTH, ctl_depth, last_depth_control_);

                if (strategy_to_exec_queue_) {
                    auto s2e = static_cast<int64_t>(strategy_to_exec_queue_->size());
                    store_depth(MetricId::Q_STRATEGY_TO_EXEC_DEPTH, s2e, last_depth_s2e_);
                }

                int64_t bmd_depth = 0;
                if (binance_md_queue_) {
                    bmd_depth = static_cast<int64_t>(binance_md_queue_->size());
                    store_depth(MetricId::Q_BINANCE_MD_DEPTH, bmd_depth,
                                last_depth_binance_md_);
                }

                // Update high water marks
                if (mkt_depth > hw_market_) {
                    metrics_.add(MetricId::Q_MARKET_HIGH_WATER, mkt_depth - hw_market_);
                    hw_market_ = mkt_depth;
                }
                if (usr_depth > hw_user_) {
                    metrics_.add(MetricId::Q_USER_HIGH_WATER, usr_depth - hw_user_);
                    hw_user_ = usr_depth;
                }
                if (exc_depth > hw_exec_) {
                    metrics_.add(MetricId::Q_EXEC_HIGH_WATER, exc_depth - hw_exec_);
                    hw_exec_ = exc_depth;
                }
                if (bmd_depth > hw_binance_md_) {
                    metrics_.add(MetricId::Q_BINANCE_MD_HIGH_WATER,
                                 bmd_depth - hw_binance_md_);
                    hw_binance_md_ = bmd_depth;
                }

                // Legacy backlog tracking
                auto backlog = mkt_depth + usr_depth + exc_depth + ctl_depth;
                if (backlog > max_backlog_observed_) {
                    max_backlog_observed_ = backlog;
                }
                if (backlog > hw_backlog_) {
                    metrics_.add(MetricId::SCHED_MAX_BACKLOG, backlog - hw_backlog_);
                    hw_backlog_ = backlog;
                }
            }

            // Periodic stats dump + stale tracker GC
            auto now = SteadyClock::now();
            if (now - last_stats_time_ > config_.stats_interval_ms * 1'000'000LL) {
                dump_stats();

                // GC pending tracker entries older than 30s (covers TIMEOUT ambiguity)
                if (working_tracker_) {
                    static constexpr int64_t kStaleAgeNs = 30'000'000'000LL;  // 30s
                    int removed = working_tracker_->gc_stale_pending(now, kStaleAgeNs);
                    if (removed > 0) {
                        char gc_buf[LogEntry::kMaxMsg];
                        std::snprintf(gc_buf, sizeof(gc_buf),
                                      "WorkingOrderTracker GC: removed %d stale pending entries",
                                      removed);
                        AsyncLogger::log(log_handle_, LogLevel::WARN, gc_buf);
                        metrics_.add(MetricId::STRAT_TRACKER_DROPS, removed);
                    }
                }

                last_stats_time_ = now;
            }
        }
    } catch (const std::exception& ex) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "FATAL Scheduler exception: %s", ex.what());
        AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
    } catch (...) {
        AsyncLogger::log(log_handle_, LogLevel::ERROR, "FATAL Scheduler unknown exception");
        if (fatal_flag_) fatal_flag_->store(true, std::memory_order_relaxed);
    }

    running_.store(false);
    AsyncLogger::log(log_handle_, LogLevel::INFO, "StrategyScheduler stopped");
}

void StrategyScheduler::request_shutdown() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

void StrategyScheduler::process_event(const SchedulerEvent& event) {
    auto proc_start = SteadyClock::now();

    // Track recv-to-proc latency
    if (event.recv_ts > 0) {
        auto latency = proc_start - event.recv_ts;
        if (latency > recv_to_proc_max_ns_) recv_to_proc_max_ns_ = latency;
        metrics_.record_latency(MetricId::SCHED_RECV_TO_PROC_NS,
                                MetricId::SCHED_RECV_TO_PROC_COUNT, latency);
        // Track user-specific recv-to-process latency separately
        if (event.source == EventSource::USER_WS) {
            metrics_.record_latency(MetricId::USER_RECV_TO_PROC_NS,
                                    MetricId::USER_RECV_TO_PROC_COUNT, latency);
        }
    }

    // Update T2-owned strategy state
    state_.on_event(event);

    // Keep QuotePlanner tick map synchronized with live market tick updates.
    if (event.source == EventSource::MARKET_WS &&
        event.kind == SchedulerEventKind::MARKET_TICK_SIZE_CHANGE &&
        event.market_tick_size > 0) {
        quote_planner_.set_tick_size(event.asset_id, event.market_tick_size);
    }

    // Journal: user WS events (level 0)
    if (journal_ && event.source == EventSource::USER_WS) {
        if (event.kind == SchedulerEventKind::USER_ORDER_UPDATE) {
            journal_->record_order_status(event);
        } else if (event.kind == SchedulerEventKind::USER_TRADE_UPDATE &&
                   event.is_new_fill) {
            Qty_t net_pos = inventory_view_
                ? inventory_view_->position_for(event.asset_id)
                : 0;
            journal_->record_fill(event, net_pos);
        }
    }

    // Maintain account-wide UI mirrors from user/exec lifecycle events.
    if (event.source == EventSource::USER_WS) {
        if (event.kind == SchedulerEventKind::USER_ORDER_UPDATE) {
            on_user_order_event_for_ui(event);
        } else if (event.kind == SchedulerEventKind::USER_TRADE_UPDATE) {
            on_user_trade_event_for_ui(event);
        }
    } else if (event.source == EventSource::EXEC_INTERNAL) {
        // Skip UI order tracking in DRY_RUN: synthetic feedback creates
        // zero-value records (tracker disabled, no price/size to copy).
        if (!(mode_sink_ && mode_sink_->mode() == ExecutionMode::DRY_RUN)) {
            on_exec_feedback_for_ui(event);
        }
    }

    // Feed WorkingOrderTracker before strategy evaluation (M5).
    // Skip in DRY_RUN: tracker is not used (no real exchange orders).
    bool tracker_active = working_tracker_ &&
        !(mode_sink_ && mode_sink_->mode() == ExecutionMode::DRY_RUN);
    if (tracker_active) {
        if (event.source == EventSource::EXEC_INTERNAL) {
            working_tracker_->on_exec_feedback(event);
        } else if (event.source == EventSource::USER_WS) {
            working_tracker_->on_user_update(event);
        }
    }

    // Journal: exec feedback (level 0)
    if (journal_ && event.source == EventSource::EXEC_INTERNAL) {
        journal_->record_exec_feedback(event);
    }

    // Route exec feedback to strategy callbacks (M5)
    if (strategy_ptr_ && event.source == EventSource::EXEC_INTERNAL) {
        auto fb_kind = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);
        if (fb_kind == ExecFeedbackKind::GATEWAY_DEGRADED) {
            strategy_ptr_->on_gateway_degraded();
        } else if (fb_kind == ExecFeedbackKind::GATEWAY_RECOVERED) {
            strategy_ptr_->on_gateway_recovered();
        } else if (fb_kind == ExecFeedbackKind::EXCHANGE_UNAVAILABLE) {
            // Emergency stop: cancel all orders + switch to DRY_RUN
            {
                ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                            : active_sink_;
                send_all_cancels(sink);
                ExecutionIntent cancel_all;
                cancel_all.action = IntentAction::WOULD_CANCEL_ALL;
                cancel_all.created_ts = SteadyClock::now();
                sink->accept(cancel_all);
            }
            if (mode_sink_) {
                if (journal_) {
                    journal_->record_mode_change(mode_sink_->mode(), ExecutionMode::DRY_RUN);
                }
                mode_sink_->set_mode(ExecutionMode::DRY_RUN);
            }
            dry_run_sim_.set_enabled(true);
            if (session_.is_active() || session_.is_pending()) {
                session_.reset();
                AsyncLogger::log(log_handle_, LogLevel::WARN,
                                 "Session ended — exchange unavailable");
            }
            AsyncLogger::log(log_handle_, LogLevel::WARN,
                             "Exchange unavailable (503) — emergency stop, switched to DRY_RUN");
        } else if (fb_kind == ExecFeedbackKind::RATE_LIMITED) {
            strategy_ptr_->on_rate_limited();
        }
    }

    // Handle M5 control events
    if (event.source == EventSource::CONTROL) {
        switch (event.kind) {
            case SchedulerEventKind::CONTROL_SET_MODE: {
                auto new_mode = static_cast<ExecutionMode>(event.control_mode);
                // Journal: mode change (level 0)
                if (journal_) {
                    auto old_mode = mode_sink_ ? mode_sink_->mode()
                                               : ExecutionMode::DRY_RUN;
                    journal_->record_mode_change(old_mode, new_mode);
                }
                if (mode_sink_) {
                    mode_sink_->set_mode(new_mode);
                }
                // Sync dry-run simulator and strategy dry_run flag
                dry_run_sim_.set_enabled(new_mode == ExecutionMode::DRY_RUN);
                if (strategy_ptr_) {
                    auto* q2 = dynamic_cast<QuoterV2Strategy*>(strategy_ptr_);
                    if (q2) q2->set_dry_run(new_mode == ExecutionMode::DRY_RUN);
                }
                if (new_mode != ExecutionMode::DRY_RUN) {
                    dry_run_sim_.reset();
                    // Clear phantom dry-run orders from tracker and strategy
                    if (working_tracker_) working_tracker_->clear_all();
                    if (strategy_ptr_) strategy_ptr_->reset_all_quotes();
                }

                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf), "Execution mode changed to %s",
                              execution_mode_name(new_mode));
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }
            case SchedulerEventKind::CONTROL_SET_SPREAD:
                // No longer settable from UI — strategy owns its own params
                break;
            case SchedulerEventKind::CONTROL_SET_SIZE:
                // Scale human-readable integer to kQtyScale before passing to strategy
                if (strategy_ptr_) {
                    strategy_ptr_->set_size(
                        static_cast<Qty_t>(event.control_int_param) * kQtyScale);
                }
                break;
            case SchedulerEventKind::CONTROL_ENABLE_STRATEGY:
                if (strategy_ptr_) {
                    strategy_ptr_->set_enabled(event.control_bool_param);
                }
                break;
            case SchedulerEventKind::CONTROL_CANCEL_ALL: {
                ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                            : active_sink_;
                // Journal: cancel-all (level 0)
                if (journal_) {
                    int working = working_tracker_
                        ? working_tracker_->working_count() : 0;
                    journal_->record_cancel_all(event.source, working);
                }
                send_all_cancels(sink);

                // Also send exchange-wide DELETE /cancel-all for orders not in tracker
                // (e.g. placed via website). Goes through mode_sink_ which always
                // forwards WOULD_CANCEL_ALL regardless of mode.
                ExecSink* cancel_all_sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                                       : active_sink_;
                ExecutionIntent cancel_all;
                cancel_all.action = IntentAction::WOULD_CANCEL_ALL;
                cancel_all.created_ts = SteadyClock::now();
                cancel_all_sink->accept(cancel_all);

                metrics_.inc(MetricId::STRAT_CANCEL_ALL_TRIGGERED);
                break;
            }
            case SchedulerEventKind::CONTROL_INVENTORY_SPLIT:
            case SchedulerEventKind::CONTROL_INVENTORY_MERGE:
            case SchedulerEventKind::CONTROL_INVENTORY_REDEEM: {
                if (!inventory_op_sink_) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Inventory control command ignored: sink not configured");
                    break;
                }

                InventoryOpRequest req;
                req.type = InventoryOpType::SPLIT;
                if (event.kind == SchedulerEventKind::CONTROL_INVENTORY_MERGE) {
                    req.type = InventoryOpType::MERGE;
                } else if (event.kind == SchedulerEventKind::CONTROL_INVENTORY_REDEEM) {
                    req.type = InventoryOpType::REDEEM;
                }
                req.condition_id = event.control_condition_id;
                req.token_id = event.control_token_id;
                req.quantity = event.control_qty_param;
                req.request_id = next_inventory_request_id_++;
                req.created_ts = SteadyClock::now();

                if (!inventory_op_sink_->try_request(req)) {
                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "Inventory request queue full: op=%s cond=%.*s",
                                  inventory_op_name(req.type),
                                  static_cast<int>(req.condition_id.len),
                                  req.condition_id.data);
                    AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
                }
                break;
            }
            // --- Slot lifecycle events ---
            case SchedulerEventKind::SLOT_ACTIVATED: {
                auto slot = static_cast<SlotName>(event.slot_name);
                // Look up token pair from MarketPairRegistry and add to map
                if (market_pairs_) {
                    const MarketPair* pair = market_pairs_->find_by_condition(
                        event.control_condition_id);
                    if (pair && !slot_token_map_.find_by_condition(event.control_condition_id)) {
                        slot_token_map_.add(slot, event.control_condition_id,
                                            pair->token_id_up, pair->token_id_down,
                                            is_current_slot(slot));
                    }
                }
                slot_token_map_.set_active(slot, true);
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf), "T2: SLOT_ACTIVATED %s cond=%.*s",
                              slot_name_str(slot),
                              static_cast<int>(event.control_condition_id.len),
                              event.control_condition_id.data);
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }
            case SchedulerEventKind::SLOT_CLOSING: {
                auto slot = static_cast<SlotName>(event.slot_name);
                slot_token_map_.set_active(slot, false);
                // Cancel all working orders for this condition immediately
                if (working_tracker_) {
                    IntentBatch cancels = working_tracker_->cancel_intents_for_market(
                        event.control_condition_id);
                    ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                                : active_sink_;
                    for (int i = 0; i < cancels.count; ++i) {
                        sink->accept(cancels.intents[i]);
                    }
                }
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf), "T2: SLOT_CLOSING %s — cancelled orders, cond=%.*s",
                              slot_name_str(slot),
                              static_cast<int>(event.control_condition_id.len),
                              event.control_condition_id.data);
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }
            case SchedulerEventKind::SLOT_PROMOTED: {
                auto slot = static_cast<SlotName>(event.slot_name);
                // The NEXT slot has been relabeled to CURRENT by T7.
                // Relabel in SlotTokenMap: NEXT_5M -> CURRENT_5M (or 15M).
                BtcTimeframe tf = slot_timeframe(slot);
                slot_token_map_.promote(next_slot_for(tf), current_slot_for(tf));
                slot_token_map_.set_active(current_slot_for(tf), true);

                // Trading session: handle PENDING→ACTIVE and end-time boundary
                int64_t window_end_s = event.control_qty_param;
                if (session_.is_pending()) {
                    if (session_.exceeds_end(window_end_s)) {
                        AsyncLogger::log(log_handle_, LogLevel::WARN,
                                         "Session end time already passed, cancelling session");
                        session_.reset();
                    } else {
                        // Clean market boundary → go LIVE
                        session_.state = SessionState::ACTIVE;
                        auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        session_.started_at_ms = wall_ms;
                        if (mode_sink_) {
                            if (journal_) {
                                journal_->record_mode_change(mode_sink_->mode(), ExecutionMode::LIVE);
                            }
                            mode_sink_->set_mode(ExecutionMode::LIVE);
                        }
                        dry_run_sim_.set_enabled(false);
                        dry_run_sim_.reset();
                        if (strategy_ptr_) {
                            auto* q2 = dynamic_cast<QuoterV2Strategy*>(strategy_ptr_);
                            if (q2) q2->set_dry_run(false);
                        }
                        AsyncLogger::log(log_handle_, LogLevel::INFO,
                                         "Session ACTIVE — trading LIVE");
                    }
                } else if (session_.is_active()) {
                    session_.markets_entered++;
                    if (session_.exceeds_end(window_end_s)) {
                        // Session boundary reached — end session
                        ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                                    : active_sink_;
                        send_all_cancels(sink);
                        ExecutionIntent cancel_all;
                        cancel_all.action = IntentAction::WOULD_CANCEL_ALL;
                        cancel_all.created_ts = SteadyClock::now();
                        sink->accept(cancel_all);
                        if (mode_sink_) {
                            if (journal_) {
                                journal_->record_mode_change(mode_sink_->mode(), ExecutionMode::DRY_RUN);
                            }
                            mode_sink_->set_mode(ExecutionMode::DRY_RUN);
                        }
                        dry_run_sim_.set_enabled(true);
                        if (strategy_ptr_) {
                            strategy_ptr_->set_enabled(false);
                            auto* q2 = dynamic_cast<QuoterV2Strategy*>(strategy_ptr_);
                            if (q2) q2->set_dry_run(true);
                        }
                        session_.reset();
                        AsyncLogger::log(log_handle_, LogLevel::INFO,
                                         "Session ended — strategy disabled, end time reached");
                    }
                }
                break;
            }
            case SchedulerEventKind::SLOT_DEMOTED: {
                auto slot = static_cast<SlotName>(event.slot_name);
                // CURRENT relabeled to PREVIOUS: deactivate, keep in map
                slot_token_map_.set_active(slot, false);
                BtcTimeframe tf = slot_timeframe(slot);
                slot_token_map_.demote(current_slot_for(tf), previous_slot_for(tf));
                break;
            }
            case SchedulerEventKind::SLOT_REMOVED: {
                auto slot = static_cast<SlotName>(event.slot_name);
                slot_token_map_.remove(slot);
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf), "T2: SLOT_REMOVED %s — tokens removed from map",
                              slot_name_str(slot));
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }

            // --- Trading session events ---
            case SchedulerEventKind::CONTROL_START_SESSION: {
                if (!session_.is_idle()) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Start session ignored — session already active or pending");
                    break;
                }
                session_.end_time_s = event.control_qty_param;
                session_.effective_end_s = compute_effective_end(session_.end_time_s);
                session_.state = SessionState::PENDING;
                // Auto-enable strategy so it evaluates in DRY_RUN while pending
                if (strategy_ptr_) {
                    strategy_ptr_->set_enabled(true);
                }
                char buf[LogEntry::kMaxMsg];
                if (session_.effective_end_s > 0) {
                    std::snprintf(buf, sizeof(buf),
                        "Session started, waiting for next market (effective end: %lld)",
                        static_cast<long long>(session_.effective_end_s));
                } else {
                    std::snprintf(buf, sizeof(buf),
                        "Session started, waiting for next market (indefinite)");
                }
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }
            case SchedulerEventKind::CONTROL_STOP_SESSION: {
                if (session_.is_idle()) break;
                // Emergency stop: cancel all, DRY_RUN, reset
                ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                            : active_sink_;
                send_all_cancels(sink);
                ExecutionIntent cancel_all;
                cancel_all.action = IntentAction::WOULD_CANCEL_ALL;
                cancel_all.created_ts = SteadyClock::now();
                sink->accept(cancel_all);
                if (mode_sink_) {
                    if (journal_) {
                        journal_->record_mode_change(mode_sink_->mode(), ExecutionMode::DRY_RUN);
                    }
                    mode_sink_->set_mode(ExecutionMode::DRY_RUN);
                }
                dry_run_sim_.set_enabled(true);
                session_.reset();
                AsyncLogger::log(log_handle_, LogLevel::INFO,
                                 "Session stopped manually");
                break;
            }

            case SchedulerEventKind::CONTROL_LATENCY_PROBE: {
                if (probe_.phase != ProbePhase::IDLE &&
                    probe_.phase != ProbePhase::DONE &&
                    probe_.phase != ProbePhase::FAILED) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Probe already in progress");
                    break;
                }
                if (mode_sink_ && mode_sink_->mode() != ExecutionMode::LIVE) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Probe requires LIVE mode — set mode to LIVE first");
                    metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                    break;
                }
                if (strategy_ptr_ && strategy_ptr_->enabled()) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Probe unavailable while strategy is running");
                    metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                    break;
                }

                // Find a token to probe: use first active slot token, or any registered market
                AssetId probe_asset;
                AssetId probe_market;
                bool found = false;
                bool neg_risk = false;
                uint16_t fee_rate = 0;

                // Try slot token map first (populated by T7) — prefer active (CURRENT) slots
                for (int i = 0; i < slot_token_map_.count() && !found; ++i) {
                    const auto& entry = slot_token_map_.entry(i);
                    if (!entry.active) continue;
                    if (market_pairs_) {
                        const MarketPair* pair = market_pairs_->find_by_condition(entry.condition_id);
                        if (pair) {
                            // Use UP token (arbitrary — $0.01 bid can't cross)
                            probe_asset = pair->token_id_up;
                            probe_market = pair->condition_id;
                            neg_risk = pair->neg_risk;
                            fee_rate = pair->fee_rate_bps;
                            found = true;
                        }
                    }
                }

                // Fallback: any market in the registry
                if (!found && market_pairs_) {
                    for (const auto& [cond_id, pair] : market_pairs_->condition_map()) {
                        probe_asset = pair.token_id_up;
                        probe_market = pair.condition_id;
                        neg_risk = pair.neg_risk;
                        fee_rate = pair.fee_rate_bps;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Probe failed: no registered market");
                    metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                    break;
                }

                probe_.reset();
                probe_.phase = ProbePhase::PLACING;
                probe_.probe_start_ts = SteadyClock::now();
                probe_.asset_id = probe_asset;
                probe_.market_id = probe_market;
                probe_.place_intent_id = next_probe_intent_id_++;

                char probe_oid[32];
                std::snprintf(probe_oid, sizeof(probe_oid), "PROBE_%u", probe_.place_intent_id);
                probe_.client_order_id = OrderId(std::string_view(probe_oid));

                ExecutionIntent place;
                place.action = IntentAction::WOULD_PLACE_BID;
                place.asset_id = probe_asset;
                place.market_id = probe_market;
                place.client_order_id = probe_.client_order_id;
                place.price = 100;  // $0.01
                place.qty = qty_from_int(5);
                place.intent_id = probe_.place_intent_id;
                place.created_ts = SteadyClock::now();
                place.order_type = OrderType::GTC;
                place.neg_risk = neg_risk;
                place.fee_rate_bps = fee_rate;

                probe_.place_sent_ts = SteadyClock::now();

                ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                            : active_sink_;
                auto result = sink->accept(place);
                if (result != SinkResult::ACCEPTED) {
                    probe_.phase = ProbePhase::FAILED;
                    metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Probe place intent rejected by sink");
                } else {
                    metrics_.probe_result.status.store(1, std::memory_order_relaxed);
                    AsyncLogger::log(log_handle_, LogLevel::INFO,
                                     "Latency probe: order placed at $0.01");
                }
                break;
            }

            case SchedulerEventKind::CONTROL_MARKET_SELL_ALL:
            case SchedulerEventKind::CONTROL_MARKET_SELL_DOWN: {
                bool sell_down = (event.kind == SchedulerEventKind::CONTROL_MARKET_SELL_DOWN);
                // SELL_ALL sells both UP and DOWN (dual-bid model accumulates both).
                // SELL_DOWN sells DOWN only.
                const char* label = sell_down ? "SELL_DOWN" : "SELL_ALL";

                if (!mode_sink_) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Market sell: no execution sink available");
                    break;
                }

                // Disable strategy to prevent re-quoting after flatten
                if (strategy_ptr_) strategy_ptr_->set_enabled(false);

                // Temporarily force LIVE mode so FAK sells go to the real
                // exchange (sell buttons must work even after session stop).
                ExecutionMode prev_mode = mode_sink_->mode();
                if (prev_mode != ExecutionMode::LIVE) {
                    mode_sink_->set_mode(ExecutionMode::LIVE);
                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                        "%s: temporarily switching %s->LIVE for emergency sell",
                        label, prev_mode == ExecutionMode::DRY_RUN ? "DRY_RUN" : "UNKNOWN");
                    AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
                }

                ExecSink* sink = static_cast<ExecSink*>(mode_sink_);
                // First cancel all working orders
                send_all_cancels(sink);

                // FAK sell tokens with positive position.
                // SELL_ALL: sell both UP and DOWN (dual-bid accumulates both).
                // SELL_DOWN: sell DOWN only.
                if (inventory_view_ && market_pairs_) {
                    static uint32_t sell_all_id = 800000;
                    for (const auto& [cond_id, pair] : market_pairs_->condition_map()) {
                        // Build list of tokens to sell
                        const AssetId* tokens[2];
                        int token_count = 0;
                        if (!sell_down) {
                            // SELL_ALL: sell both UP and DOWN
                            tokens[token_count++] = &pair.token_id_up;
                            tokens[token_count++] = &pair.token_id_down;
                        } else {
                            // SELL_DOWN: sell DOWN only
                            tokens[token_count++] = &pair.token_id_down;
                        }

                        for (int t = 0; t < token_count; ++t) {
                            const AssetId& token = *tokens[t];
                            Qty_t pos = inventory_view_->position_for(token);
                            if (pos <= 0) continue;
                            constexpr Qty_t kShareRound = 10000;
                            Qty_t rounded = (pos / kShareRound) * kShareRound;
                            if (rounded <= 0) continue;

                            ExecutionIntent fak;
                            fak.action = IntentAction::WOULD_PLACE_ASK;
                            fak.asset_id = token;
                            fak.market_id = cond_id;
                            fak.price = 100;  // sweep price $0.01
                            fak.qty = rounded;
                            fak.order_type = OrderType::FAK;
                            fak.intent_id = sell_all_id++;
                            fak.created_ts = SteadyClock::now();
                            fak.neg_risk = pair.neg_risk;
                            fak.fee_rate_bps = pair.fee_rate_bps;
                            sink->accept(fak);

                            char buf[LogEntry::kMaxMsg];
                            std::snprintf(buf, sizeof(buf),
                                "%s: FAK SELL %.*s %lld shares",
                                label, static_cast<int>(token.len), token.data,
                                static_cast<long long>(rounded / kQtyScale));
                            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                        }
                    }
                }

                // Restore previous mode (stay in DRY_RUN so strategy doesn't re-quote)
                if (prev_mode != ExecutionMode::LIVE) {
                    mode_sink_->set_mode(prev_mode);
                }
                break;
            }

            default:
                break;
        }
    }

    // Handle probe feedback matching (after normal exec feedback processing)
    if (event.source == EventSource::EXEC_INTERNAL) {
        handle_probe_feedback(event);
    }

    // Batch per-source counts (flushed once per cycle, not per event)
    switch (event.source) {
        case EventSource::MARKET_WS:   ++cycle_counters_.events_market; break;
        case EventSource::USER_WS:     ++cycle_counters_.events_user;   break;
        case EventSource::EXEC_INTERNAL: ++cycle_counters_.events_exec; break;
        case EventSource::CONTROL:     ++cycle_counters_.events_control; break;
    }
    ++cycle_counters_.events;

    // Determine if this event should trigger strategy evaluation
    bool should_trigger = false;
    switch (event.source) {
        case EventSource::MARKET_WS:
            should_trigger = (event.kind == SchedulerEventKind::MARKET_BOOK_SNAPSHOT ||
                              event.kind == SchedulerEventKind::MARKET_PRICE_CHANGE ||
                              event.kind == SchedulerEventKind::MARKET_BBO_UPDATE ||
                              event.kind == SchedulerEventKind::MARKET_RESOLVED);
            break;
        case EventSource::USER_WS:
            should_trigger = true;  // always trigger on user events
            break;
        case EventSource::EXEC_INTERNAL:
            should_trigger = true;  // always trigger on exec feedback
            break;
        case EventSource::CONTROL:
            // Slot lifecycle events must reach strategy for market state init/cleanup
            should_trigger = (event.kind == SchedulerEventKind::SLOT_ACTIVATED ||
                              event.kind == SchedulerEventKind::SLOT_CLOSING);
            break;
    }

    if (should_trigger) {
        state_.increment_triggers();

        // Call strategy: polymorphic if M5 strategy provided, else stub
        auto strat_start = SteadyClock::now();
        IntentBatch intents;
        if (strategy_ptr_) {
            // In DRY_RUN, suppress real inventory ops (split/merge/redeem)
            // by passing nullptr — strategy null-checks this everywhere.
            bool is_live = mode_sink_ && mode_sink_->mode() != ExecutionMode::DRY_RUN;
            StrategyContext ctx{
                event, strategy_books_, inventory_view_,
                is_live ? inventory_op_sink_ : nullptr,
                working_tracker_,
                market_pairs_, state_, SteadyClock::now(),
                slot_queue_ ? &slot_token_map_ : nullptr};
            intents = strategy_ptr_->evaluate(ctx);
        } else {
            intents = strategy_.evaluate(event, state_);
        }
        auto strat_dt = SteadyClock::now() - strat_start;

        ++cycle_counters_.strategy_calls;
        metrics_.record_latency(MetricId::SCHED_STRAT_LATENCY_NS,
                                MetricId::SCHED_STRAT_LATENCY_COUNT, strat_dt);
        metrics_.tracker(LatencyTrackerId::ENGINE_SPEED).record(strat_dt);

        // Journal: strategy evaluation (level 1)
        if (journal_ && intents.count > 0) {
            journal_->record_strategy_eval(
                event, event.bbo.best_bid, event.bbo.best_ask,
                intents.intents[0].price,
                intents.count > 1 ? intents.intents[1].price : 0,
                intents.intents[0].qty, intents.count);
        }

        // Determine which risk gate and sink to use
        ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                    : active_sink_;

        // Enrich intents with neg_risk and fee_rate_bps from MarketPairRegistry
        if (market_pairs_) {
            for (int i = 0; i < intents.count; ++i) {
                const auto& mid = intents.intents[i].market_id;
                if (mid.len > 0) {
                    const MarketPair* pair = market_pairs_->find_by_condition(mid);
                    if (pair) {
                        intents.intents[i].neg_risk = pair->neg_risk;
                        intents.intents[i].fee_rate_bps = pair->fee_rate_bps;
                    }
                }
            }
        }

        // Pipeline: planner BEFORE risk gate.
        // Planner is currently pass-through, so risk checks the strategy's
        // exact token/side/price/qty instruction.
        int planned_count = 0;
        for (int i = 0; i < intents.count; ++i) {
            IntentBatch planned = quote_planner_.plan(intents.intents[i]);
            planned_count += planned.count;

            for (int j = 0; j < planned.count; ++j) {
                RiskResult risk_result = risk_gate_ptr_
                    ? risk_gate_ptr_->evaluate(planned.intents[j])
                    : RiskResult{risk_gate_.evaluate(planned.intents[j]),
                                 RiskDenyReason::NONE};
                ++cycle_counters_.risk_checks;

                if (risk_result.decision != RiskDecision::ALLOW) {
                    // Journal: risk denied (level 1)
                    if (journal_) {
                        Qty_t position = inventory_view_
                            ? inventory_view_->position_for(planned.intents[j].asset_id)
                            : 0;
                        int64_t notional = working_tracker_
                            ? working_tracker_->total_working_notional() : 0;
                        journal_->record_risk_decision(
                            planned.intents[j], risk_result.decision,
                            risk_result.reason, position, notional);
                    }
                    continue;
                }
                ++cycle_counters_.intents_allowed;

                // Journal: risk allowed (level 1)
                if (journal_) {
                    Qty_t position = inventory_view_
                        ? inventory_view_->position_for(planned.intents[j].asset_id)
                        : 0;
                    int64_t notional = working_tracker_
                        ? working_tracker_->total_working_notional() : 0;
                    journal_->record_risk_decision(
                        planned.intents[j], risk_result.decision,
                        risk_result.reason, position, notional);
                }

                auto result = sink->accept(planned.intents[j]);
                if (result == SinkResult::OVERFLOW) {
                    AsyncLogger::log(log_handle_, LogLevel::WARN,
                                     "Exec feedback queue overflow - event dropped");
                }
                // Track sent intents in WorkingOrderTracker (M5).
                // Skip in DRY_RUN: no real exchange orders to track, and the
                // sim feedback cycle creates phantom slots that never get cancelled.
                bool track_in_tracker = working_tracker_ &&
                    !(mode_sink_ && mode_sink_->mode() == ExecutionMode::DRY_RUN);
                if (result == SinkResult::ACCEPTED && track_in_tracker) {
                    if (!working_tracker_->on_intent_sent(planned.intents[j])) {
                        metrics_.inc(MetricId::STRAT_TRACKER_DROPS);
                        AsyncLogger::log(log_handle_, LogLevel::ERROR,
                                         "WorkingOrderTracker full - placement untracked");
                    }
                }
                // Journal: order sent (level 0)
                if (result == SinkResult::ACCEPTED && journal_) {
                    journal_->record_order_sent(
                        planned.intents[j], event.bbo.best_bid, event.bbo.best_ask);
                }
            }
        }

        if (planned_count > 0) {
            cycle_counters_.intents_produced += planned_count;
        }

        // Check risk gate pending_cancel_all flag (M5)
        if (risk_gate_ptr_ && risk_gate_ptr_->pending_cancel_all()) {
            send_all_cancels(sink);
            risk_gate_ptr_->clear_pending_cancel_all();
            metrics_.inc(MetricId::STRAT_CANCEL_ALL_TRIGGERED);
        }

        // For MARKET_RESOLVED: cancel all working orders in the resolved market
        // directly (bypasses IntentBatch 32-cap vs 256 tracker slots).
        if (event.kind == SchedulerEventKind::MARKET_RESOLVED &&
            event.resolved_condition_id.len > 0) {
            send_market_cancels(event.resolved_condition_id, sink);
        }
    }

    // Track end-to-end event processing latency
    if (event.recv_ts > 0) {
        auto e2e = SteadyClock::now() - event.recv_ts;
        metrics_.record_latency(MetricId::E2E_LATENCY_NS,
                                MetricId::E2E_LATENCY_COUNT, e2e);
        if (event.source == EventSource::MARKET_WS) {
            metrics_.tracker(LatencyTrackerId::WS_TO_PROCESS).record(e2e);
            // Exchange-to-receive: compare exchange wall clock (ms) to local wall clock
            if (event.exchange_ts > 0) {
                auto wall_now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                auto delta_ms = wall_now_ms - event.exchange_ts;
                if (delta_ms > 0 && delta_ms < 60000) {  // sanity: 0-60s
                    metrics_.tracker(LatencyTrackerId::EXCHANGE_TO_RECV).record(delta_ms * 1'000'000);
                }
            }
        }
    }
}

void StrategyScheduler::on_user_order_event_for_ui(const SchedulerEvent& event) {
    OrderId resolved_exchange_id = event.order_id;
    OrderId resolved_client_id = event.client_order_id;
    AssetId resolved_asset_id = event.asset_id;
    AssetId resolved_market_id = event.user_market_id;
    Side resolved_side = event.user_side;
    Price_t resolved_price = event.user_price;
    Qty_t resolved_original_size = event.user_original_size;
    Qty_t resolved_filled_size = event.user_cumulative_filled;

    auto hydrate_from_tracker = [&]() {
        if (!working_tracker_) return;
        const WorkingOrder* wo = nullptr;
        if (resolved_client_id.len > 0) {
            wo = working_tracker_->find_by_client_id(resolved_client_id);
        }
        if (!wo && resolved_exchange_id.len > 0) {
            wo = working_tracker_->find_by_exchange_id(resolved_exchange_id);
        }
        if (!wo && resolved_market_id.len > 0) {
            const WorkingOrder* by_market =
                working_tracker_->find_by_market_side(resolved_market_id, resolved_side);
            if (by_market &&
                (resolved_asset_id.len == 0 || by_market->asset_id == resolved_asset_id) &&
                (resolved_price <= 0 || by_market->price == resolved_price)) {
                wo = by_market;
            }
        }
        if (!wo) return;

        if (resolved_exchange_id.len == 0 && wo->exchange_order_id.len > 0) {
            resolved_exchange_id = wo->exchange_order_id;
        }
        if (resolved_client_id.len == 0 && wo->client_order_id.len > 0) {
            resolved_client_id = wo->client_order_id;
        }
        if (resolved_asset_id.len == 0 && wo->asset_id.len > 0) {
            resolved_asset_id = wo->asset_id;
        }
        if (resolved_market_id.len == 0 && wo->market_id.len > 0) {
            resolved_market_id = wo->market_id;
        }
        if (resolved_price == 0) {
            resolved_price = wo->price;
        }
        if (resolved_original_size <= 0) {
            resolved_original_size = wo->original_size;
        }
        if (wo->filled_size > resolved_filled_size) {
            resolved_filled_size = wo->filled_size;
        }
    };

    auto find_record_idx = [&](const auto& rows, bool allow_attribute_fallback) -> int {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& rec = rows[i];
            if (ids_match(rec.exchange_order_id, rec.client_order_id,
                          resolved_exchange_id, resolved_client_id)) {
                return i;
            }
        }

        if (!allow_attribute_fallback || resolved_asset_id.len == 0) return -1;

        int candidate = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& rec = rows[i];
            if (rec.asset_id != resolved_asset_id) continue;
            if (rec.side != resolved_side) continue;
            if (resolved_market_id.len > 0 && rec.market_id.len > 0 &&
                rec.market_id != resolved_market_id) {
                continue;
            }
            if (resolved_price > 0 && rec.price != resolved_price) continue;
            if (resolved_original_size > 0 && rec.original_size != resolved_original_size) {
                continue;
            }

            if (candidate >= 0) return -1;  // ambiguous fallback match
            candidate = i;
        }
        return candidate;
    };

    auto hydrate_from_existing = [&]() {
        auto hydrate_from_row = [&](const auto& rec) {
            if (resolved_exchange_id.len == 0 && rec.exchange_order_id.len > 0) {
                resolved_exchange_id = rec.exchange_order_id;
            }
            if (resolved_client_id.len == 0 && rec.client_order_id.len > 0) {
                resolved_client_id = rec.client_order_id;
            }
            if (resolved_asset_id.len == 0 && rec.asset_id.len > 0) {
                resolved_asset_id = rec.asset_id;
            }
            if (resolved_market_id.len == 0 && rec.market_id.len > 0) {
                resolved_market_id = rec.market_id;
            }
            if (resolved_price == 0) {
                resolved_price = rec.price;
            }
            if (resolved_original_size <= 0) {
                resolved_original_size = rec.original_size;
            }
            if (rec.filled_size > resolved_filled_size) {
                resolved_filled_size = rec.filled_size;
            }
        };

        int idx = find_record_idx(ui_account_working_orders_, false);
        if (idx >= 0) {
            hydrate_from_row(ui_account_working_orders_[idx]);
            return;
        }
        idx = find_record_idx(ui_closed_orders_, false);
        if (idx >= 0) {
            hydrate_from_row(ui_closed_orders_[idx]);
        }
    };

    auto remove_matching_working = [&](const UiOrderRecord& target) {
        for (int i = static_cast<int>(ui_account_working_orders_.size()) - 1; i >= 0; --i) {
            const auto& rec = ui_account_working_orders_[i];
            bool id_match = ids_match(rec.exchange_order_id, rec.client_order_id,
                                      target.exchange_order_id, target.client_order_id);
            bool attr_match = target.asset_id.len > 0 &&
                              rec.asset_id == target.asset_id &&
                              rec.side == target.side &&
                              rec.price == target.price &&
                              rec.original_size == target.original_size;
            if (id_match || attr_match) {
                // O(1) swap-and-pop instead of O(N) erase
                ui_account_working_orders_[i] = ui_account_working_orders_.back();
                ui_account_working_orders_.pop_back();
            }
        }
    };

    auto upsert_closed = [&](UiOrderRecord rec) {
        int closed_idx = -1;
        for (int i = 0; i < static_cast<int>(ui_closed_orders_.size()); ++i) {
            const auto& existing = ui_closed_orders_[i];
            bool id_match = ids_match(existing.exchange_order_id, existing.client_order_id,
                                      rec.exchange_order_id, rec.client_order_id);
            bool attr_match = rec.asset_id.len > 0 &&
                              existing.asset_id == rec.asset_id &&
                              existing.side == rec.side &&
                              existing.price == rec.price &&
                              existing.original_size == rec.original_size;
            if (id_match || attr_match) {
                closed_idx = i;
                break;
            }
        }

        if (closed_idx >= 0) {
            // O(1) swap-and-pop instead of O(N) erase
            ui_closed_orders_[closed_idx] = ui_closed_orders_.back();
            ui_closed_orders_.pop_back();
        }
        ui_closed_orders_.push_back(rec);
        if (ui_closed_orders_.size() > static_cast<std::size_t>(kMaxUiClosedOrders)) {
            ui_closed_orders_.erase(ui_closed_orders_.begin());
        }
    };

    hydrate_from_tracker();
    hydrate_from_existing();

    int working_idx = find_record_idx(ui_account_working_orders_, true);
    int closed_idx = find_record_idx(ui_closed_orders_, true);

    UiOrderRecord rec{};
    if (working_idx >= 0) {
        rec = ui_account_working_orders_[working_idx];
    } else if (closed_idx >= 0) {
        rec = ui_closed_orders_[closed_idx];
    }

    if (resolved_exchange_id.len > 0) rec.exchange_order_id = resolved_exchange_id;
    if (resolved_client_id.len > 0) rec.client_order_id = resolved_client_id;
    if (resolved_asset_id.len > 0) rec.asset_id = resolved_asset_id;
    if (resolved_market_id.len > 0) rec.market_id = resolved_market_id;
    rec.side = resolved_side;
    if (resolved_price > 0 || rec.price == 0) rec.price = resolved_price;
    if (resolved_original_size > 0 || rec.original_size == 0) {
        rec.original_size = resolved_original_size;
    }
    if (resolved_filled_size > rec.filled_size) {
        rec.filled_size = resolved_filled_size;
    }
    rec.is_pending = false;

    auto status = static_cast<OrderStatus>(event.order_status_raw);
    rec.lifecycle_state = classify_lifecycle(status, rec.filled_size);
    rec.is_live = (status == OrderStatus::LIVE || status == OrderStatus::PARTIAL);
    rec.last_update_ts = event.recv_ts;
    rec.update_seq = ++ui_order_update_seq_;

    if (rec.lifecycle_state == static_cast<uint8_t>(UiOrderLifecycleState::WORKING)) {
        // Keep terminal history terminal; ignore stale reopen events.
        if (closed_idx >= 0 && working_idx < 0) {
            return;
        }

        if (working_idx >= 0) {
            ui_account_working_orders_[working_idx] = rec;
        } else {
            remove_matching_working(rec);
            ui_account_working_orders_.push_back(rec);
        }
        return;
    }

    remove_matching_working(rec);
    upsert_closed(rec);
}

void StrategyScheduler::on_user_trade_event_for_ui(const SchedulerEvent& event) {
    UiTradeRecord rec{};
    int existing_idx = -1;
    for (int i = 0; i < static_cast<int>(ui_trade_history_.size()); ++i) {
        if (ui_trade_history_[i].trade_id.len > 0 &&
            ui_trade_history_[i].trade_id == event.trade_id) {
            existing_idx = i;
            rec = ui_trade_history_[i];
            break;
        }
    }

    rec.trade_id = event.trade_id;
    if (event.order_id.len > 0) rec.order_id = event.order_id;
    if (event.asset_id.len > 0) rec.asset_id = event.asset_id;
    if (event.user_market_id.len > 0) rec.market_id = event.user_market_id;
    rec.side = event.user_side;
    rec.price = event.user_price;
    rec.size = event.user_fill_size;
    rec.trade_status = event.trade_status_raw;
    rec.last_update_ts = event.recv_ts;
    rec.update_seq = ++ui_trade_update_seq_;

    if (existing_idx >= 0) {
        // O(1) swap-and-pop instead of O(N) erase
        ui_trade_history_[existing_idx] = ui_trade_history_.back();
        ui_trade_history_.pop_back();
    }
    ui_trade_history_.push_back(rec);
    if (ui_trade_history_.size() > static_cast<std::size_t>(kMaxUiTrades)) {
        ui_trade_history_.erase(ui_trade_history_.begin());
    }
}

void StrategyScheduler::on_exec_feedback_for_ui(const SchedulerEvent& event) {
    OrderId resolved_exchange_id = event.order_id;
    OrderId resolved_client_id = event.client_order_id;

    auto build_from_tracker = [&](UiOrderRecord& rec) {
        if (!working_tracker_) return;

        const WorkingOrder* wo = nullptr;
        if (resolved_client_id.len > 0) {
            wo = working_tracker_->find_by_client_id(resolved_client_id);
        }
        if (!wo && resolved_exchange_id.len > 0) {
            wo = working_tracker_->find_by_exchange_id(resolved_exchange_id);
        }
        if (!wo) return;

        if (resolved_exchange_id.len == 0 && wo->exchange_order_id.len > 0) {
            resolved_exchange_id = wo->exchange_order_id;
        }
        if (resolved_client_id.len == 0 && wo->client_order_id.len > 0) {
            resolved_client_id = wo->client_order_id;
        }

        rec.exchange_order_id = wo->exchange_order_id;
        rec.client_order_id = wo->client_order_id;
        rec.asset_id = wo->asset_id;
        rec.market_id = wo->market_id;
        rec.side = wo->side;
        rec.price = wo->price;
        rec.original_size = wo->original_size;
        rec.filled_size = wo->filled_size;
        rec.is_live = wo->is_live;
        rec.is_pending = wo->is_pending;
    };

    auto find_record_idx = [&](const auto& rows, bool allow_attribute_fallback) -> int {
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& rec = rows[i];
            if (ids_match(rec.exchange_order_id, rec.client_order_id,
                          resolved_exchange_id, resolved_client_id)) {
                return i;
            }
        }

        if (!allow_attribute_fallback) return -1;

        UiOrderRecord tracker_row{};
        build_from_tracker(tracker_row);
        if (tracker_row.asset_id.len == 0) return -1;

        int candidate = -1;
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const auto& rec = rows[i];
            if (rec.asset_id != tracker_row.asset_id) continue;
            if (rec.side != tracker_row.side) continue;
            if (rec.market_id.len > 0 && tracker_row.market_id.len > 0 &&
                rec.market_id != tracker_row.market_id) {
                continue;
            }
            if (rec.price != tracker_row.price) continue;
            if (rec.original_size != tracker_row.original_size) continue;

            if (candidate >= 0) return -1;  // ambiguous fallback match
            candidate = i;
        }
        return candidate;
    };

    auto remove_matching_working = [&](const UiOrderRecord& target) {
        for (int i = static_cast<int>(ui_account_working_orders_.size()) - 1; i >= 0; --i) {
            const auto& rec = ui_account_working_orders_[i];
            bool id_match = ids_match(rec.exchange_order_id, rec.client_order_id,
                                      target.exchange_order_id, target.client_order_id);
            bool attr_match = target.asset_id.len > 0 &&
                              rec.asset_id == target.asset_id &&
                              rec.side == target.side &&
                              rec.price == target.price &&
                              rec.original_size == target.original_size;
            if (id_match || attr_match) {
                // O(1) swap-and-pop instead of O(N) erase
                ui_account_working_orders_[i] = ui_account_working_orders_.back();
                ui_account_working_orders_.pop_back();
            }
        }
    };

    auto upsert_closed = [&](UiOrderRecord rec) {
        int existing_idx = -1;
        for (int i = 0; i < static_cast<int>(ui_closed_orders_.size()); ++i) {
            const auto& existing = ui_closed_orders_[i];
            bool id_match = ids_match(existing.exchange_order_id, existing.client_order_id,
                                      rec.exchange_order_id, rec.client_order_id);
            bool attr_match = rec.asset_id.len > 0 &&
                              existing.asset_id == rec.asset_id &&
                              existing.side == rec.side &&
                              existing.price == rec.price &&
                              existing.original_size == rec.original_size;
            if (id_match || attr_match) {
                existing_idx = i;
                break;
            }
        }
        if (existing_idx >= 0) {
            // O(1) swap-and-pop instead of O(N) erase
            ui_closed_orders_[existing_idx] = ui_closed_orders_.back();
            ui_closed_orders_.pop_back();
        }
        ui_closed_orders_.push_back(rec);
        if (ui_closed_orders_.size() > static_cast<std::size_t>(kMaxUiClosedOrders)) {
            ui_closed_orders_.erase(ui_closed_orders_.begin());
        }
    };

    const auto fb_kind = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);
    const auto now = event.recv_ts > 0 ? event.recv_ts : SteadyClock::now();

    UiOrderRecord tracker_rec{};
    build_from_tracker(tracker_rec);

    int working_idx = find_record_idx(ui_account_working_orders_, true);
    int closed_idx = find_record_idx(ui_closed_orders_, true);

    if (fb_kind == ExecFeedbackKind::ORDER_ACCEPTED) {
        // Keep terminal history terminal; ignore stale reopen feedback.
        if (closed_idx >= 0 && working_idx < 0) {
            return;
        }

        UiOrderRecord rec{};
        if (working_idx >= 0) {
            rec = ui_account_working_orders_[working_idx];
        } else {
            rec = tracker_rec;
        }
        if (resolved_client_id.len > 0) rec.client_order_id = resolved_client_id;
        if (resolved_exchange_id.len > 0) rec.exchange_order_id = resolved_exchange_id;
        rec.lifecycle_state = static_cast<uint8_t>(UiOrderLifecycleState::WORKING);
        rec.is_live = true;
        rec.is_pending = false;
        rec.last_update_ts = now;
        rec.update_seq = ++ui_order_update_seq_;

        if (working_idx >= 0) {
            ui_account_working_orders_[working_idx] = rec;
        } else if (rec.client_order_id.len > 0 || rec.exchange_order_id.len > 0) {
            remove_matching_working(rec);
            ui_account_working_orders_.push_back(rec);
        }
        return;
    }

    if (fb_kind == ExecFeedbackKind::ORDER_REJECTED) {
        UiOrderRecord rec{};
        if (working_idx >= 0) {
            rec = ui_account_working_orders_[working_idx];
        } else {
            rec = tracker_rec;
        }
        if (resolved_client_id.len > 0) rec.client_order_id = resolved_client_id;
        if (resolved_exchange_id.len > 0) rec.exchange_order_id = resolved_exchange_id;
        rec.lifecycle_state = static_cast<uint8_t>(UiOrderLifecycleState::REJECTED);
        rec.is_live = false;
        rec.is_pending = false;
        rec.last_update_ts = now;
        rec.update_seq = ++ui_order_update_seq_;

        remove_matching_working(rec);
        if (rec.client_order_id.len > 0 || rec.exchange_order_id.len > 0 ||
            rec.asset_id.len > 0) {
            upsert_closed(rec);
        }
        return;
    }

    if (fb_kind == ExecFeedbackKind::CANCEL_CONFIRMED) {
        UiOrderRecord rec{};
        if (working_idx >= 0) {
            rec = ui_account_working_orders_[working_idx];
        } else {
            rec = tracker_rec;
        }
        if (resolved_client_id.len > 0) rec.client_order_id = resolved_client_id;
        if (resolved_exchange_id.len > 0) rec.exchange_order_id = resolved_exchange_id;
        rec.lifecycle_state = static_cast<uint8_t>(
            rec.filled_size > 0 ? UiOrderLifecycleState::CANCELED_WITH_FILL
                                : UiOrderLifecycleState::CANCELED_NO_FILL);
        rec.is_live = false;
        rec.is_pending = false;
        rec.last_update_ts = now;
        rec.update_seq = ++ui_order_update_seq_;

        remove_matching_working(rec);
        if (rec.client_order_id.len > 0 || rec.exchange_order_id.len > 0 ||
            rec.asset_id.len > 0) {
            upsert_closed(rec);
        }
    }
}

void StrategyScheduler::dump_stats() {
    char buf[LogEntry::kMaxMsg];
    auto recv_proc_count = metrics_.get(MetricId::SCHED_RECV_TO_PROC_COUNT);
    auto recv_proc_avg =
        recv_proc_count > 0
            ? (metrics_.get(MetricId::SCHED_RECV_TO_PROC_NS) / recv_proc_count)
            : 0;

    std::snprintf(
        buf, sizeof(buf),
        "Sched: cycles=%lld events=%lld(mkt=%lld usr=%lld exec=%lld ctrl=%lld) "
        "triggers=%lld intents=%lld empty=%lld avg_lat=%lld ns max_lat=%lld ns",
        static_cast<long long>(state_.cycle_count()),
        static_cast<long long>(state_.total_events()),
        static_cast<long long>(state_.events_by_source(EventSource::MARKET_WS)),
        static_cast<long long>(state_.events_by_source(EventSource::USER_WS)),
        static_cast<long long>(state_.events_by_source(EventSource::EXEC_INTERNAL)),
        static_cast<long long>(state_.events_by_source(EventSource::CONTROL)),
        static_cast<long long>(state_.trigger_count()),
        static_cast<long long>(exec_sink_.intent_count()),
        static_cast<long long>(empty_polls_),
        static_cast<long long>(recv_proc_avg),
        static_cast<long long>(recv_to_proc_max_ns_));

    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
}

void StrategyScheduler::send_all_cancels(ExecSink* sink) {
    if (!working_tracker_ || !sink) return;
    int canceled = 0;
    for (int i = 0; i < working_tracker_->capacity(); ++i) {
        const auto& wo = working_tracker_->slot(i);
        if (!wo.occupied || wo.is_terminal) continue;
        ExecutionIntent cancel;
        cancel.action = (wo.side == Side::BID)
            ? IntentAction::WOULD_CANCEL_BID : IntentAction::WOULD_CANCEL_ASK;
        cancel.asset_id = wo.asset_id;
        cancel.market_id = wo.market_id;
        cancel.exchange_order_id = wo.exchange_order_id;
        cancel.client_order_id = wo.client_order_id;
        cancel.price = wo.price;
        cancel.qty = wo.original_size - wo.filled_size;
        sink->accept(cancel);
        ++canceled;
    }
    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf), "send_all_cancels: %d cancel intents sent", canceled);
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
}

void StrategyScheduler::send_market_cancels(const AssetId& condition_id, ExecSink* sink) {
    if (!working_tracker_ || !sink) return;
    int canceled = 0;
    for (int i = 0; i < working_tracker_->capacity(); ++i) {
        const auto& wo = working_tracker_->slot(i);
        if (!wo.occupied || wo.is_terminal) continue;
        if (wo.market_id != condition_id) continue;
        ExecutionIntent cancel;
        cancel.action = (wo.side == Side::BID)
            ? IntentAction::WOULD_CANCEL_BID : IntentAction::WOULD_CANCEL_ASK;
        cancel.asset_id = wo.asset_id;
        cancel.market_id = wo.market_id;
        cancel.exchange_order_id = wo.exchange_order_id;
        cancel.client_order_id = wo.client_order_id;
        cancel.price = wo.price;
        cancel.qty = wo.original_size - wo.filled_size;
        sink->accept(cancel);
        ++canceled;
    }
    if (canceled > 0) {
        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf), "send_market_cancels: %d cancel intents sent for market",
                      canceled);
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }
}

void StrategyScheduler::set_rotation_coordinator(RotationCoordinator* coord) {
    rotation_coordinator_ = coord;
    if (coord) {
        rotation_phase_ = &coord->rotation_phase();
    }
}

void StrategyScheduler::set_ui_state_queue(SpscQueue<UiStateSnapshot>* q, int rate_hz) {
    ui_state_queue_ = q;
    if (rate_hz > 0) {
        ui_state_push_interval_ns_ = 1'000'000'000LL / rate_hz;
    }
}

void StrategyScheduler::maybe_push_ui_state() {
    if (!ui_state_queue_) return;

    auto now = SteadyClock::now();
    if (now - last_ui_state_push_ts_ < ui_state_push_interval_ns_) return;
    last_ui_state_push_ts_ = now;

    UiStateSnapshot snap{};
    snap.timestamp = now;

    // Build account-wide working orders from USER_WS mirror, then overlay
    // tracker-only pending intents that are not yet visible via USER_WS.
    // Re-use pre-allocated scratch vector to avoid per-cycle heap allocation.
    auto& working_rows = ui_working_scratch_;
    working_rows.clear();

    for (const auto& rec : ui_account_working_orders_) {
        if (rec.lifecycle_state != static_cast<uint8_t>(UiOrderLifecycleState::WORKING)) {
            continue;
        }
        working_rows.push_back(rec);
    }

    if (working_tracker_) {
        for (int i = 0; i < working_tracker_->capacity(); ++i) {
            const auto& wo = working_tracker_->slot(i);
            if (!wo.occupied || wo.is_terminal) continue;

            bool duplicate = false;
            for (const auto& existing : working_rows) {
                if (ids_match(existing.exchange_order_id, existing.client_order_id,
                              wo.exchange_order_id, wo.client_order_id)) {
                    duplicate = true;
                    break;
                }
                if (existing.exchange_order_id.len == 0 &&
                    existing.client_order_id.len == 0 &&
                    wo.exchange_order_id.len == 0 &&
                    wo.client_order_id.len == 0 &&
                    existing.asset_id == wo.asset_id &&
                    existing.side == wo.side &&
                    existing.price == wo.price &&
                    existing.original_size == wo.original_size &&
                    existing.filled_size == wo.filled_size) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;

            UiOrderRecord rec{};
            rec.exchange_order_id = wo.exchange_order_id;
            rec.client_order_id = wo.client_order_id;
            rec.asset_id = wo.asset_id;
            rec.market_id = wo.market_id;
            rec.side = wo.side;
            rec.price = wo.price;
            rec.original_size = wo.original_size;
            rec.filled_size = wo.filled_size;
            rec.lifecycle_state = static_cast<uint8_t>(UiOrderLifecycleState::WORKING);
            rec.is_live = wo.is_live;
            rec.is_pending = wo.is_pending;
            rec.last_update_ts = wo.sent_ts;
            rec.update_seq = 0;
            working_rows.push_back(rec);
        }
    }

    // Only sort if we have more rows than the snapshot can hold, or if
    // multiple rows exist — otherwise the partial_sort is trivially O(N).
    const int n_rows = static_cast<int>(working_rows.size());
    const int n_keep = std::min(n_rows, static_cast<int>(kMaxUiWorkingOrders));
    if (n_rows > 1) {
        auto cmp = [](const UiOrderRecord& a, const UiOrderRecord& b) {
            if (a.update_seq != b.update_seq) return a.update_seq > b.update_seq;
            if (a.last_update_ts != b.last_update_ts) {
                return a.last_update_ts > b.last_update_ts;
            }
            // Tiebreaker: lexicographic compare on order id with length tiebreaker
            // to satisfy strict weak ordering when len differs (e.g. 0 vs 66).
            int c = std::memcmp(a.exchange_order_id.data, b.exchange_order_id.data,
                                std::min(a.exchange_order_id.len, b.exchange_order_id.len));
            if (c != 0) return c < 0;
            return a.exchange_order_id.len < b.exchange_order_id.len;
        };
        if (n_keep < n_rows) {
            std::partial_sort(working_rows.begin(),
                              working_rows.begin() + n_keep,
                              working_rows.end(), cmp);
        } else {
            std::sort(working_rows.begin(), working_rows.end(), cmp);
        }
    }

    int working_copied = 0;
    for (const auto& rec : working_rows) {
        if (working_copied >= kMaxUiWorkingOrders) break;
        auto& dst = snap.working_orders[working_copied++];
        dst.client_order_id = rec.client_order_id;
        dst.exchange_order_id = rec.exchange_order_id;
        dst.asset_id = rec.asset_id;
        dst.market_id = rec.market_id;
        dst.side = rec.side;
        dst.price = rec.price;
        dst.original_size = rec.original_size;
        dst.filled_size = rec.filled_size;
        dst.is_live = rec.is_live;
        dst.is_pending = rec.is_pending;
        dst.lifecycle_state = rec.lifecycle_state;
        dst.last_update_ts = rec.last_update_ts;
    }
    snap.working_order_count = working_copied;

    int closed_copied = 0;
    for (int i = static_cast<int>(ui_closed_orders_.size()) - 1; i >= 0; --i) {
        if (closed_copied >= kMaxUiClosedOrders) break;
        const auto& rec = ui_closed_orders_[i];
        auto& dst = snap.closed_orders[closed_copied++];
        dst.client_order_id = rec.client_order_id;
        dst.exchange_order_id = rec.exchange_order_id;
        dst.asset_id = rec.asset_id;
        dst.market_id = rec.market_id;
        dst.side = rec.side;
        dst.price = rec.price;
        dst.original_size = rec.original_size;
        dst.filled_size = rec.filled_size;
        dst.lifecycle_state = rec.lifecycle_state;
        dst.last_update_ts = rec.last_update_ts;
    }
    snap.closed_order_count = closed_copied;

    int trade_copied = 0;
    for (int i = static_cast<int>(ui_trade_history_.size()) - 1; i >= 0; --i) {
        if (trade_copied >= kMaxUiTrades) break;
        const auto& rec = ui_trade_history_[i];
        auto& dst = snap.trades[trade_copied++];
        dst.trade_id = rec.trade_id;
        dst.order_id = rec.order_id;
        dst.asset_id = rec.asset_id;
        dst.market_id = rec.market_id;
        dst.side = rec.side;
        dst.price = rec.price;
        dst.size = rec.size;
        dst.trade_status = rec.trade_status;
        dst.last_update_ts = rec.last_update_ts;
    }
    snap.trade_count = trade_copied;

    // Strategy params
    if (strategy_ptr_) {
        snap.strategy_enabled = strategy_ptr_->enabled();
        snap.spread_ticks = strategy_ptr_->spread_ticks();
        snap.quote_size = strategy_ptr_->quote_size();
    }

    // Execution mode
    if (mode_sink_) {
        snap.execution_mode = static_cast<uint8_t>(mode_sink_->mode());
    }

    // Trading session state
    snap.session_state = static_cast<uint8_t>(session_.state);
    snap.session_end_time_s = session_.effective_end_s;
    snap.session_markets_entered = session_.markets_entered;

    // Risk counters
    if (risk_gate_ptr_) {
        snap.risk_checks = risk_gate_ptr_->check_count();
        snap.risk_allows = risk_gate_ptr_->allow_count();
        snap.risk_denies = risk_gate_ptr_->deny_count();
    }

    // Positions (routed through T2 to avoid direct T6->T1 reads)
    if (inventory_view_ && market_pairs_) {
        int pos_idx = 0;
        for (const auto& [cond_id, pair] : market_pairs_->condition_map()) {
            if (pos_idx + 1 >= kMaxUiPositions) break;
            snap.positions[pos_idx] = {pair.token_id_up,
                inventory_view_->position_for(pair.token_id_up)};
            snap.positions[pos_idx + 1] = {pair.token_id_down,
                inventory_view_->position_for(pair.token_id_down)};
            pos_idx += 2;
        }
        snap.position_count = pos_idx;
    }

    if (!ui_state_queue_->try_push(snap)) {
        metrics_.inc(MetricId::UI_STATE_DROPS);
    }
}

// ---------------------------------------------------------------------------
// Latency probe state machine helpers
// ---------------------------------------------------------------------------

void StrategyScheduler::handle_probe_feedback(const SchedulerEvent& event) {
    if (probe_.phase == ProbePhase::PLACING) {
        if (event.client_order_id == probe_.client_order_id) {
            auto fb_kind = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);
            if (fb_kind == ExecFeedbackKind::ORDER_ACCEPTED) {
                probe_.place_ack_ts = SteadyClock::now();
                probe_.order_rtt_ns = probe_.place_ack_ts - probe_.place_sent_ts;
                probe_.exchange_order_id = event.order_id;
                probe_.phase = ProbePhase::WAITING_CANCEL;
                AsyncLogger::log(log_handle_, LogLevel::INFO,
                                 "Probe: order ACK received, scheduling cancel");
            } else if (fb_kind == ExecFeedbackKind::ORDER_REJECTED ||
                       fb_kind == ExecFeedbackKind::RATE_LIMITED ||
                       fb_kind == ExecFeedbackKind::TIMEOUT) {
                probe_.phase = ProbePhase::FAILED;
                metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                AsyncLogger::log(log_handle_, LogLevel::WARN,
                                 "Probe: order rejected/failed");
            }
        }
    } else if (probe_.phase == ProbePhase::CANCELLING) {
        if (event.client_order_id == probe_.client_order_id) {
            auto fb_kind = static_cast<ExecFeedbackKind>(event.exec_feedback_kind);
            if (fb_kind == ExecFeedbackKind::CANCEL_CONFIRMED) {
                probe_.cancel_ack_ts = SteadyClock::now();
                probe_.cancel_rtt_ns = probe_.cancel_ack_ts - probe_.cancel_sent_ts;
                probe_.roundtrip_ns = probe_.cancel_ack_ts - probe_.probe_start_ts;
                probe_.phase = ProbePhase::DONE;

                metrics_.probe_result.order_rtt_ns.store(probe_.order_rtt_ns, std::memory_order_relaxed);
                metrics_.probe_result.cancel_rtt_ns.store(probe_.cancel_rtt_ns, std::memory_order_relaxed);
                metrics_.probe_result.roundtrip_ns.store(probe_.roundtrip_ns, std::memory_order_relaxed);
                metrics_.probe_result.status.store(2, std::memory_order_relaxed);

                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                    "Probe complete: order_rtt=%.1fms cancel_rtt=%.1fms total=%.1fms",
                    probe_.order_rtt_ns / 1e6, probe_.cancel_rtt_ns / 1e6,
                    probe_.roundtrip_ns / 1e6);
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            } else if (fb_kind == ExecFeedbackKind::ORDER_REJECTED ||
                       fb_kind == ExecFeedbackKind::TIMEOUT) {
                probe_.phase = ProbePhase::FAILED;
                metrics_.probe_result.status.store(3, std::memory_order_relaxed);
                AsyncLogger::log(log_handle_, LogLevel::WARN, "Probe: cancel failed");
            }
        }
    }
}

void StrategyScheduler::check_probe_cancel_timer() {
    if (probe_.phase != ProbePhase::WAITING_CANCEL) return;

    auto now = SteadyClock::now();
    constexpr int64_t kProbeCancelDelayNs = 1'000'000'000LL;  // 1 second
    if (now - probe_.place_ack_ts < kProbeCancelDelayNs) return;

    probe_.cancel_intent_id = next_probe_intent_id_++;
    probe_.cancel_sent_ts = SteadyClock::now();
    probe_.phase = ProbePhase::CANCELLING;

    ExecutionIntent cancel;
    cancel.action = IntentAction::WOULD_CANCEL_BID;
    cancel.asset_id = probe_.asset_id;
    cancel.market_id = probe_.market_id;
    cancel.exchange_order_id = probe_.exchange_order_id;
    cancel.client_order_id = probe_.client_order_id;
    cancel.intent_id = probe_.cancel_intent_id;
    cancel.created_ts = SteadyClock::now();

    ExecSink* sink = mode_sink_ ? static_cast<ExecSink*>(mode_sink_)
                                : active_sink_;
    sink->accept(cancel);
    AsyncLogger::log(log_handle_, LogLevel::INFO, "Probe: cancel sent after 1s delay");
}

void StrategyScheduler::check_probe_timeout() {
    if (probe_.phase == ProbePhase::IDLE ||
        probe_.phase == ProbePhase::DONE ||
        probe_.phase == ProbePhase::FAILED) return;

    auto now = SteadyClock::now();
    constexpr int64_t kProbeTimeoutNs = 10'000'000'000LL;  // 10 seconds
    if (now - probe_.probe_start_ts > kProbeTimeoutNs) {
        probe_.phase = ProbePhase::FAILED;
        metrics_.probe_result.status.store(3, std::memory_order_relaxed);
        AsyncLogger::log(log_handle_, LogLevel::WARN, "Probe timed out (10s)");
    }
}

}  // namespace lt
