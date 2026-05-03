#include "ui_bridge/ui_command_parser.h"

#include <cstring>
#include <limits>

#include "simdjson.h"
#include "scheduler/execution_mode.h"

namespace lt {

namespace {

uint8_t parse_mode_string(std::string_view mode_str) {
    if (mode_str == "DRY_RUN") return static_cast<uint8_t>(ExecutionMode::DRY_RUN);
    if (mode_str == "LIVE") return static_cast<uint8_t>(ExecutionMode::LIVE);
    return 255;  // invalid
}

bool shares_to_qty(int64_t shares, Qty_t& out) {
    if (shares < 0) return false;
    constexpr int64_t kMaxShares =
        std::numeric_limits<int64_t>::max() / kQtyScale;
    if (shares > kMaxShares) return false;
    out = qty_from_int(shares);
    return true;
}

}  // namespace

UiCommand parse_ui_command(std::string_view json) {
    UiCommand result;

    if (json.empty()) return result;

    // simdjson requires padded input
    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) return result;

    auto doc = std::move(doc_result).value();

    std::string_view cmd;
    auto cmd_result = doc["cmd"].get_string();
    if (cmd_result.error()) return result;
    cmd = cmd_result.value();

    if (cmd == "enable_strategy") {
        result.event = SchedulerEvent::make_enable_strategy(true);
        result.valid = true;
    } else if (cmd == "disable_strategy") {
        result.event = SchedulerEvent::make_enable_strategy(false);
        result.valid = true;
    } else if (cmd == "cancel_all") {
        result.event = SchedulerEvent::make_cancel_all();
        result.valid = true;
    } else if (cmd == "set_mode") {
        std::string_view mode_str;
        auto mode_result = doc["mode"].get_string();
        if (mode_result.error()) return result;
        mode_str = mode_result.value();
        uint8_t mode = parse_mode_string(mode_str);
        if (mode == 255) return result;
        result.event = SchedulerEvent::make_set_mode(mode);
        result.valid = true;
    } else if (cmd == "start_session") {
        int64_t end_time = 0;
        auto end_result = doc["end_time"].get_int64();
        if (end_result.error() == simdjson::SUCCESS) {
            end_time = end_result.value();
        } else if (end_result.error() != simdjson::NO_SUCH_FIELD) {
            return result;
        }
        result.event = SchedulerEvent::make_start_session(end_time);
        result.valid = true;
    } else if (cmd == "stop_session") {
        result.event = SchedulerEvent::make_stop_session();
        result.valid = true;
    } else if (cmd == "inventory_split" || cmd == "inventory_merge") {
        std::string_view condition_sv;
        auto cond_result = doc["condition_id"].get_string();
        if (cond_result.error()) return result;
        condition_sv = cond_result.value();

        int64_t shares = 0;
        auto shares_result = doc["shares"].get_int64();
        if (shares_result.error()) return result;
        shares = shares_result.value();
        Qty_t qty = 0;
        if (!shares_to_qty(shares, qty) || qty <= 0) return result;

        if (cmd == "inventory_split") {
            result.event = SchedulerEvent::make_inventory_split(AssetId(condition_sv), qty);
        } else {
            result.event = SchedulerEvent::make_inventory_merge(AssetId(condition_sv), qty);
        }
        result.valid = true;
    } else if (cmd == "inventory_redeem") {
        std::string_view condition_sv;
        auto cond_result = doc["condition_id"].get_string();
        if (cond_result.error()) return result;
        condition_sv = cond_result.value();

        AssetId token_id;
        auto token_result = doc["token_id"].get_string();
        if (token_result.error() == simdjson::SUCCESS) {
            token_id = AssetId(token_result.value());
        } else if (token_result.error() != simdjson::NO_SUCH_FIELD) {
            return result;
        }

        Qty_t qty = 0;  // default: redeem all available
        auto shares_result = doc["shares"].get_int64();
        if (shares_result.error() == simdjson::SUCCESS) {
            int64_t shares = shares_result.value();
            if (!shares_to_qty(shares, qty)) return result;
        } else if (shares_result.error() != simdjson::NO_SUCH_FIELD) {
            return result;
        }

        result.event = SchedulerEvent::make_inventory_redeem(
            AssetId(condition_sv), token_id, qty);
        result.valid = true;
    } else if (cmd == "latency_probe") {
        result.event = SchedulerEvent::make_latency_probe();
        result.valid = true;
    } else if (cmd == "market_sell_all") {
        result.event = SchedulerEvent::make_market_sell_all();
        result.valid = true;
    } else if (cmd == "market_sell_down") {
        result.event = SchedulerEvent::make_market_sell_down();
        result.valid = true;
    }

    return result;
}

namespace {

bool parse_timeframe(std::string_view key, BtcTimeframe& out) {
    if (key == "BTC_5m")  { out = BtcTimeframe::BTC_5M;  return true; }
    if (key == "BTC_15m") { out = BtcTimeframe::BTC_15M; return true; }
    return false;
}

}  // namespace

WatchCommand parse_watch_command(std::string_view json) {
    WatchCommand result;

    if (json.empty()) return result;

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) return result;

    auto doc = std::move(doc_result).value();

    std::string_view cmd;
    auto cmd_result = doc["cmd"].get_string();
    if (cmd_result.error()) return result;
    cmd = cmd_result.value();

    if (cmd == "watch_subscribe") {
        std::string_view key;
        auto key_result = doc["series_key"].get_string();
        if (key_result.error()) return result;
        key = key_result.value();
        if (!parse_timeframe(key, result.timeframe)) return result;
        result.type = WatchCommand::Type::SUBSCRIBE;
        result.valid = true;
    } else if (cmd == "watch_unsubscribe") {
        std::string_view key;
        auto key_result = doc["series_key"].get_string();
        if (key_result.error()) return result;
        key = key_result.value();
        if (!parse_timeframe(key, result.timeframe)) return result;
        result.type = WatchCommand::Type::UNSUBSCRIBE;
        result.valid = true;
    } else if (cmd == "request_series_list") {
        result.type = WatchCommand::Type::REQUEST_LIST;
        result.valid = true;
    }

    return result;
}

bool is_watch_command(std::string_view json) {
    if (json.empty()) return false;

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc_result = parser.iterate(padded);
    if (doc_result.error()) return false;

    auto doc = std::move(doc_result).value();
    std::string_view cmd;
    auto cmd_result = doc["cmd"].get_string();
    if (cmd_result.error()) return false;
    cmd = cmd_result.value();

    return cmd == "watch_subscribe" ||
           cmd == "watch_unsubscribe" ||
           cmd == "request_series_list";
}

}  // namespace lt
