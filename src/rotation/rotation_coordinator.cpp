#include "rotation/rotation_coordinator.h"

#include <chrono>
#include <cstdio>
#include <thread>

#include "common/discovery.h"
#include "logger/async_logger.h"

namespace lt {

RotationCoordinator::RotationCoordinator(const RotationConfig& config,
                                         AsyncLogger& logger,
                                         std::atomic<bool>& shutdown_flag,
                                         std::atomic<bool>& fatal_flag)
    : config_(config),
      logger_(logger),
      log_handle_(logger.create_producer("rotation")),
      shutdown_flag_(shutdown_flag),
      fatal_flag_(fatal_flag),
      discovery_host_(extract_host(config.discovery_api_url)) {}

void RotationCoordinator::request_shutdown() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

int64_t RotationCoordinator::window_start(int64_t unix_s) const {
    int64_t period = timeframe_period_seconds(config_.timeframe);
    auto candidates = discovery_window_candidates(config_.timeframe, unix_s);
    if (!candidates.empty()) return candidates.front();
    return (unix_s / period) * period;
}

int64_t RotationCoordinator::window_end(int64_t unix_s) const {
    return window_start(unix_s) + timeframe_period_seconds(config_.timeframe);
}

std::optional<DiscoveredMarket> RotationCoordinator::discover_market(int64_t window_ts) {
    auto result = discover_btc_market_for_window(
        discovery_host_, config_.timeframe, window_ts, 10000);
    if (result && !result->token_id_up.empty()) {
        // Retry neg_risk and fee_rate queries up to 3 times each.
        // Wrong negRisk = wrong domain separator = all orders rejected.
        // Wrong fee_rate = wrong amounts = orders rejected or mispriced.
        constexpr int kMetadataRetries = 3;

        bool got_neg_risk = false;
        for (int i = 0; i < kMetadataRetries; ++i) {
            auto nr = query_neg_risk(result->token_id_up);
            if (nr.ok()) {
                result->neg_risk = nr.value;
                got_neg_risk = true;
                break;
            }
            if (i + 1 < kMetadataRetries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        if (!got_neg_risk) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "Rotation: query_neg_risk failed after %d retries for token=%.40s, "
                "defaulting to false", kMetadataRetries, result->token_id_up.c_str());
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
        }

        bool got_fee_rate = false;
        for (int i = 0; i < kMetadataRetries; ++i) {
            auto fr = query_fee_rate(result->token_id_up);
            if (fr.ok()) {
                result->fee_rate_bps = fr.value;
                got_fee_rate = true;
                break;
            }
            if (i + 1 < kMetadataRetries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        if (!got_fee_rate) {
            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                "Rotation: query_fee_rate failed after %d retries for token=%.40s, "
                "defaulting to 0", kMetadataRetries, result->token_id_up.c_str());
            AsyncLogger::log(log_handle_, LogLevel::ERROR, buf);
        }
    }
    return result;
}

void RotationCoordinator::register_market_full(RotationCallbacks& cb,
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
        cb.seed_scheduler_state(mkt.condition_id);
    }
}

bool RotationCoordinator::sleep_ms(int64_t ms) {
    constexpr int64_t kSleepChunkMs = 100;
    int64_t remaining = ms;
    while (remaining > 0 && !stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed) &&
           !fatal_flag_.load(std::memory_order_relaxed)) {
        auto chunk = std::min(remaining, kSleepChunkMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(chunk));
        remaining -= chunk;
    }
    return !stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed) &&
           !fatal_flag_.load(std::memory_order_relaxed);
}

void RotationCoordinator::execute_rotation(RotationCallbacks& cb,
                                            const DiscoveredMarket& new_next) {
    char buf[LogEntry::kMaxMsg];
    std::snprintf(buf, sizeof(buf), "Rotation #%d: initiating pause protocol",
                  rotation_count_ + 1);
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

    // Step 1: Request T2 to pause
    rotation_phase_.store(static_cast<int>(RotationPhase::PAUSE_REQUESTED),
                          std::memory_order_release);

    // Step 2: Wait for T2 to acknowledge pause
    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed)) {
        auto phase = rotation_phase_.load(std::memory_order_acquire);
        if (phase == static_cast<int>(RotationPhase::PAUSED)) break;
        std::this_thread::yield();
    }
    if (stop_requested_.load(std::memory_order_relaxed) ||
        shutdown_flag_.load(std::memory_order_relaxed)) return;

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Rotation: T2 paused, stopping T0/T1");

    // Step 3: Stop T0 (market WS) and T1 (user WS)
    if (cb.stop_market_ws) cb.stop_market_ws();
    if (cb.stop_user_ws) cb.stop_user_ws();

    // Step 4: Wait for T3 to drain cancel intents
    {
        constexpr int64_t kDrainTimeoutMs = 3000;
        constexpr int64_t kDrainPollMs = 10;
        int64_t waited = 0;
        while (waited < kDrainTimeoutMs) {
            if (cb.is_exec_queue_drained && cb.is_exec_queue_drained()) break;
            if (!sleep_ms(kDrainPollMs)) return;
            waited += kDrainPollMs;
        }
    }

    // Step 5: Register the new next market (safe: T0/T1 stopped, T2 paused, T3 idle)
    if (!new_next.condition_id.empty()) {
        register_market_full(cb, new_next);
        std::snprintf(buf, sizeof(buf),
                      "Rotation: registered new next market cond=%.40s...",
                      new_next.condition_id.c_str());
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }

    // Step 5.5: Record old market for delayed redeem (before swap)
    if (!current_market_.condition_id.empty()) {
        PendingRedeem pr;
        pr.condition_id = current_market_.condition_id;
        pr.token_id_up = current_market_.token_id_up;
        pr.token_id_down = current_market_.token_id_down;
        pr.fire_at = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(kRedeemDelayMs);
        pending_redeems_.push_back(std::move(pr));

        std::snprintf(buf, sizeof(buf),
                      "Rotation: queued redeem for old market cond=%.40s... (delay %llds)",
                      current_market_.condition_id.c_str(),
                      static_cast<long long>(kRedeemDelayMs / 1000));
        AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
    }

    // Step 6: Swap current <- next (the market we're switching to was pre-registered)
    current_market_ = *next_market_;
    next_market_ = new_next.condition_id.empty() ? std::nullopt
                                                  : std::optional<DiscoveredMarket>(new_next);

    // Step 7: Build token lists for new subscriptions
    std::vector<std::string> token_ids;
    token_ids.push_back(current_market_.token_id_up);
    token_ids.push_back(current_market_.token_id_down);
    if (next_market_) {
        token_ids.push_back(next_market_->token_id_up);
        token_ids.push_back(next_market_->token_id_down);
    }

    std::vector<std::string> condition_ids;
    condition_ids.push_back(current_market_.condition_id);
    if (next_market_) {
        condition_ids.push_back(next_market_->condition_id);
    }

    // Step 8: Start new T0 and T1 with new subscriptions
    if (cb.start_market_ws) cb.start_market_ws(token_ids);
    if (cb.start_user_ws) cb.start_user_ws(condition_ids);

    // Step 9: Update timing context for new window
    auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    timing_context_.window_start_unix_s = window_start(now_s);
    timing_context_.window_end_unix_s = window_end(now_s);

    ++rotation_count_;

    std::snprintf(buf, sizeof(buf),
                  "Rotation #%d complete: cond=%.40s... window=[%lld, %lld]",
                  rotation_count_, current_market_.condition_id.c_str(),
                  static_cast<long long>(timing_context_.window_start_unix_s),
                  static_cast<long long>(timing_context_.window_end_unix_s));
    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

    // Step 10: Request T2 to resume
    rotation_phase_.store(static_cast<int>(RotationPhase::RESUME_REQUESTED),
                          std::memory_order_release);

    // Wait for T2 to acknowledge resume
    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed)) {
        auto phase = rotation_phase_.load(std::memory_order_acquire);
        if (phase == static_cast<int>(RotationPhase::NORMAL)) break;
        std::this_thread::yield();
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Rotation: T2 resumed, normal operation");

    // Push updated rotation info to UI
    if (cb.update_ui_rotation) {
        cb.update_ui_rotation(current_market_.condition_id,
                              timing_context_.window_start_unix_s,
                              timing_context_.window_end_unix_s,
                              rotation_count_, false);
    }
}

void RotationCoordinator::run(RotationCallbacks cb) {
    AsyncLogger::log(log_handle_, LogLevel::INFO, "RotationCoordinator starting");

    if (discovery_host_.empty()) {
        AsyncLogger::log(log_handle_, LogLevel::ERROR,
                         "Rotation: empty discovery host, cannot start");
        return;
    }

    // --- Phase A: Initial discovery ---
    // Retry initial discovery up to 10 times (1s between retries)
    int discovery_attempts = 0;
    constexpr int kMaxDiscoveryAttempts = 10;

    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed)) {
        auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t current_window = window_start(now_s);

        auto market = discover_market(current_window);
        if (market && !market->is_closed) {
            current_market_ = *market;

            char buf[LogEntry::kMaxMsg];
            std::snprintf(buf, sizeof(buf),
                          "Rotation: discovered current market cond=%.40s... up=%.20s... down=%.20s...",
                          current_market_.condition_id.c_str(),
                          current_market_.token_id_up.c_str(),
                          current_market_.token_id_down.c_str());
            AsyncLogger::log(log_handle_, LogLevel::INFO, buf);

            // Try to discover next market too
            int64_t next_window = current_window + timeframe_period_seconds(config_.timeframe);
            auto next = discover_market(next_window);
            if (next) {
                next_market_ = *next;
                std::snprintf(buf, sizeof(buf),
                              "Rotation: discovered next market cond=%.40s...",
                              next->condition_id.c_str());
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
            }
            break;
        } else {
            // Current window closed — try next window
            int64_t next_window = current_window + timeframe_period_seconds(config_.timeframe);
            auto next = discover_market(next_window);
            if (next && !next->is_closed) {
                current_market_ = *next;
                char buf[LogEntry::kMaxMsg];
                std::snprintf(buf, sizeof(buf),
                              "Rotation: current window closed, using next: cond=%.40s...",
                              current_market_.condition_id.c_str());
                AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                break;
            }
        }

        ++discovery_attempts;
        if (discovery_attempts >= kMaxDiscoveryAttempts) {
            AsyncLogger::log(log_handle_, LogLevel::ERROR,
                             "Rotation: failed to discover initial market after max retries");
            return;
        }

        char buf[LogEntry::kMaxMsg];
        std::snprintf(buf, sizeof(buf),
                      "Rotation: discovery attempt %d/%d failed, retrying...",
                      discovery_attempts, kMaxDiscoveryAttempts);
        AsyncLogger::log(log_handle_, LogLevel::WARN, buf);
        if (!sleep_ms(1000)) return;
    }

    if (stop_requested_.load(std::memory_order_relaxed) ||
        shutdown_flag_.load(std::memory_order_relaxed)) return;

    // --- Phase B: Register current + next markets ---
    register_market_full(cb, current_market_);
    if (next_market_) {
        register_market_full(cb, *next_market_);
    }

    // Bootstrap positions from Data API (before WS starts, tokens already registered)
    if (cb.bootstrap_positions) {
        std::vector<std::string> condition_ids;
        condition_ids.push_back(current_market_.condition_id);
        if (next_market_) condition_ids.push_back(next_market_->condition_id);
        cb.bootstrap_positions(condition_ids);
    }

    // --- Phase C: Set initial timing context ---
    auto now_s = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    timing_context_.window_start_unix_s = window_start(now_s);
    timing_context_.window_end_unix_s = window_end(now_s);
    timing_context_.period_s = timeframe_period_seconds(config_.timeframe);
    timing_context_.no_trade_start_ms = config_.no_trade_start_ms;
    timing_context_.no_trade_end_ms = config_.no_trade_end_ms;
    timing_context_.enabled = true;

    // --- Phase D: Start T0/T1 with initial subscriptions ---
    std::vector<std::string> token_ids;
    token_ids.push_back(current_market_.token_id_up);
    token_ids.push_back(current_market_.token_id_down);
    if (next_market_) {
        token_ids.push_back(next_market_->token_id_up);
        token_ids.push_back(next_market_->token_id_down);
    }

    std::vector<std::string> condition_ids;
    condition_ids.push_back(current_market_.condition_id);
    if (next_market_) {
        condition_ids.push_back(next_market_->condition_id);
    }

    if (cb.start_market_ws) cb.start_market_ws(token_ids);
    if (cb.start_user_ws) cb.start_user_ws(condition_ids);

    // Push initial rotation info to UI
    if (cb.update_ui_rotation) {
        cb.update_ui_rotation(current_market_.condition_id,
                              timing_context_.window_start_unix_s,
                              timing_context_.window_end_unix_s,
                              rotation_count_, false);
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "Rotation: T0/T1 started, entering main loop");

    // --- Phase E: Main rotation loop ---
    auto last_discovery_poll = std::chrono::steady_clock::now();

    while (!stop_requested_.load(std::memory_order_relaxed) &&
           !shutdown_flag_.load(std::memory_order_relaxed) &&
           !fatal_flag_.load(std::memory_order_relaxed)) {

        if (!sleep_ms(100)) break;

        auto now_sys = std::chrono::system_clock::now();
        auto unix_s = std::chrono::duration_cast<std::chrono::seconds>(
            now_sys.time_since_epoch()).count();

        // Check if we need to rotate: current time is within pre_rotation_ms of window end
        int64_t time_to_end_ms = (timing_context_.window_end_unix_s - unix_s) * 1000;
        if (time_to_end_ms <= config_.pre_rotation_ms && next_market_) {
            // Use pre-cached discovery instead of blocking HTTPS during rotation
            DiscoveredMarket new_next_mkt;
            if (pre_discovered_next_) {
                new_next_mkt = *pre_discovered_next_;
                pre_discovered_next_.reset();
            }
            // Fallback: if cache miss, try discovery (may block)
            if (new_next_mkt.condition_id.empty()) {
                int64_t new_next_window = timing_context_.window_end_unix_s +
                                           timeframe_period_seconds(config_.timeframe);
                auto new_next = discover_market(new_next_window);
                if (new_next) {
                    new_next_mkt = *new_next;
                }
            }

            execute_rotation(cb, new_next_mkt);

            // Reset discovery poll timer after rotation
            last_discovery_poll = std::chrono::steady_clock::now();
            continue;
        }

        // Check for matured pending redeems
        if (!pending_redeems_.empty() && cb.fire_redeem) {
            auto now_steady = std::chrono::steady_clock::now();
            auto it = pending_redeems_.begin();
            while (it != pending_redeems_.end()) {
                if (now_steady >= it->fire_at) {
                    char redeem_msg[LogEntry::kMaxMsg];
                    std::snprintf(redeem_msg, sizeof(redeem_msg),
                                  "Rotation: firing redeem for old market cond=%.40s...",
                                  it->condition_id.c_str());
                    AsyncLogger::log(log_handle_, LogLevel::INFO, redeem_msg);

                    cb.fire_redeem(it->condition_id, it->token_id_up, it->token_id_down);
                    it = pending_redeems_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Periodic discovery poll for the next market
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - last_discovery_poll).count();
        if (elapsed >= config_.discovery_poll_ms) {
            last_discovery_poll = std::chrono::steady_clock::now();

            if (!next_market_) {
                // Try to discover next market
                int64_t next_window = timing_context_.window_end_unix_s;
                auto next = discover_market(next_window);
                if (next && next->condition_id != current_market_.condition_id) {
                    next_market_ = *next;
                    // Pre-register the next market
                    register_market_full(cb, *next);

                    char buf[LogEntry::kMaxMsg];
                    std::snprintf(buf, sizeof(buf),
                                  "Rotation: discovered next market cond=%.40s...",
                                  next->condition_id.c_str());
                    AsyncLogger::log(log_handle_, LogLevel::INFO, buf);
                }
            } else {
                // Pre-discover market two windows ahead for next rotation
                int64_t two_ahead_window = timing_context_.window_end_unix_s +
                                           timeframe_period_seconds(config_.timeframe);
                auto two_ahead = discover_market(two_ahead_window);
                if (two_ahead) {
                    pre_discovered_next_ = *two_ahead;
                }
            }
        }
    }

    // --- Phase F: Shutdown ---
    // Request T2 to pause cleanly before returning
    if (rotation_phase_.load(std::memory_order_relaxed) ==
        static_cast<int>(RotationPhase::NORMAL)) {
        rotation_phase_.store(static_cast<int>(RotationPhase::PAUSE_REQUESTED),
                              std::memory_order_release);
        // Brief wait for T2 to pause — don't block forever
        for (int i = 0; i < 100; ++i) {
            if (rotation_phase_.load(std::memory_order_acquire) ==
                static_cast<int>(RotationPhase::PAUSED)) {
                // Resume T2 so it can exit its run loop
                rotation_phase_.store(static_cast<int>(RotationPhase::RESUME_REQUESTED),
                                      std::memory_order_release);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    AsyncLogger::log(log_handle_, LogLevel::INFO, "RotationCoordinator stopped");
}

}  // namespace lt
