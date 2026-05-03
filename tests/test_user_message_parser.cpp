#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "common/types.h"
#include "events/user_events.h"
#include "parser/user_message_parser.h"

using namespace lt;

static std::string read_fixture(const char* name) {
    std::string path = FIXTURES_DIR + std::string(name);
    std::ifstream file(path);
    if (!file.is_open()) {
        path = "tests/fixtures/" + std::string(name);
        file.open(path);
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

TEST_SUITE("UserMessageParser") {
    TEST_CASE("parse order PLACEMENT") {
        UserMessageParser parser;
        auto json = read_fixture("user_order_placement.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 50000, 1, event);
        CHECK(err == ErrorCode::OK);
        CHECK(event.recv_ts == 50000);
        CHECK(event.seq == 1);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->event_type == OrderEventType::PLACEMENT);
        CHECK(upd->side == Side::ASK);  // SELL maps to ASK
        CHECK(upd->price == 5500);  // "0.55" -> 5500
        CHECK(upd->original_size == qty_from_int(100));
        CHECK(upd->size_matched == qty_from_int(0));
        CHECK(upd->exchange_ts == 1700000000);
        CHECK(upd->order_id.len > 0);
        CHECK(upd->asset_id.len > 0);
        CHECK(upd->market_id.len > 0);
    }

    TEST_CASE("parse order UPDATE (partial fill)") {
        UserMessageParser parser;
        auto json = read_fixture("user_order_update.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 50001, 2, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->event_type == OrderEventType::UPDATE);
        CHECK(upd->size_matched == qty_from_int(30));
        CHECK(upd->original_size == qty_from_int(100));
        CHECK(upd->exchange_ts == 1700000001);
    }

    TEST_CASE("parse order UPDATE (fully filled)") {
        UserMessageParser parser;
        auto json = read_fixture("user_order_filled.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 50002, 3, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->event_type == OrderEventType::UPDATE);
        CHECK(upd->size_matched == qty_from_int(100));
        CHECK(upd->original_size == qty_from_int(100));
    }

    TEST_CASE("parse order CANCELLATION") {
        UserMessageParser parser;
        auto json = read_fixture("user_order_cancellation.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 50003, 4, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->event_type == OrderEventType::CANCELLATION);
        CHECK(upd->size_matched == qty_from_int(30));
    }

    TEST_CASE("parse trade MATCHED") {
        UserMessageParser parser;
        auto json = read_fixture("user_trade_matched.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 60000, 5, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->status == TradeStatus::MATCHED);
        CHECK(upd->side == Side::BID);  // BUY maps to BID
        CHECK(upd->fill_price == 5500);
        CHECK(upd->fill_size == qty_from_int(50));
        CHECK(upd->match_ts == 1700000010);
        CHECK(upd->last_update_ts == 1700000010);
        CHECK(upd->trade_id.len > 0);
        CHECK(upd->taker_order_id.len > 0);
        CHECK(upd->maker_entry_count == 0);  // no maker_orders in this fixture
    }

    TEST_CASE("parse trade with maker_orders") {
        UserMessageParser parser;
        auto json = read_fixture("user_trade_matched_with_maker_orders.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 60010, 10, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->status == TradeStatus::MATCHED);
        CHECK(upd->side == Side::ASK);  // SELL maps to ASK
        CHECK(upd->fill_size == qty_from_int(150));  // top-level size = taker total
        CHECK(upd->maker_entry_count == 2);

        // First maker entry
        CHECK(upd->maker_entries[0].matched_amount == qty_from_int(50));
        CHECK(upd->maker_entries[0].order_id.len > 0);

        // Second maker entry
        CHECK(upd->maker_entries[1].matched_amount == qty_from_int(100));
        CHECK(upd->maker_entries[1].order_id.len > 0);
    }

    TEST_CASE("parse trade CONFIRMED") {
        UserMessageParser parser;
        auto json = read_fixture("user_trade_confirmed.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 60001, 6, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->status == TradeStatus::CONFIRMED);
        CHECK(upd->last_update_ts == 1700000020);
    }

    TEST_CASE("parse trade FAILED") {
        UserMessageParser parser;
        auto json = read_fixture("user_trade_failed.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 60002, 7, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->status == TradeStatus::FAILED);
        CHECK(upd->side == Side::ASK);  // SELL maps to ASK
        CHECK(upd->fill_price == 4500);  // "0.45"
        CHECK(upd->fill_size == qty_from_int(25));
    }

    TEST_CASE("PONG message") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse("PONG", 70000, 8, event);
        CHECK(err == ErrorCode::PONG_MESSAGE);
    }

    TEST_CASE("empty payload") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse("", 70001, 9, event);
        CHECK(err == ErrorCode::EMPTY_INPUT);
    }

    TEST_CASE("malformed JSON") {
        UserMessageParser parser;
        auto json = read_fixture("user_malformed.json");
        UserMessageEvent event;

        auto err = parser.parse(json, 70002, 10, event);
        CHECK(err != ErrorCode::OK);
    }

    TEST_CASE("missing event_type") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"id": "test"})", 70003, 11, event);
        CHECK(err == ErrorCode::JSON_MISSING_FIELD);
    }

    TEST_CASE("unknown event_type") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"event_type": "unknown_thing"})", 70004, 12, event);
        CHECK(err == ErrorCode::UNKNOWN_EVENT_TYPE);
    }

    TEST_CASE("order missing required id field") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::JSON_MISSING_FIELD);
    }

    TEST_CASE("order missing required price field") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::JSON_MISSING_FIELD);
    }

    TEST_CASE("order unknown type value") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"UNKNOWN_TYPE","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::USER_UNKNOWN_EVENT_TYPE);
    }

    TEST_CASE("trade missing required taker_order_id") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"trade","id":"tid","asset_id":"a","market":"m","status":"MATCHED","side":"BUY","price":"0.5","size":"10"})",
            1, 1, event);
        CHECK(err == ErrorCode::JSON_MISSING_FIELD);
    }

    TEST_CASE("trade unknown status") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"trade","id":"tid","asset_id":"a","market":"m","status":"WEIRD","side":"BUY","price":"0.5","size":"10","taker_order_id":"tok"})",
            1, 1, event);
        CHECK(err == ErrorCode::USER_UNKNOWN_STATUS);
    }

    TEST_CASE("order price parsed as 10000x fixed-point") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.1234","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->price == 1234);  // "0.1234" -> 1234
    }

    TEST_CASE("trade side BUY maps to BID") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"trade","id":"tid","asset_id":"a","market":"m","status":"MATCHED","side":"BUY","price":"0.5","size":"10","taker_order_id":"tok"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->side == Side::BID);
    }

    TEST_CASE("trade side SELL maps to ASK") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"trade","id":"tid","asset_id":"a","market":"m","status":"MATCHED","side":"SELL","price":"0.5","size":"10","taker_order_id":"tok"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserTradeUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->side == Side::ASK);
    }

    TEST_CASE("[audit2] client_order_id parsed when present") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","client_order_id":"my_local_id","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->client_order_id == OrderId("my_local_id"));
    }

    TEST_CASE("[audit2] client_order_id defaults to empty when absent") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->client_order_id.len == 0);
    }

    TEST_CASE("[hardening] server error message captured") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"error":"invalid API key"})", 1, 1, event);
        CHECK(err == ErrorCode::USER_WS_SERVER_ERROR);
        CHECK(std::string(event.server_error_msg) == "invalid API key");
    }

    TEST_CASE("[hardening] server error with extra fields") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"error":"rate limited","code":429})", 1, 1, event);
        CHECK(err == ErrorCode::USER_WS_SERVER_ERROR);
        CHECK(std::string(event.server_error_msg) == "rate limited");
    }

    TEST_CASE("[hardening] empty server error") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"error":""})", 1, 1, event);
        CHECK(err == ErrorCode::USER_WS_SERVER_ERROR);
        CHECK(std::string(event.server_error_msg) == "");
    }

    TEST_CASE("[hardening] unknown event_type captured in server_error_msg") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(R"({"event_type":"settlement"})", 1, 1, event);
        CHECK(err == ErrorCode::UNKNOWN_EVENT_TYPE);
        CHECK(std::string(event.server_error_msg) == "settlement");
    }

    TEST_CASE("[hardening] size_matched > original_size sets flag") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"UPDATE","side":"BUY","price":"0.5","original_size":"10","size_matched":"20"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->size_matched_exceeds_original == true);
    }

    TEST_CASE("[hardening] size_matched == original_size does not set flag") {
        UserMessageParser parser;
        UserMessageEvent event;

        auto err = parser.parse(
            R"({"event_type":"order","id":"oid","asset_id":"a","market":"m","type":"UPDATE","side":"BUY","price":"0.5","original_size":"10","size_matched":"10"})",
            1, 1, event);
        CHECK(err == ErrorCode::OK);

        auto* upd = std::get_if<UserOrderUpdate>(&event.payload);
        REQUIRE(upd != nullptr);
        CHECK(upd->size_matched_exceeds_original == false);
    }

    TEST_CASE("[hardening] long order_id triggers truncation detection") {
        UserMessageParser parser;
        UserMessageEvent event;

        // 100-char order ID exceeds kMaxOrderIdLen (80)
        std::string long_id(100, 'x');
        std::string json = R"({"event_type":"order","id":")" + long_id +
            R"(","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})";

        auto err = parser.parse(json, 1, 1, event);
        CHECK(err == ErrorCode::OK);
        CHECK(event.truncated_fields >= 1);
    }

    TEST_CASE("[hardening] normal order_id no truncation") {
        UserMessageParser parser;
        UserMessageEvent event;

        // 66-char order ID fits in kMaxOrderIdLen (80)
        std::string normal_id(66, 'a');
        std::string json = R"({"event_type":"order","id":")" + normal_id +
            R"(","asset_id":"a","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})";

        auto err = parser.parse(json, 1, 1, event);
        CHECK(err == ErrorCode::OK);
        CHECK(event.truncated_fields == 0);
    }

    TEST_CASE("[hardening] long asset_id triggers truncation detection") {
        UserMessageParser parser;
        UserMessageEvent event;

        // 200-char asset_id exceeds kMaxAssetIdLen (128)
        std::string long_aid(200, 'y');
        std::string json = R"({"event_type":"order","id":"oid","asset_id":")" + long_aid +
            R"(","market":"m","type":"PLACEMENT","side":"BUY","price":"0.5","original_size":"10","size_matched":"0"})";

        auto err = parser.parse(json, 1, 1, event);
        CHECK(err == ErrorCode::OK);
        CHECK(event.truncated_fields >= 1);
    }
}
