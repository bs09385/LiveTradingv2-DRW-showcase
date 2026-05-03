#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "common/types.h"
#include "events/event_variant.h"
#include "parser/market_message_parser.h"

using namespace lt;

static std::string read_fixture(const char* name) {
    std::string path = FIXTURES_DIR + std::string(name);
    std::ifstream file(path);
    if (!file.is_open()) {
        // Fallback: try relative path
        path = "tests/fixtures/" + std::string(name);
        file.open(path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

TEST_SUITE("MarketMessageParser") {
    TEST_CASE("parse book snapshot") {
        MarketMessageParser parser;
        auto json = read_fixture("book_snapshot.json");
        MarketEvent event;

        auto err = parser.parse(json, 12345, 1, event);
        CHECK(err == ErrorCode::OK);
        CHECK(event.recv_ts == 12345);
        CHECK(event.seq == 1);

        auto* snap = std::get_if<BookSnapshot>(&event.payload);
        REQUIRE(snap != nullptr);
        CHECK(snap->bid_count == 4);
        CHECK(snap->ask_count == 4);
        CHECK(snap->bids[0].price == 5200);  // "0.52"
        CHECK(snap->bids[0].size == qty_from_int(100));
        CHECK(snap->asks[0].price == 5300);  // "0.53"
        CHECK(snap->asks[0].size == qty_from_int(120));
        CHECK(snap->exchange_ts == 1700000000);
    }

    TEST_CASE("parse price_change single") {
        MarketMessageParser parser;
        auto json = read_fixture("price_change_single.json");
        MarketEvent event;

        auto err = parser.parse(json, 12346, 2, event);
        CHECK(err == ErrorCode::OK);

        auto* pce = std::get_if<PriceChangeEvent>(&event.payload);
        REQUIRE(pce != nullptr);
        CHECK(pce->asset_count == 1);
        CHECK(pce->asset_changes[0].change_count == 1);
        CHECK(pce->asset_changes[0].changes[0].price == 5200);
        CHECK(pce->asset_changes[0].changes[0].side == Side::BID);
        CHECK(pce->asset_changes[0].changes[0].size == qty_from_int(150));
        CHECK(pce->asset_changes[0].best_bid == 5200);
        CHECK(pce->asset_changes[0].best_ask == 5300);
    }

    TEST_CASE("parse price_change batch") {
        MarketMessageParser parser;
        auto json = read_fixture("price_change_batch.json");
        MarketEvent event;

        auto err = parser.parse(json, 12347, 3, event);
        CHECK(err == ErrorCode::OK);

        auto* pce = std::get_if<PriceChangeEvent>(&event.payload);
        REQUIRE(pce != nullptr);
        CHECK(pce->asset_count == 2);
        CHECK(pce->asset_changes[0].change_count == 3);
        CHECK(pce->asset_changes[1].change_count == 1);
    }

    TEST_CASE("parse best_bid_ask") {
        MarketMessageParser parser;
        auto json = read_fixture("best_bid_ask.json");
        MarketEvent event;

        auto err = parser.parse(json, 12348, 4, event);
        CHECK(err == ErrorCode::OK);

        auto* bba = std::get_if<BestBidAskEvent>(&event.payload);
        REQUIRE(bba != nullptr);
        CHECK(bba->best_bid == 5200);
        CHECK(bba->best_ask == 5300);
        CHECK(bba->spread == 100);
    }

    TEST_CASE("parse tick_size_change") {
        MarketMessageParser parser;
        auto json = read_fixture("tick_size_change.json");
        MarketEvent event;

        auto err = parser.parse(json, 12349, 5, event);
        CHECK(err == ErrorCode::OK);

        auto* tsc = std::get_if<TickSizeChangeEvent>(&event.payload);
        REQUIRE(tsc != nullptr);
        CHECK(tsc->old_tick_size == 100);   // 0.01
        CHECK(tsc->new_tick_size == 10);    // 0.001
    }

    TEST_CASE("parse last_trade_price") {
        MarketMessageParser parser;
        auto json = read_fixture("last_trade_price.json");
        MarketEvent event;

        auto err = parser.parse(json, 12350, 6, event);
        CHECK(err == ErrorCode::OK);

        auto* ltp = std::get_if<LastTradePriceEvent>(&event.payload);
        REQUIRE(ltp != nullptr);
        CHECK(ltp->price == 5200);
        CHECK(ltp->size == qty_from_int(50));
        CHECK(ltp->side == Side::BID);
    }

    TEST_CASE("PONG message") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse("PONG", 12351, 7, event);
        CHECK(err == ErrorCode::PONG_MESSAGE);
    }

    TEST_CASE("malformed JSON") {
        MarketMessageParser parser;
        auto json = read_fixture("malformed.json");
        MarketEvent event;

        auto err = parser.parse(json, 12352, 8, event);
        // simdjson may partially parse the object but fail on field access
        CHECK(err != ErrorCode::OK);
    }

    TEST_CASE("empty payload") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse("", 12353, 9, event);
        CHECK(err == ErrorCode::EMPTY_INPUT);
    }

    TEST_CASE("missing event_type") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(R"({"asset_id": "test"})", 12354, 10, event);
        CHECK(err == ErrorCode::JSON_MISSING_FIELD);
    }

    TEST_CASE("watcher price_changes without event_type parses via shape inference") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "market":"cond1",
                "timestamp":"1700000001",
                "price_changes":[
                    {"asset_id":"a1","price":"0.52","side":"BUY","size":"10"}
                ]
            })",
            12356, 12, event);
        CHECK(err == ErrorCode::OK);

        auto* pce = std::get_if<PriceChangeEvent>(&event.payload);
        REQUIRE(pce != nullptr);
        REQUIRE(pce->asset_count == 1);
        REQUIRE(pce->asset_changes[0].change_count == 1);
        CHECK(pce->asset_changes[0].asset_id == AssetId("a1"));
        CHECK(pce->asset_changes[0].changes[0].price == 5200);
        CHECK(pce->asset_changes[0].changes[0].side == Side::BID);
        CHECK(pce->asset_changes[0].changes[0].size == qty_from_int(10));
    }

    TEST_CASE("watcher book object without event_type parses via shape inference") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "asset_id":"a1",
                "timestamp":"1700000002",
                "bids":[{"price":"0.52","size":"10"}],
                "asks":[{"price":"0.53","size":"20"}]
            })",
            12357, 13, event);
        CHECK(err == ErrorCode::OK);

        auto* snap = std::get_if<BookSnapshot>(&event.payload);
        REQUIRE(snap != nullptr);
        CHECK(snap->asset_id == AssetId("a1"));
        CHECK(snap->bid_count == 1);
        CHECK(snap->ask_count == 1);
        CHECK(snap->bids[0].price == 5200);
        CHECK(snap->asks[0].price == 5300);
    }

    TEST_CASE("unknown event_type") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(R"({"event_type": "unknown_thing"})", 12355, 11, event);
        CHECK(err == ErrorCode::UNKNOWN_EVENT_TYPE);
    }

    TEST_CASE("parse new_market event") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"new_market",
                "market":"cond123",
                "assets_ids":["token_a","token_b"],
                "outcomes":["Yes","No"],
                "timestamp":"1700000010"
            })",
            100, 20, event);
        CHECK(err == ErrorCode::OK);

        auto* nm = std::get_if<NewMarketEvent>(&event.payload);
        REQUIRE(nm != nullptr);
        CHECK(nm->market_id == AssetId("cond123"));
        CHECK(nm->asset_count == 2);
        CHECK(nm->assets[0] == AssetId("token_a"));
        CHECK(nm->assets[1] == AssetId("token_b"));
        CHECK(nm->outcome_count == 2);
        CHECK(std::string_view(nm->outcomes[0]) == "Yes");
        CHECK(std::string_view(nm->outcomes[1]) == "No");
    }

    TEST_CASE("parse market_resolved event") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"market_resolved",
                "market":"cond456",
                "winning_asset_id":"token_a",
                "winning_outcome":"Yes",
                "assets_ids":["token_a","token_b"],
                "timestamp":"1700000020"
            })",
            200, 30, event);
        CHECK(err == ErrorCode::OK);

        auto* mr = std::get_if<MarketResolvedEvent>(&event.payload);
        REQUIRE(mr != nullptr);
        CHECK(mr->market_id == AssetId("cond456"));
        CHECK(mr->winning_asset_id == AssetId("token_a"));
        CHECK(std::string_view(mr->winning_outcome) == "Yes");
        CHECK(mr->asset_count == 2);
        CHECK(mr->assets[0] == AssetId("token_a"));
        CHECK(mr->assets[1] == AssetId("token_b"));
    }

    TEST_CASE("parse last_trade_price with fee_rate_bps and market") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"last_trade_price",
                "asset_id":"token1",
                "market":"cond789",
                "price":"0.55",
                "size":"25",
                "side":"BUY",
                "fee_rate_bps":"200",
                "timestamp":"1700000030"
            })",
            300, 40, event);
        CHECK(err == ErrorCode::OK);

        auto* ltp = std::get_if<LastTradePriceEvent>(&event.payload);
        REQUIRE(ltp != nullptr);
        CHECK(ltp->market_id == AssetId("cond789"));
        CHECK(ltp->fee_rate_bps == 200);
        CHECK(ltp->price == 5500);
        CHECK(ltp->size == qty_from_int(25));
    }

    TEST_CASE("parse book snapshot with hash and market fields") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"book",
                "asset_id":"token1",
                "market":"cond_abc",
                "hash":"abc123def456",
                "timestamp":"1700000040",
                "bids":[{"price":"0.50","size":"30"}],
                "asks":[{"price":"0.60","size":"40"}]
            })",
            400, 50, event);
        CHECK(err == ErrorCode::OK);

        auto* snap = std::get_if<BookSnapshot>(&event.payload);
        REQUIRE(snap != nullptr);
        CHECK(snap->market_id == AssetId("cond_abc"));
        CHECK(snap->hash_len == 12);
        CHECK(std::string_view(snap->hash, snap->hash_len) == "abc123def456");
        CHECK(snap->bids[0].size == qty_from_int(30));
        CHECK(snap->asks[0].size == qty_from_int(40));
    }

    TEST_CASE("price_change flat format is primary") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"price_change",
                "timestamp":"1700000050",
                "price_changes":[
                    {"asset_id":"token1","price":"0.52","side":"BUY","size":"100","best_bid":"0.52","best_ask":"0.53"}
                ]
            })",
            500, 60, event);
        CHECK(err == ErrorCode::OK);

        auto* pce = std::get_if<PriceChangeEvent>(&event.payload);
        REQUIRE(pce != nullptr);
        CHECK(pce->asset_count == 1);
        CHECK(pce->asset_changes[0].change_count == 1);
        CHECK(pce->asset_changes[0].changes[0].price == 5200);
        CHECK(pce->asset_changes[0].changes[0].size == qty_from_int(100));
    }

    TEST_CASE("best_bid_ask missing required best_ask fails") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"best_bid_ask",
                "asset_id":"a1",
                "best_bid":"0.52"
            })",
            1, 1, event);
        CHECK(err != ErrorCode::OK);
    }

    TEST_CASE("last_trade_price missing side fails") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"last_trade_price",
                "asset_id":"a1",
                "price":"0.52",
                "size":"10"
            })",
            1, 1, event);
        CHECK(err != ErrorCode::OK);
    }

    TEST_CASE("tick_size_change missing new_tick_size fails") {
        MarketMessageParser parser;
        MarketEvent event;

        auto err = parser.parse(
            R"({
                "event_type":"tick_size_change",
                "asset_id":"a1",
                "old_tick_size":"0.01"
            })",
            1, 1, event);
        CHECK(err != ErrorCode::OK);
    }
}
