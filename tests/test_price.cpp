#include <doctest/doctest.h>

#include "common/price.h"
#include "common/types.h"

using namespace lt;

TEST_SUITE("parse_price") {
    TEST_CASE("standard decimal") {
        auto r = parse_price("0.52");
        CHECK(r.ok());
        CHECK(r.value == 5200);
    }

    TEST_CASE("leading dot") {
        auto r = parse_price(".48");
        CHECK(r.ok());
        CHECK(r.value == 4800);
    }

    TEST_CASE("three decimal places") {
        auto r = parse_price("0.001");
        CHECK(r.ok());
        CHECK(r.value == 10);
    }

    TEST_CASE("one decimal place") {
        auto r = parse_price("0.5");
        CHECK(r.ok());
        CHECK(r.value == 5000);
    }

    TEST_CASE("integer zero") {
        auto r = parse_price("0");
        CHECK(r.ok());
        CHECK(r.value == 0);
    }

    TEST_CASE("integer one") {
        auto r = parse_price("1");
        CHECK(r.ok());
        CHECK(r.value == 10000);
    }

    TEST_CASE("1.0") {
        auto r = parse_price("1.0");
        CHECK(r.ok());
        CHECK(r.value == 10000);
    }

    TEST_CASE("four decimal places") {
        auto r = parse_price("0.1234");
        CHECK(r.ok());
        CHECK(r.value == 1234);
    }

    TEST_CASE("0.99") {
        auto r = parse_price("0.99");
        CHECK(r.ok());
        CHECK(r.value == 9900);
    }

    TEST_CASE("0.01") {
        auto r = parse_price("0.01");
        CHECK(r.ok());
        CHECK(r.value == 100);
    }

    TEST_CASE("empty string") {
        auto r = parse_price("");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::EMPTY_INPUT);
    }

    TEST_CASE("invalid string") {
        auto r = parse_price("abc");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::INVALID_FORMAT);
    }

    TEST_CASE("out of range high") {
        auto r = parse_price("2.0");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::OUT_OF_RANGE);
    }

    TEST_CASE("negative") {
        auto r = parse_price("-0.5");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::OUT_OF_RANGE);
    }

    TEST_CASE("just dot") {
        auto r = parse_price(".");
        CHECK(r.ok());
        CHECK(r.value == 0);
    }
}

TEST_SUITE("parse_qty") {
    TEST_CASE("valid integer") {
        auto r = parse_qty("100");
        CHECK(r.ok());
        CHECK(r.value == qty_from_int(100));
    }

    TEST_CASE("zero") {
        auto r = parse_qty("0");
        CHECK(r.ok());
        CHECK(r.value == 0);
    }

    TEST_CASE("large number") {
        auto r = parse_qty("1000000");
        CHECK(r.ok());
        CHECK(r.value == qty_from_int(1000000));
    }

    TEST_CASE("empty") {
        auto r = parse_qty("");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::EMPTY_INPUT);
    }

    TEST_CASE("invalid") {
        auto r = parse_qty("abc");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::INVALID_FORMAT);
    }

    TEST_CASE("negative") {
        auto r = parse_qty("-5");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::NEGATIVE_VALUE);
    }

    TEST_CASE("decimal with trailing zeros") {
        auto r = parse_qty("150.00");
        CHECK(r.ok());
        CHECK(r.value == qty_from_int(150));
    }

    TEST_CASE("decimal with fractional part") {
        auto r = parse_qty("150.50");
        CHECK(r.ok());
        CHECK(r.value == 150 * kQtyScale + 500000);
    }

    TEST_CASE("zero with decimal") {
        auto r = parse_qty("0.00");
        CHECK(r.ok());
        CHECK(r.value == 0);
    }

    TEST_CASE("decimal one digit frac") {
        auto r = parse_qty("12.5");
        CHECK(r.ok());
        CHECK(r.value == 12 * kQtyScale + 500000);
    }

    TEST_CASE("leading dot only") {
        auto r = parse_qty(".5");
        CHECK(r.ok());
        CHECK(r.value == 500000);
    }

    TEST_CASE("decimal with invalid frac") {
        auto r = parse_qty("100.abc");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::INVALID_FORMAT);
    }

    TEST_CASE("[hardening] overflow detection - huge value") {
        auto r = parse_qty("9999999999999999");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::QTY_OVERFLOW);
    }

    TEST_CASE("[hardening] large but valid value") {
        auto r = parse_qty("1000000");
        CHECK(r.ok());
        CHECK(r.value == 1000000LL * kQtyScale);
    }

    TEST_CASE("[hardening] overflow boundary - just below") {
        // max int64 / kQtyScale = 9223372036854775807 / 1000000 = 9223372036854
        auto r = parse_qty("9223372036854");
        CHECK(r.ok());
        CHECK(r.value == 9223372036854LL * kQtyScale);
    }

    TEST_CASE("[hardening] overflow boundary - just above") {
        // 9223372036855 * 1000000 would overflow int64
        auto r = parse_qty("9223372036855");
        CHECK(!r.ok());
        CHECK(r.error == ErrorCode::QTY_OVERFLOW);
    }
}

TEST_SUITE("parse_side") {
    TEST_CASE("BUY") {
        auto r = parse_side("BUY");
        CHECK(r.ok());
        CHECK(r.value == Side::BID);
    }

    TEST_CASE("SELL") {
        auto r = parse_side("SELL");
        CHECK(r.ok());
        CHECK(r.value == Side::ASK);
    }

    TEST_CASE("BID") {
        auto r = parse_side("BID");
        CHECK(r.ok());
        CHECK(r.value == Side::BID);
    }

    TEST_CASE("ASK") {
        auto r = parse_side("ASK");
        CHECK(r.ok());
        CHECK(r.value == Side::ASK);
    }

    TEST_CASE("invalid") {
        auto r = parse_side("UNKNOWN");
        CHECK(!r.ok());
    }
}

TEST_SUITE("format_price") {
    TEST_CASE("5200 -> 0.52") { CHECK(format_price(5200) == "0.52"); }

    TEST_CASE("4800 -> 0.48") { CHECK(format_price(4800) == "0.48"); }

    TEST_CASE("10 -> 0.001") { CHECK(format_price(10) == "0.001"); }

    TEST_CASE("10000 -> 1") { CHECK(format_price(10000) == "1"); }

    TEST_CASE("0 -> 0") { CHECK(format_price(0) == "0"); }

    TEST_CASE("1234 -> 0.1234") { CHECK(format_price(1234) == "0.1234"); }

    TEST_CASE("100 -> 0.01") { CHECK(format_price(100) == "0.01"); }
}

TEST_SUITE("is_on_tick") {
    TEST_CASE("0.52 on 0.01 tick") { CHECK(is_on_tick(5200, 100)); }

    TEST_CASE("0.52 on 0.001 tick") { CHECK(is_on_tick(5200, 10)); }

    TEST_CASE("0.525 NOT on 0.01 tick") { CHECK_FALSE(is_on_tick(5250, 100)); }

    TEST_CASE("0.525 on 0.001 tick") {
        // 5250 % 10 = 0
        CHECK(is_on_tick(5250, 10));
    }

    TEST_CASE("invalid tick size") { CHECK_FALSE(is_on_tick(5200, 0)); }
}
