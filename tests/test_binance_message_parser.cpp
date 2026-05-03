#include "binance/binance_message_parser.h"
#include "binance/binance_types.h"
#include "doctest/doctest.h"

using namespace lt;

TEST_SUITE("BinanceMessageParser") {

TEST_CASE("bookTicker combined-stream envelope") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({
        "stream":"btcusdt@bookTicker",
        "data":{"u":400900217,"s":"BTCUSDT",
                "b":"67234.50000000","B":"0.12300000",
                "a":"67235.10000000","A":"0.98700000"}
    })";

    auto err = parser.parse(json, 42, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.type == static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER));
    CHECK(out.symbol.view() == "btcusdt");
    CHECK(out.bid_price == doctest::Approx(67234.5));
    CHECK(out.bid_qty == doctest::Approx(0.123));
    CHECK(out.ask_price == doctest::Approx(67235.1));
    CHECK(out.ask_qty == doctest::Approx(0.987));
    CHECK(out.update_id == 400900217);
    CHECK(out.recv_ts == 42);
}

TEST_CASE("bookTicker raw (single-stream) payload without envelope") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"u":1,"s":"BTCUSDT",
        "b":"100.0","B":"1.0","a":"101.0","A":"2.0"})";

    auto err = parser.parse(json, 7, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.type == static_cast<uint8_t>(BinanceStreamType::BOOK_TICKER));
    CHECK(out.symbol.view() == "btcusdt");
    CHECK(out.bid_price == doctest::Approx(100.0));
    CHECK(out.ask_price == doctest::Approx(101.0));
}

TEST_CASE("trade combined-stream envelope") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({
        "stream":"btcusdt@trade",
        "data":{"e":"trade","E":1672515782136,"s":"BTCUSDT","t":12345,
                "p":"67230.75","q":"0.00150000","T":1672515782140,"m":true}
    })";

    auto err = parser.parse(json, 100, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.type == static_cast<uint8_t>(BinanceStreamType::TRADE));
    CHECK(out.symbol.view() == "btcusdt");
    CHECK(out.last_price == doctest::Approx(67230.75));
    CHECK(out.last_qty == doctest::Approx(0.0015));
    CHECK(out.exchange_ts_ms == 1672515782140);
    CHECK(out.update_id == 12345);
    CHECK(out.buyer_is_maker == 1);
    CHECK(out.recv_ts == 100);
}

TEST_CASE("trade raw payload detected via event field") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"e":"trade","E":1000,"s":"ETHUSDT","t":7,
        "p":"3500.5","q":"1.0","T":1001,"m":false})";

    auto err = parser.parse(json, 1, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.type == static_cast<uint8_t>(BinanceStreamType::TRADE));
    CHECK(out.symbol.view() == "ethusdt");
    CHECK(out.buyer_is_maker == 0);
    CHECK(out.last_price == doctest::Approx(3500.5));
}

TEST_CASE("aggTrade") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"stream":"btcusdt@aggTrade","data":{
        "e":"aggTrade","E":5,"s":"BTCUSDT","a":99,
        "p":"50000.0","q":"0.5","f":10,"l":12,"T":6,"m":true}})";

    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.type == static_cast<uint8_t>(BinanceStreamType::AGG_TRADE));
    CHECK(out.update_id == 99);
    CHECK(out.exchange_ts_ms == 6);
    CHECK(out.last_price == doctest::Approx(50000.0));
}

TEST_CASE("subscribe ack is reported as UNKNOWN_EVENT_TYPE") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"result":null,"id":1})";
    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::UNKNOWN_EVENT_TYPE);
}

TEST_CASE("malformed JSON -> JSON_PARSE_ERROR") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    auto err = parser.parse("{not-json", 0, out);
    CHECK(err == ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("bookTicker missing required string field fails") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    // Missing "a"
    const char* json = R"({"stream":"btcusdt@bookTicker",
        "data":{"u":1,"s":"BTCUSDT","b":"1.0","B":"1.0","A":"1.0"}})";
    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("non-numeric price string fails") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"stream":"btcusdt@bookTicker",
        "data":{"u":1,"s":"BTCUSDT","b":"abc","B":"1.0","a":"1.0","A":"1.0"}})";
    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("unknown stream suffix -> UNKNOWN_EVENT_TYPE") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    // Envelope says "kline_1m" which we don't decode; data has no "e"=trade/aggTrade.
    // Fallback path returns JSON_PARSE_ERROR when required fields missing, or
    // UNKNOWN_EVENT_TYPE if we can tell from the stream suffix.
    const char* json = R"({"stream":"btcusdt@kline_1m","data":{"s":"BTCUSDT"}})";
    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::UNKNOWN_EVENT_TYPE);
}

TEST_CASE("symbol is lowercased for consistency with RTDS") {
    BinanceMessageParser parser;
    BinanceMarketUpdate out;

    const char* json = R"({"u":1,"s":"ETHUSDT",
        "b":"1.0","B":"1.0","a":"1.0","A":"1.0"})";
    auto err = parser.parse(json, 0, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.symbol.view() == "ethusdt");
}

}  // TEST_SUITE
