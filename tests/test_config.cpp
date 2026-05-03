#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>

#include "common/config.h"

using namespace lt;

TEST_SUITE("Config") {
    TEST_CASE("load valid config") {
        // Write a temp config file
        const char* path = "test_config_temp.json";
        {
            std::ofstream f(path);
            f << R"({
                "ws_endpoint": "wss://example.com/ws",
                "asset_ids": ["id1", "id2"],
                "ping_interval_ms": 5000,
                "strategy_queue_capacity": 32768,
                "log_level": "DEBUG"
            })";
        }

        auto result = load_config(path);
        CHECK(result.ok());
        CHECK(result.value.ws_endpoint == "wss://example.com/ws");
        CHECK(result.value.asset_ids.size() == 2);
        CHECK(result.value.asset_ids[0] == "id1");
        CHECK(result.value.asset_ids[1] == "id2");
        CHECK(result.value.ping_interval_ms == 5000);
        CHECK(result.value.strategy_queue_capacity == 32768);
        CHECK(result.value.log_level == "DEBUG");

        // Defaults should remain
        CHECK(result.value.pong_timeout_ms == 5000);
        CHECK(result.value.reconnect_base_ms == 1000);

        std::remove(path);
    }

    TEST_CASE("missing file returns defaults-friendly error") {
        auto result = load_config("nonexistent_file.json");
        CHECK(!result.ok());
        CHECK(result.error == ErrorCode::CONFIG_FILE_NOT_FOUND);
    }

    TEST_CASE("partial config uses defaults for missing fields") {
        const char* path = "test_config_partial.json";
        {
            std::ofstream f(path);
            f << R"({"log_level": "TRACE"})";
        }

        auto result = load_config(path);
        CHECK(result.ok());
        CHECK(result.value.log_level == "TRACE");
        // Everything else is default
        CHECK(result.value.ws_endpoint ==
              "wss://ws-subscriptions-clob.polymarket.com/ws/market");
        CHECK(result.value.strategy_queue_capacity == 65536);

        std::remove(path);
    }

    TEST_CASE("malformed JSON returns error") {
        const char* path = "test_config_bad.json";
        {
            std::ofstream f(path);
            f << "{not valid json";
        }

        auto result = load_config(path);
        CHECK(!result.ok());
        CHECK(result.error == ErrorCode::CONFIG_PARSE_ERROR);

        std::remove(path);
    }
}
