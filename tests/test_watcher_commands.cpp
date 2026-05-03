#include "doctest/doctest.h"

#include "ui_bridge/ui_command_parser.h"
#include "ui_bridge/ui_serializer.h"

#include "simdjson.h"

using namespace lt;

TEST_SUITE("WatcherCommands") {

// --- parse_watch_command ---

TEST_CASE("parse watch_subscribe BTC_5m") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_5m"})");
    CHECK(cmd.valid);
    CHECK(cmd.type == WatchCommand::Type::SUBSCRIBE);
    CHECK(cmd.timeframe == BtcTimeframe::BTC_5M);
}

TEST_CASE("parse watch_subscribe BTC_15m") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_15m"})");
    CHECK(cmd.valid);
    CHECK(cmd.type == WatchCommand::Type::SUBSCRIBE);
    CHECK(cmd.timeframe == BtcTimeframe::BTC_15M);
}

TEST_CASE("parse watch_subscribe BTC_1h is invalid") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_1h"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse watch_subscribe BTC_4h is invalid") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_4h"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse watch_unsubscribe") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_unsubscribe","series_key":"BTC_5m"})");
    CHECK(cmd.valid);
    CHECK(cmd.type == WatchCommand::Type::UNSUBSCRIBE);
    CHECK(cmd.timeframe == BtcTimeframe::BTC_5M);
}

TEST_CASE("parse request_series_list") {
    auto cmd = parse_watch_command(R"({"cmd":"request_series_list"})");
    CHECK(cmd.valid);
    CHECK(cmd.type == WatchCommand::Type::REQUEST_LIST);
}

TEST_CASE("parse watch_subscribe invalid series_key") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_2m"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse watch_subscribe missing series_key") {
    auto cmd = parse_watch_command(R"({"cmd":"watch_subscribe"})");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse empty input") {
    auto cmd = parse_watch_command("");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse invalid JSON") {
    auto cmd = parse_watch_command("{bad json}");
    CHECK_FALSE(cmd.valid);
}

TEST_CASE("parse unknown command returns invalid") {
    auto cmd = parse_watch_command(R"({"cmd":"unknown_cmd"})");
    CHECK_FALSE(cmd.valid);
}

// --- is_watch_command ---

TEST_CASE("is_watch_command identifies watcher commands") {
    CHECK(is_watch_command(R"({"cmd":"watch_subscribe","series_key":"BTC_5m"})"));
    CHECK(is_watch_command(R"({"cmd":"watch_unsubscribe","series_key":"BTC_15m"})"));
    CHECK(is_watch_command(R"({"cmd":"request_series_list"})"));
}

TEST_CASE("is_watch_command rejects non-watcher commands") {
    CHECK_FALSE(is_watch_command(R"({"cmd":"enable_strategy"})"));
    CHECK_FALSE(is_watch_command(R"({"cmd":"cancel_all"})"));
    CHECK_FALSE(is_watch_command(R"({"cmd":"set_mode","mode":"DRY_RUN"})"));
    CHECK_FALSE(is_watch_command(""));
    CHECK_FALSE(is_watch_command("{bad}"));
}

// --- Watcher serialization ---

TEST_CASE("serialize_series_list") {
    std::vector<SeriesListEntry> entries;
    entries.push_back({BtcTimeframe::BTC_5M, "cond123", WatcherState::CONNECTED, false});
    entries.push_back({BtcTimeframe::BTC_15M, "cond456", WatcherState::ROLL_PENDING, true});

    auto json = serialize_series_list(entries);

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded).value();

    std::string_view type = doc["type"].get_string().value();
    CHECK(type == "series_list");

    auto series = doc["series"].get_array().value();
    int count = 0;
    for (auto item : series) {
        auto obj = item.get_object().value();
        if (count == 0) {
            CHECK(obj["series_key"].get_string().value() == "BTC_5m");
            CHECK(obj["condition_id"].get_string().value() == "cond123");
            CHECK(obj["status"].get_string().value() == "CONNECTED");
            CHECK(obj["has_next"].get_bool().value() == false);
        } else if (count == 1) {
            CHECK(obj["series_key"].get_string().value() == "BTC_15m");
            CHECK(obj["status"].get_string().value() == "ROLL_PENDING");
            CHECK(obj["has_next"].get_bool().value() == true);
        }
        ++count;
    }
    CHECK(count == 2);
}

TEST_CASE("serialize_watcher_books") {
    WatcherBookStore::MergedLadder ladder;
    ladder.buy_levels = {{5200, 100}, {5100, 50}};
    ladder.sell_levels = {{5300, 75}, {5400, 150}};
    std::vector<WatcherBookLevel> trades = {{5250, 10}};
    TickSize_t tick_size = 100;

    auto json = serialize_watcher_books(BtcTimeframe::BTC_5M, "cond_abc", ladder,
                                         trades, tick_size);

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded).value();

    CHECK(doc["type"].get_string().value() == "watcher_books");
    CHECK(doc["series_key"].get_string().value() == "BTC_5m");
    CHECK(doc["condition_id"].get_string().value() == "cond_abc");

    auto buy = doc["buy_levels"].get_array().value();
    int buy_count = 0;
    for (auto level : buy) {
        auto obj = level.get_object().value();
        if (buy_count == 0) {
            CHECK(obj["price"].get_int64().value() == 5200);
            CHECK(obj["size"].get_int64().value() == 100);
        }
        ++buy_count;
    }
    CHECK(buy_count == 2);

    auto sell = doc["sell_levels"].get_array().value();
    int sell_count = 0;
    for (auto level : sell) {
        auto obj = level.get_object().value();
        if (sell_count == 0) {
            CHECK(obj["price"].get_int64().value() == 5300);
            CHECK(obj["size"].get_int64().value() == 75);
        }
        ++sell_count;
    }
    CHECK(sell_count == 2);

    auto trade_arr = doc["trades"].get_array().value();
    int trade_count = 0;
    for (auto t : trade_arr) {
        auto obj = t.get_object().value();
        if (trade_count == 0) {
            CHECK(obj["price"].get_int64().value() == 5250);
            CHECK(obj["size"].get_int64().value() == 10);
        }
        ++trade_count;
    }
    CHECK(trade_count == 1);

    CHECK(doc["tick_size"].get_int64().value() == 100);
}

TEST_CASE("serialize_watcher_books empty trades") {
    WatcherBookStore::MergedLadder ladder;
    ladder.buy_levels = {{5000, 50}};
    ladder.sell_levels = {{5100, 30}};
    std::vector<WatcherBookLevel> no_trades;

    auto json = serialize_watcher_books(BtcTimeframe::BTC_15M, "cond_x", ladder,
                                         no_trades, 10);

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded).value();

    auto trade_arr = doc["trades"].get_array().value();
    int trade_count = 0;
    for (auto t : trade_arr) { (void)t; ++trade_count; }
    CHECK(trade_count == 0);

    CHECK(doc["tick_size"].get_int64().value() == 10);
}

TEST_CASE("serialize_watcher_status") {
    auto json = serialize_watcher_status(BtcTimeframe::BTC_15M, WatcherState::STALE);

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded).value();

    CHECK(doc["type"].get_string().value() == "watcher_status");
    CHECK(doc["series_key"].get_string().value() == "BTC_15m");
    CHECK(doc["status"].get_string().value() == "STALE");
}

TEST_CASE("serialize_engine_snapshot includes type field") {
    EngineSnapshot snap;
    snap.timestamp_ns = 12345;
    auto json = serialize_engine_snapshot(snap);

    simdjson::padded_string padded(json);
    simdjson::ondemand::parser parser;
    auto doc = parser.iterate(padded).value();

    CHECK(doc["type"].get_string().value() == "engine_snapshot");
    CHECK(doc["timestamp_ns"].get_int64().value() == 12345);
}

}  // TEST_SUITE
