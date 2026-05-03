#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "common/types.h"
#include "events/event_variant.h"
#include "logger/async_logger.h"
#include "ui_bridge/watcher_types.h"

namespace lt {

class AsyncLogger;

// Runs on T_watch thread: manages a MarketWsClient for watcher data
// and periodic REST discovery polling. Posts parsed results to T6's io_context.
//
// Thread safety: start()/stop() called from T6's thread.
// update_subscriptions() called from T6, passed to T_watch via atomic flag + mutex.
// Callbacks fire on T_watch, should post() to T6's io_context.
class WatcherService {
public:
    // Callback types (fire on T_watch thread — caller should post to bridge io_context)
    using OnBookData = std::function<void(const MarketEvent&)>;
    using OnWsConnected = std::function<void()>;
    using OnWsDisconnected = std::function<void(const std::string& reason)>;
    using OnDiscovery = std::function<void(BtcTimeframe tf,
                                           SeriesMarketInfo current,
                                           std::optional<SeriesMarketInfo> next)>;

    WatcherService(AsyncLogger& logger, const EngineConfig& config);
    ~WatcherService();

    // Non-copyable
    WatcherService(const WatcherService&) = delete;
    WatcherService& operator=(const WatcherService&) = delete;

    // Set callbacks before start().
    void set_on_book_data(OnBookData cb);
    void set_on_ws_connected(OnWsConnected cb);
    void set_on_ws_disconnected(OnWsDisconnected cb);
    void set_on_discovery(OnDiscovery cb);

    // Start the T_watch thread.
    void start();

    // Stop the T_watch thread (blocks until joined).
    void stop();

    // Called from T6: update the set of asset_ids the WS should subscribe to.
    // The WS client will reconnect with the new set.
    void update_subscriptions(std::vector<std::string> asset_ids);

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    void thread_main();
    void run_ws_client(const std::vector<std::string>& asset_ids);
    void do_discovery_poll();

    AsyncLogger& logger_;
    ProducerHandle log_handle_;
    EngineConfig config_;

    OnBookData on_book_data_;
    OnWsConnected on_ws_connected_;
    OnWsDisconnected on_ws_disconnected_;
    OnDiscovery on_discovery_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Subscription updates from T6
    std::mutex sub_mutex_;
    std::vector<std::string> pending_asset_ids_;
    std::atomic<bool> sub_changed_{false};
};

}  // namespace lt
