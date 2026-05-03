#include "slot/market_slot_manager.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace lt {

MarketSlotManager::MarketSlotManager(const SlotManagerConfig& config,
                                     SpscQueue<SchedulerEvent>& slot_queue,
                                     AsyncLogger& logger,
                                     std::atomic<bool>& shutdown_flag,
                                     std::atomic<bool>& fatal_flag)
    : config_(config), slot_queue_(slot_queue), logger_(logger),
      log_handle_(logger.create_producer("SlotManager")),
      shutdown_flag_(shutdown_flag), fatal_flag_(fatal_flag) {

    discovery_host_ = extract_host(config_.discovery_api_url);

    // Initialize slot names
    for (int i = 0; i < kSlotCount; ++i) {
        slots_[i].name = static_cast<SlotName>(i);
    }
}

void MarketSlotManager::request_shutdown() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

bool MarketSlotManager::sleep_ms(int64_t ms) {
    constexpr int64_t kSleepChunk = 50;
    for (int64_t elapsed = 0; elapsed < ms; elapsed += kSleepChunk) {
        if (stop_requested_.load(std::memory_order_relaxed) ||
            shutdown_flag_.load(std::memory_order_relaxed)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kSleepChunk));
    }
    return true;
}

int64_t MarketSlotManager::window_start_for(BtcTimeframe tf, int64_t unix_s) const {
    int64_t period = timeframe_period_seconds(tf);
    return (unix_s / period) * period;
}

int64_t MarketSlotManager::window_end_for(BtcTimeframe tf, int64_t unix_s) const {
    int64_t period = timeframe_period_seconds(tf);
    return (unix_s / period) * period + period;
}

std::optional<DiscoveredMarket> MarketSlotManager::discover_market(
    BtcTimeframe tf, int64_t window_start_s) {

    auto result = discover_btc_market_for_window(discovery_host_, tf, window_start_s);
    if (!result) return std::nullopt;

    // Query neg_risk and fee_rate metadata
    auto neg_risk = query_neg_risk(result->token_id_up);
    if (neg_risk.ok()) {
        result->neg_risk = neg_risk.value;
    }
    auto fee_rate = query_fee_rate(result->token_id_up);
    if (fee_rate.ok()) {
        result->fee_rate_bps = fee_rate.value;
    }

    return result;
}

void MarketSlotManager::register_market_full(SlotManagerCallbacks& cb,
                                              const DiscoveredMarket& mkt) {
    if (cb.register_market) {
        cb.register_market(mkt.condition_id, mkt.token_id_up, mkt.token_id_down,
                           mkt.neg_risk, mkt.fee_rate_bps);
    }
    if (cb.register_token) {
        cb.register_token(mkt.token_id_up);
        cb.register_token(mkt.token_id_down);
    }
    if (cb.seed_market_state) {
        cb.seed_market_state(mkt.token_id_up);
        cb.seed_market_state(mkt.token_id_down);
    }
    if (cb.seed_scheduler_state) {
        cb.seed_scheduler_state(mkt.token_id_up);
        cb.seed_scheduler_state(mkt.token_id_down);
    }
    if (cb.seed_strategy_book) {
        cb.seed_strategy_book(mkt.token_id_up);
        cb.seed_strategy_book(mkt.token_id_down);
    }
}

void MarketSlotManager::push_slot_event(SchedulerEventKind kind, SlotName slot,
                                         const std::string& condition_id) {
    SchedulerEvent ev = SchedulerEvent::make_slot_event(
        kind, static_cast<uint8_t>(slot), AssetId(condition_id));
    if (!slot_queue_.try_push(ev)) {
        AsyncLogger::log(log_handle_, LogLevel::ERROR,
                         "slot_queue overflow — slot event dropped");
    }
}

bool MarketSlotManager::initial_discovery(SlotManagerCallbacks& cb) {
    auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Discover current and next for both 5M and 15M
    struct DiscoveryTarget {
        SlotName slot;
        BtcTimeframe tf;
        int64_t window_start_s;
    };

    int64_t period_5m = timeframe_period_seconds(BtcTimeframe::BTC_5M);
    int64_t period_15m = timeframe_period_seconds(BtcTimeframe::BTC_15M);

    int64_t cur_5m_start = window_start_for(BtcTimeframe::BTC_5M, now_s);
    int64_t next_5m_start = cur_5m_start + period_5m;
    int64_t cur_15m_start = window_start_for(BtcTimeframe::BTC_15M, now_s);
    int64_t next_15m_start = cur_15m_start + period_15m;

    DiscoveryTarget targets_all[] = {
        {SlotName::CURRENT_5M,  BtcTimeframe::BTC_5M,  cur_5m_start},
        {SlotName::NEXT_5M,     BtcTimeframe::BTC_5M,  next_5m_start},
        {SlotName::CURRENT_15M, BtcTimeframe::BTC_15M, cur_15m_start},
        {SlotName::NEXT_15M,    BtcTimeframe::BTC_15M, next_15m_start},
    };

    // Filter to enabled timeframes
    DiscoveryTarget targets[4];
    int target_count = 0;
    for (auto& t : targets_all) {
        if (t.tf == BtcTimeframe::BTC_5M && !config_.enable_5m) continue;
        if (t.tf == BtcTimeframe::BTC_15M && !config_.enable_15m) continue;
        targets[target_count++] = t;
    }

    for (int ti = 0; ti < target_count; ++ti) {
        auto& t = targets[ti];
        if (stop_requested_.load(std::memory_order_relaxed)) return false;

        char buf[256];
        std::snprintf(buf, sizeof(buf), "Discovering %s (window_start=%lld)",
                      slot_name_str(t.slot), static_cast<long long>(t.window_start_s));
        std::fprintf(stderr, "SlotManager: %s\n", buf);
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        int max_retries = 10;
        std::optional<DiscoveredMarket> mkt;
        for (int attempt = 0; attempt < max_retries; ++attempt) {
            mkt = discover_market(t.tf, t.window_start_s);
            if (mkt) break;

            std::snprintf(buf, sizeof(buf), "Discovery retry %d/%d for %s",
                          attempt + 1, max_retries, slot_name_str(t.slot));
            std::fprintf(stderr, "SlotManager: %s\n", buf);
            AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
            if (!sleep_ms(config_.discovery_poll_ms)) return false;
        }

        if (!mkt) {
            std::snprintf(buf, sizeof(buf), "Failed to discover %s after %d retries",
                          slot_name_str(t.slot), max_retries);
            std::fprintf(stderr, "SlotManager: %s\n", buf);
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
            return false;
        }

        // Register the market
        register_market_full(cb, *mkt);

        // Set up slot state
        int idx = static_cast<int>(t.slot);
        slots_[idx].phase = SlotPhase::REGISTERED;
        slots_[idx].timeframe = t.tf;
        slots_[idx].market = *mkt;
        slots_[idx].window_start_unix_s = t.window_start_s;
        slots_[idx].window_end_unix_s = t.window_start_s + timeframe_period_seconds(t.tf);

        std::snprintf(buf, sizeof(buf), "Discovered %s: condition=%s window=[%lld,%lld)",
                      slot_name_str(t.slot), mkt->condition_id.c_str(),
                      static_cast<long long>(slots_[idx].window_start_unix_s),
                      static_cast<long long>(slots_[idx].window_end_unix_s));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }

    return true;
}

void MarketSlotManager::check_rotation(int64_t now_s, BtcTimeframe tf,
                                        SlotManagerCallbacks& cb) {
    SlotName current_name = current_slot_for(tf);
    SlotName next_name = next_slot_for(tf);
    SlotName prev_name = previous_slot_for(tf);
    PreviousMarketState& prev_state = (tf == BtcTimeframe::BTC_5M) ? previous_5m_ : previous_15m_;

    int cur_idx = static_cast<int>(current_name);
    int next_idx = static_cast<int>(next_name);
    int prev_idx = static_cast<int>(prev_name);

    MarketSlot& current = slots_[cur_idx];
    MarketSlot& next = slots_[next_idx];

    // Subscribe NEXT market early if it's REGISTERED and close to window start
    if (next.phase == SlotPhase::REGISTERED) {
        int64_t time_to_start_ms = (next.window_start_unix_s - now_s) * 1000;
        if (time_to_start_ms <= config_.pre_subscribe_ms) {
            // Subscribe to next market tokens
            if (cb.subscribe_market_ws) {
                cb.subscribe_market_ws({next.market.token_id_up, next.market.token_id_down});
            }
            if (cb.subscribe_user_ws) {
                cb.subscribe_user_ws({next.market.condition_id});
            }
            next.phase = SlotPhase::SUBSCRIBING;

            char buf[256];
            std::snprintf(buf, sizeof(buf), "Subscribed NEXT %s (%.1fs before window)",
                          timeframe_name(tf), time_to_start_ms / 1000.0);
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }
    }

    // Promote SUBSCRIBING -> ACTIVE for NEXT slots once their window starts
    // (NEXT becomes CURRENT only on rotation below; this just marks data arrival)
    if (next.phase == SlotPhase::SUBSCRIBING && now_s >= next.window_start_unix_s) {
        next.phase = SlotPhase::ACTIVE;
    }

    // Check if current window has ended
    if (current.phase == SlotPhase::ACTIVE && now_s >= current.window_end_unix_s) {
        char buf[256];

        // 1. Cancel: push SLOT_CLOSING for current
        push_slot_event(SchedulerEventKind::SLOT_CLOSING, current_name,
                        current.market.condition_id);
        current.phase = SlotPhase::CLOSING;
        std::snprintf(buf, sizeof(buf), "SLOT_CLOSING %s cond=%s",
                      slot_name_str(current_name), current.market.condition_id.c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        // 2. Demote: current -> PREVIOUS
        prev_state.market = current.market;
        prev_state.window_start_unix_s = current.window_start_unix_s;
        prev_state.window_end_unix_s = current.window_end_unix_s;
        prev_state.awaiting_resolved = true;
        prev_state.awaiting_redeem = false;

        slots_[prev_idx].phase = SlotPhase::PREVIOUS;
        slots_[prev_idx].market = current.market;
        slots_[prev_idx].timeframe = tf;
        slots_[prev_idx].window_start_unix_s = current.window_start_unix_s;
        slots_[prev_idx].window_end_unix_s = current.window_end_unix_s;

        push_slot_event(SchedulerEventKind::SLOT_DEMOTED, prev_name,
                        current.market.condition_id);

        std::snprintf(buf, sizeof(buf), "SLOT_DEMOTED %s -> %s cond=%s",
                      slot_name_str(current_name), slot_name_str(prev_name),
                      current.market.condition_id.c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        // 3. Promote: next -> current (token_ids stay same, just relabel)
        current.market = next.market;
        current.timeframe = next.timeframe;
        current.window_start_unix_s = next.window_start_unix_s;
        current.window_end_unix_s = next.window_end_unix_s;
        current.phase = SlotPhase::ACTIVE;

        {
            // SLOT_PROMOTED carries window_end for session boundary checking
            SchedulerEvent ev = SchedulerEvent::make_slot_promoted(
                static_cast<uint8_t>(current_name), AssetId(next.market.condition_id),
                next.window_end_unix_s);
            if (!slot_queue_.try_push(ev)) {
                AsyncLogger::log(log_handle_, LogLevel::ERROR,
                                 "slot_queue overflow — slot event dropped");
            }
        }
        push_slot_event(SchedulerEventKind::SLOT_ACTIVATED, current_name,
                        next.market.condition_id);

        std::snprintf(buf, sizeof(buf), "SLOT_PROMOTED %s -> %s cond=%s",
                      slot_name_str(next_name), slot_name_str(current_name),
                      next.market.condition_id.c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        // 4. Clear next slot and begin discovery for the next window
        int64_t new_next_start = next.window_end_unix_s;
        next.clear();
        next.phase = SlotPhase::DISCOVERING;
        next.timeframe = tf;
        next.window_start_unix_s = new_next_start;
        next.window_end_unix_s = new_next_start + timeframe_period_seconds(tf);

        std::snprintf(buf, sizeof(buf), "Begin discovery for new NEXT %s (window_start=%lld)",
                      timeframe_name(tf), static_cast<long long>(new_next_start));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

        // UI update for new current
        if (cb.update_ui_slot) {
            cb.update_ui_slot(current_name, current.market.condition_id,
                              current.window_start_unix_s, current.window_end_unix_s,
                              current.phase);
        }
    }
}

void MarketSlotManager::check_previous_markets(int64_t now_s, SlotManagerCallbacks& cb) {
    auto check_one = [&](PreviousMarketState& prev, SlotName prev_slot_name,
                          std::atomic<bool>& resolved_flag) {
        MarketSlot& slot = slots_[static_cast<int>(prev_slot_name)];
        if (slot.phase != SlotPhase::PREVIOUS) return;

        // Check if market_resolved arrived
        if (prev.awaiting_resolved && resolved_flag.load(std::memory_order_acquire)) {
            prev.awaiting_resolved = false;
            prev.awaiting_redeem = true;
            prev.redeem_fire_time_s = now_s + (config_.redeem_delay_ms / 1000);
            resolved_flag.store(false, std::memory_order_release);

            char buf[256];
            std::snprintf(buf, sizeof(buf), "market_resolved for PREVIOUS %s, redeem in %llds",
                          slot_name_str(prev_slot_name),
                          static_cast<long long>(config_.redeem_delay_ms / 1000));
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }

        // Fire redeem when ready
        if (prev.awaiting_redeem && now_s >= prev.redeem_fire_time_s) {
            if (cb.fire_redeem) {
                cb.fire_redeem(prev.market.condition_id,
                               prev.market.token_id_up,
                               prev.market.token_id_down);
            }

            // Unsubscribe from old market
            if (cb.unsubscribe_market_ws) {
                cb.unsubscribe_market_ws({prev.market.token_id_up, prev.market.token_id_down});
            }
            if (cb.unsubscribe_user_ws) {
                cb.unsubscribe_user_ws({prev.market.condition_id});
            }

            // Clear previous state
            push_slot_event(SchedulerEventKind::SLOT_REMOVED, prev_slot_name,
                            prev.market.condition_id);
            prev.clear();
            slot.clear();

            char buf[128];
            std::snprintf(buf, sizeof(buf), "SLOT_REMOVED %s (redeem fired, unsubscribed)",
                          slot_name_str(prev_slot_name));
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
        }
    };

    check_one(previous_5m_, SlotName::PREVIOUS_5M, resolved_5m_);
    check_one(previous_15m_, SlotName::PREVIOUS_15M, resolved_15m_);
}

void MarketSlotManager::check_pending_discoveries(int64_t now_s, SlotManagerCallbacks& cb) {
    for (int i = 0; i < kSlotCount; ++i) {
        if (slots_[i].phase != SlotPhase::DISCOVERING) continue;
        if (stop_requested_.load(std::memory_order_relaxed)) return;

        auto mkt = discover_market(slots_[i].timeframe, slots_[i].window_start_unix_s);
        if (!mkt) continue;  // will retry on next poll

        // Register the discovered market
        register_market_full(cb, *mkt);

        slots_[i].market = *mkt;
        slots_[i].phase = SlotPhase::REGISTERED;

        char buf[256];
        std::snprintf(buf, sizeof(buf), "Discovered %s: condition=%s window=[%lld,%lld)",
                      slot_name_str(slots_[i].name), mkt->condition_id.c_str(),
                      static_cast<long long>(slots_[i].window_start_unix_s),
                      static_cast<long long>(slots_[i].window_end_unix_s));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }
}

void MarketSlotManager::run(SlotManagerCallbacks cb) {
    AsyncLogger::log(log_handle_, LogLevel::INFO, "MarketSlotManager starting");

    // Step 1: Initial discovery of all 4 active slots
    std::fprintf(stderr, "SlotManager: beginning initial discovery...\n");
    if (!initial_discovery(cb)) {
        std::fprintf(stderr, "SlotManager: initial discovery FAILED\n");
        AsyncLogger::log(log_handle_, LogLevel::ERROR,
                         "Initial discovery failed — slot manager aborting");
        fatal_flag_.store(true, std::memory_order_relaxed);
        return;
    }
    std::fprintf(stderr, "SlotManager: initial discovery complete\n");

    // Bootstrap positions from Data API (before WS starts, tokens already registered)
    if (cb.bootstrap_positions) {
        std::vector<std::string> condition_ids;
        for (int i = 0; i < kSlotCount; ++i) {
            if (slots_[i].phase == SlotPhase::EMPTY ||
                slots_[i].phase == SlotPhase::DISCOVERING) continue;
            condition_ids.push_back(slots_[i].market.condition_id);
        }
        cb.bootstrap_positions(condition_ids);
    }

    // Step 2: Collect all token IDs and condition IDs for initial WS start
    std::vector<std::string> all_token_ids;
    std::vector<std::string> all_condition_ids;
    for (int i = 0; i < kSlotCount; ++i) {
        if (slots_[i].phase == SlotPhase::EMPTY || slots_[i].phase == SlotPhase::DISCOVERING)
            continue;
        all_token_ids.push_back(slots_[i].market.token_id_up);
        all_token_ids.push_back(slots_[i].market.token_id_down);
        all_condition_ids.push_back(slots_[i].market.condition_id);
    }

    // Step 3: Start T0 and T1 with all initial tokens
    if (cb.start_market_ws) {
        cb.start_market_ws(all_token_ids);
    }
    if (cb.start_user_ws) {
        cb.start_user_ws(all_condition_ids);
    }

    // Step 4: Mark current slots as ACTIVE and push events
    for (auto s : {SlotName::CURRENT_5M, SlotName::CURRENT_15M}) {
        if (s == SlotName::CURRENT_5M && !config_.enable_5m) continue;
        if (s == SlotName::CURRENT_15M && !config_.enable_15m) continue;
        int idx = static_cast<int>(s);
        slots_[idx].phase = SlotPhase::ACTIVE;
        push_slot_event(SchedulerEventKind::SLOT_ACTIVATED, s,
                        slots_[idx].market.condition_id);

        if (cb.update_ui_slot) {
            cb.update_ui_slot(s, slots_[idx].market.condition_id,
                              slots_[idx].window_start_unix_s,
                              slots_[idx].window_end_unix_s,
                              slots_[idx].phase);
        }
    }

    // Mark next slots as SUBSCRIBING (data is already flowing from initial WS start)
    for (auto s : {SlotName::NEXT_5M, SlotName::NEXT_15M}) {
        if (s == SlotName::NEXT_5M && !config_.enable_5m) continue;
        if (s == SlotName::NEXT_15M && !config_.enable_15m) continue;
        int idx = static_cast<int>(s);
        slots_[idx].phase = SlotPhase::SUBSCRIBING;
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO,
                     "MarketSlotManager initial setup complete, entering main loop");

    // Step 5: Main loop — 100ms poll
    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed)) {

        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // Independent rotation checks
        if (config_.enable_5m)  check_rotation(now_s, BtcTimeframe::BTC_5M, cb);
        if (config_.enable_15m) check_rotation(now_s, BtcTimeframe::BTC_15M, cb);

        // Background processing for previous markets
        check_previous_markets(now_s, cb);

        // Complete pending discoveries
        check_pending_discoveries(now_s, cb);

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "MarketSlotManager shutting down");
}

}  // namespace lt
