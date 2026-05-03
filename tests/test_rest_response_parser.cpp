#include <doctest/doctest.h>
#include "rest/rest_response_parser.h"

#include <fstream>
#include <sstream>
#include <string>

static std::string read_fixture(const std::string& name) {
    std::ifstream f(std::string(FIXTURES_DIR) + name);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

TEST_SUITE("RestResponseParser") {

TEST_CASE("parse order success response") {
    std::string json = read_fixture("rest_order_response.json");
    REQUIRE(!json.empty());

    auto result = lt::parse_order_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.success == true);
    CHECK(result.value.order_id == "0xabc123def456");
    CHECK(result.value.status == "live");
    CHECK(result.value.error_msg.empty());
}

TEST_CASE("parse order error response") {
    std::string json = R"({"success":false,"errorMsg":"insufficient balance","orderID":"","status":""})";
    auto result = lt::parse_order_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.success == false);
    CHECK(result.value.error_msg == "insufficient balance");
}

TEST_CASE("parse cancel response") {
    std::string json = read_fixture("rest_cancel_response.json");
    REQUIRE(!json.empty());

    auto result = lt::parse_cancel_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.canceled.size() == 2);
    CHECK(result.value.canceled[0] == "0xorder1");
    CHECK(result.value.canceled[1] == "0xorder2");
    CHECK(result.value.not_canceled.size() == 1);
    CHECK(result.value.not_canceled[0].first == "0xorder3");
    CHECK(result.value.not_canceled[0].second == "order not found");
}

TEST_CASE("parse heartbeat response") {
    std::string json = read_fixture("rest_heartbeat_response.json");
    REQUIRE(!json.empty());

    auto result = lt::parse_heartbeat_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.heartbeat_id == "hb-12345-67890");
}

TEST_CASE("parse empty cancel response") {
    std::string json = R"({"canceled":[],"not_canceled":{}})";
    auto result = lt::parse_cancel_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.canceled.empty());
    CHECK(result.value.not_canceled.empty());
}

TEST_CASE("parse cancel response missing fields returns error") {
    auto result = lt::parse_cancel_response(R"({})");
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::JSON_MISSING_FIELD);
}

TEST_CASE("parse cancel response wrong field types returns error") {
    auto result = lt::parse_cancel_response(R"({"canceled":"bad","not_canceled":{}})");
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::JSON_TYPE_ERROR);
}

TEST_CASE("parse invalid JSON returns error") {
    auto result = lt::parse_order_response("not json{{{");
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("parse order response with missing fields") {
    std::string json = R"({})";
    auto result = lt::parse_order_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.success == false);
    CHECK(result.value.order_id.empty());
}

TEST_CASE("parse batch order response with 2 items") {
    std::string json = R"([
        {"success":true,"errorMsg":"","orderID":"0xabc123","status":"live"},
        {"success":true,"errorMsg":"","orderID":"0xdef456","status":"matched"}
    ])";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 2);
    CHECK(result.value.items[0].success == true);
    CHECK(result.value.items[0].order_id == "0xabc123");
    CHECK(result.value.items[0].status == "live");
    CHECK(result.value.items[1].success == true);
    CHECK(result.value.items[1].order_id == "0xdef456");
    CHECK(result.value.items[1].status == "matched");
}

TEST_CASE("parse batch order response with mixed success/failure") {
    std::string json = R"([
        {"success":true,"errorMsg":"","orderID":"0xabc","status":"live"},
        {"success":false,"errorMsg":"insufficient balance","orderID":"","status":""}
    ])";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 2);
    CHECK(result.value.items[0].success == true);
    CHECK(result.value.items[0].order_id == "0xabc");
    CHECK(result.value.items[1].success == false);
    CHECK(result.value.items[1].error_msg == "insufficient balance");
}

TEST_CASE("parse batch order response with empty array") {
    std::string json = R"([])";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    CHECK(result.value.items.empty());
}

TEST_CASE("parse batch order response with malformed JSON") {
    auto result = lt::parse_batch_order_response("not json{{{");
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::JSON_PARSE_ERROR);
}

TEST_CASE("parse batch order response with non-array top-level (no orders key)") {
    auto result = lt::parse_batch_order_response(R"({"success":true})");
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::JSON_TYPE_ERROR);
}

TEST_CASE("parse batch order response with orders wrapper") {
    std::string json = R"({"orders":[{"success":true,"orderID":"0xwrapped","status":"live"}]})";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 1);
    CHECK(result.value.items[0].success == true);
    CHECK(result.value.items[0].order_id == "0xwrapped");
}

TEST_CASE("parse batch order response with id fallback field") {
    std::string json = R"([{"success":true,"id":"0xfallback","status":"live"}])";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 1);
    CHECK(result.value.items[0].order_id == "0xfallback");
}

TEST_CASE("parse batch order response with top-level error object") {
    auto result = lt::parse_batch_order_response(R"({"error":"rate limited"})");
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 1);
    CHECK(result.value.items[0].success == false);
    CHECK(result.value.items[0].error_msg == "rate limited");
}

TEST_CASE("parse batch order response success=true empty orderID captures raw JSON") {
    std::string json = R"([{"success":true,"status":"live"}])";
    auto result = lt::parse_batch_order_response(json);
    REQUIRE(result.ok());
    REQUIRE(result.value.items.size() == 1);
    CHECK(result.value.items[0].success == true);
    CHECK(result.value.items[0].order_id.empty());
    CHECK_FALSE(result.value.items[0].error_msg.empty());  // raw JSON captured
}

}  // TEST_SUITE
