#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace lt {

class TokenInventory;

struct AccountPollResult {
    double position_value = 0.0;
    bool value_ok = false;
    int64_t usdc_balance = 0;       // micro-USDC from on-chain (for UI display)
    bool balance_ok = false;
    double pol_balance = 0.0;       // POL/MATIC in full units (e.g. 10.0 = 10 POL)
    bool pol_ok = false;
    std::string error;              // human-readable error for UI display
};

class AccountPollService {
public:
    using OnResult = std::function<void(const AccountPollResult&)>;

    AccountPollService(const std::string& data_api_host,
                       const std::string& address,
                       int poll_interval_ms,
                       const std::string& polygon_rpc_url,
                       const std::string& balance_address,
                       int balance_poll_interval_ms,
                       TokenInventory* token_inventory,
                       const std::string& eoa_address = "");
    ~AccountPollService();

    AccountPollService(const AccountPollService&) = delete;
    AccountPollService& operator=(const AccountPollService&) = delete;

    void set_on_result(OnResult cb);
    void start();
    void stop();

private:
    void thread_main();
    double poll_position_value();
    int64_t poll_usdc_balance();
    double poll_pol_balance();

    std::string data_api_host_;
    std::string address_;
    int poll_interval_ms_;

    // On-chain balance polling
    std::string polygon_rpc_url_;
    std::string balance_address_;
    int balance_poll_interval_ms_;
    TokenInventory* token_inventory_;
    std::string eoa_address_;  // EOA/signer address for POL balance

    OnResult on_result_;
    std::thread thread_;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace lt
