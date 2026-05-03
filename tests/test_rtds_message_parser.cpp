#include "doctest/doctest.h"
#include "rtds/rtds_message_parser.h"
#include "rtds/rtds_types.h"

using namespace lt;

TEST_SUITE("RtdsMessageParser") {

TEST_CASE("Parse valid crypto_prices update") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "crypto_prices",
        "type": "update",
        "timestamp": 1753314088421,
        "payload": {
            "symbol": "btcusdt",
            "timestamp": 1753314088395,
            "value": 67234.50
        }
    })";

    auto err = parser.parse(json, 999, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.symbol.view() == "btcusdt");
    CHECK(out.value == doctest::Approx(67234.50));
    CHECK(out.exchange_ts_ms == 1753314088395);
    CHECK(out.recv_ts == 999);
}

TEST_CASE("Parse ethusdt symbol") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "crypto_prices",
        "type": "update",
        "timestamp": 1753314088421,
        "payload": {
            "symbol": "ethusdt",
            "timestamp": 1753314088400,
            "value": 3456.78
        }
    })";

    auto err = parser.parse(json, 1000, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.symbol.view() == "ethusdt");
    CHECK(out.value == doctest::Approx(3456.78));
    CHECK(out.exchange_ts_ms == 1753314088400);
}

TEST_CASE("Parse integer value") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "crypto_prices",
        "type": "update",
        "timestamp": 1753314088421,
        "payload": {
            "symbol": "btcusdt",
            "timestamp": 1753314088395,
            "value": 67234
        }
    })";

    auto err = parser.parse(json, 500, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.value == doctest::Approx(67234.0));
}

TEST_CASE("PONG message returns PONG_MESSAGE") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    CHECK(parser.parse("PONG", 100, out) == ErrorCode::PONG_MESSAGE);
    CHECK(parser.parse("pong", 100, out) == ErrorCode::PONG_MESSAGE);
}

TEST_CASE("Non-crypto_prices topic returns UNKNOWN_EVENT_TYPE") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "market_data",
        "type": "update",
        "timestamp": 1753314088421,
        "payload": {}
    })";

    CHECK(parser.parse(json, 100, out) == ErrorCode::UNKNOWN_EVENT_TYPE);
}

TEST_CASE("Non-update type returns UNKNOWN_EVENT_TYPE") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "crypto_prices",
        "type": "snapshot",
        "timestamp": 1753314088421,
        "payload": {}
    })";

    CHECK(parser.parse(json, 100, out) == ErrorCode::UNKNOWN_EVENT_TYPE);
}

TEST_CASE("Invalid JSON returns PARSE_ERROR") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    CHECK(parser.parse("not json at all", 100, out) == ErrorCode::JSON_PARSE_ERROR);
    CHECK(parser.parse("{broken", 100, out) == ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("Missing payload fields return PARSE_ERROR") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    SUBCASE("Missing symbol") {
        const char* json = R"({
            "topic": "crypto_prices",
            "type": "update",
            "timestamp": 1753314088421,
            "payload": {
                "timestamp": 1753314088395,
                "value": 67234.50
            }
        })";
        CHECK(parser.parse(json, 100, out) == ErrorCode::JSON_PARSE_ERROR);
    }

    SUBCASE("Missing value") {
        const char* json = R"({
            "topic": "crypto_prices",
            "type": "update",
            "timestamp": 1753314088421,
            "payload": {
                "symbol": "btcusdt",
                "timestamp": 1753314088395
            }
        })";
        CHECK(parser.parse(json, 100, out) == ErrorCode::JSON_PARSE_ERROR);
    }

    SUBCASE("Missing payload") {
        const char* json = R"({
            "topic": "crypto_prices",
            "type": "update",
            "timestamp": 1753314088421
        })";
        CHECK(parser.parse(json, 100, out) == ErrorCode::JSON_PARSE_ERROR);
    }
}

TEST_CASE("Missing payload timestamp uses outer timestamp") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json = R"({
        "topic": "crypto_prices",
        "type": "update",
        "timestamp": 1753314088421,
        "payload": {
            "symbol": "btcusdt",
            "value": 67234.50
        }
    })";

    auto err = parser.parse(json, 100, out);
    CHECK(err == ErrorCode::OK);
    CHECK(out.exchange_ts_ms == 1753314088421);
}

TEST_CASE("CryptoSymbol truncation") {
    CryptoSymbol sym("abcdefghijklmnopqrstuvwxyz");  // 26 chars, max is 23
    CHECK(sym.len == 23);
    CHECK(sym.view() == "abcdefghijklmnopqrstuvw");
}

TEST_CASE("CryptoSymbol equality") {
    CryptoSymbol a("btcusdt");
    CryptoSymbol b("btcusdt");
    CryptoSymbol c("ethusdt");
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("CryptoPriceUpdate is trivially copyable POD") {
    CHECK(std::is_trivially_copyable_v<CryptoPriceUpdate>);
    CHECK(std::is_standard_layout_v<CryptoPriceUpdate>);
}

TEST_CASE("Multiple parses reuse parser safely") {
    RtdsMessageParser parser;
    CryptoPriceUpdate out;

    const char* json1 = R"({
        "topic": "crypto_prices", "type": "update",
        "timestamp": 1, "payload": {"symbol": "btcusdt", "timestamp": 1, "value": 100.0}
    })";
    const char* json2 = R"({
        "topic": "crypto_prices", "type": "update",
        "timestamp": 2, "payload": {"symbol": "ethusdt", "timestamp": 2, "value": 200.0}
    })";

    CHECK(parser.parse(json1, 10, out) == ErrorCode::OK);
    CHECK(out.symbol.view() == "btcusdt");
    CHECK(out.value == doctest::Approx(100.0));

    CHECK(parser.parse(json2, 20, out) == ErrorCode::OK);
    CHECK(out.symbol.view() == "ethusdt");
    CHECK(out.value == doctest::Approx(200.0));
}

}  // TEST_SUITE
