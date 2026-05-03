#include "ui_bridge/account_poll_service.h"

#include <chrono>
#include <cstdio>
#include <string_view>

#include "common/balance_fetcher.h"
#include "common/discovery.h"
#include "common/token_inventory.h"
#include "simdjson.h"

namespace lt {

AccountPollService::AccountPollService(const std::string& data_api_host,
                                       const std::string& address,
                                       int poll_interval_ms,
                                       const std::string& polygon_rpc_url,
                                       const std::string& balance_address,
                                       int balance_poll_interval_ms,
                                       TokenInventory* token_inventory,
                                       const std::string& eoa_address)
    : data_api_host_(data_api_host),
      address_(address),
      poll_interval_ms_(poll_interval_ms),
      polygon_rpc_url_(polygon_rpc_url),
      balance_address_(balance_address),
      balance_poll_interval_ms_(balance_poll_interval_ms),
      token_inventory_(token_inventory),
      eoa_address_(eoa_address) {}

AccountPollService::~AccountPollService() {
    stop();
}

void AccountPollService::set_on_result(OnResult cb) {
    on_result_ = std::move(cb);
}

void AccountPollService::start() {
    stop_requested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this]() { thread_main(); });
}

void AccountPollService::stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

void AccountPollService::thread_main() {
    using Clock = std::chrono::steady_clock;

    // Fire both polls immediately on first iteration.
    auto last_value_poll = Clock::now() - std::chrono::milliseconds(poll_interval_ms_);
    auto last_balance_poll = Clock::now() - std::chrono::milliseconds(balance_poll_interval_ms_);

    bool balance_enabled = !polygon_rpc_url_.empty() && !balance_address_.empty();

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        auto now = Clock::now();
        bool did_value = false;
        bool did_balance = false;

        AccountPollResult result;

        // Position value poll (every account_poll_interval_ms, e.g. 15s)
        auto value_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_value_poll).count();
        if (value_elapsed >= poll_interval_ms_) {
            result.position_value = poll_position_value();
            result.value_ok = (result.position_value >= 0.0);
            if (!result.value_ok) {
                result.error = "Positions value request failed";
            }
            last_value_poll = now;
            did_value = true;
        }

        // USDC + POL balance poll (every balance_poll_interval_ms, e.g. 1s)
        if (balance_enabled) {
            auto balance_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_balance_poll).count();
            if (balance_elapsed >= balance_poll_interval_ms_) {
                int64_t bal = poll_usdc_balance();
                if (bal >= 0) {
                    result.usdc_balance = bal;
                    result.balance_ok = true;
                    if (token_inventory_) {
                        token_inventory_->set_usdc_balance(bal);
                    }
                }
                double pol = poll_pol_balance();
                if (pol >= 0.0) {
                    result.pol_balance = pol;
                    result.pol_ok = true;
                }
                last_balance_poll = now;
                did_balance = true;
            }
        }

        // Notify callback if either poll ran.
        if ((did_value || did_balance) && on_result_) {
            on_result_(result);
        }

        // Sleep in short intervals to allow quick shutdown.
        // Use the shorter poll interval to determine tick rate.
        int tick_ms = balance_enabled
            ? std::min(balance_poll_interval_ms_, poll_interval_ms_)
            : poll_interval_ms_;
        int sleep_chunk = std::min(250, tick_ms);
        if (sleep_chunk < 50) sleep_chunk = 50;

        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_chunk));
    }
}

int64_t AccountPollService::poll_usdc_balance() {
    auto result = fetch_usdc_balance(polygon_rpc_url_, balance_address_);
    if (result.ok()) {
        return result.value;
    }
    return -1;
}

double AccountPollService::poll_pol_balance() {
    if (eoa_address_.empty() || polygon_rpc_url_.empty()) return -1.0;
    auto result = fetch_pol_balance(polygon_rpc_url_, eoa_address_);
    if (result.ok()) {
        return result.value;
    }
    return -1.0;
}

double AccountPollService::poll_position_value() {
    std::string target = "/value?user=" + address_;
    auto body = sync_https_get(data_api_host_, target, 10000);
    if (body.empty()) return -1.0;

    // Parse: [{"user":"...","value":1234.56}] or {"value":1234.56}
    try {
        simdjson::dom::parser parser;
        auto doc_result = parser.parse(body);
        if (doc_result.error()) {
            std::fprintf(stderr, "account_poll: value parse error\n");
            return -1.0;
        }

        auto doc = doc_result.value();

        // Try array format first
        simdjson::dom::array arr;
        if (!doc.get_array().get(arr)) {
            for (auto elem : arr) {
                double val = 0.0;
                if (!elem["value"].get_double().get(val)) {
                    return val;
                }
                std::string_view sv;
                if (!elem["value"].get_string().get(sv)) {
                    try {
                        return std::stod(std::string(sv));
                    } catch (...) {
                    }
                }
            }
            std::fprintf(stderr, "account_poll: value field missing in array\n");
            return -1.0;
        }

        // Try object format
        double val = 0.0;
        if (!doc["value"].get_double().get(val)) {
            return val;
        }
        std::string_view sv;
        if (!doc["value"].get_string().get(sv)) {
            try {
                return std::stod(std::string(sv));
            } catch (...) {
            }
        }

        std::fprintf(stderr, "account_poll: value field missing\n");
        return -1.0;
    } catch (...) {
        std::fprintf(stderr, "account_poll: value exception\n");
        return -1.0;
    }
}

}  // namespace lt
