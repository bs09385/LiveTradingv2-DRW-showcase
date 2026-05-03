#include <doctest/doctest.h>
#include "exec/order_builder.h"
#include "crypto/order_signer.h"
#include "crypto/hex_utils.h"

#include <cstring>

TEST_SUITE("OrderBuilder") {

static const char* TEST_PRIVATE_KEY_HEX =
    "ac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";
static const char* EXPECTED_ADDRESS = "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266";
static const char* TEST_OWNER_UUID = "test-owner-uuid-123";

TEST_CASE("build BUY order produces valid JSON") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5200;  // 0.52
    intent.size = lt::qty_from_int(100);
    intent.order_type = lt::OrderType::GTC;
    intent.client_order_id = lt::OrderId("client-001");

    auto result = builder.build(intent);
    REQUIRE(result.ok());
    CHECK_FALSE(result.value.json_body.empty());

    // Verify JSON contains expected fields
    auto& json = result.value.json_body;
    CHECK(json.find("\"order\"") != std::string::npos);
    CHECK(json.find("\"salt\"") != std::string::npos);
    CHECK(json.find("\"maker\"") != std::string::npos);
    CHECK(json.find("\"signer\"") != std::string::npos);
    CHECK(json.find("\"signature\"") != std::string::npos);
    CHECK(json.find("\"tokenId\":\"12345\"") != std::string::npos);
    CHECK(json.find("\"side\":\"BUY\"") != std::string::npos);
    CHECK(json.find("\"orderType\":\"GTC\"") != std::string::npos);
    CHECK(json.find(TEST_OWNER_UUID) != std::string::npos);
}

TEST_CASE("build SELL order has correct side") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::ASK;
    intent.price = 7500;  // 0.75
    intent.size = lt::qty_from_int(50);
    intent.order_type = lt::OrderType::GTC;

    auto result = builder.build(intent);
    REQUIRE(result.ok());
    CHECK(result.value.json_body.find("\"side\":\"SELL\"") != std::string::npos);
}

TEST_CASE("BUY amount calculation") {
    // BUY: takerAmount = size * 1_000_000
    //      makerAmount = size * (price/10000) * 1_000_000
    // price=5200 (0.52), size=100
    // takerAmount = 100 * 1000000 = 100000000
    // makerAmount = 100 * 5200 * 1000000 / 10000 = 52000000
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5200;
    intent.size = lt::qty_from_int(100);

    auto result = builder.build(intent);
    REQUIRE(result.ok());

    // takerAmount = size in micro-units = 100 * 10^6 = 100000000
    CHECK(result.value.json_body.find("\"takerAmount\":\"100000000\"") != std::string::npos);
    // makerAmount = size * price / kPriceScale = 100000000 * 5200 / 10000 = 52000000
    CHECK(result.value.json_body.find("\"makerAmount\":\"52000000\"") != std::string::npos);
}

TEST_CASE("SELL amount calculation") {
    // SELL: makerAmount = size * 1_000_000
    //       takerAmount = size * (price/10000) * 1_000_000
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::ASK;
    intent.price = 5200;
    intent.size = lt::qty_from_int(100);

    auto result = builder.build(intent);
    REQUIRE(result.ok());

    // makerAmount = size in micro-units = 100 * 10^6 = 100000000
    CHECK(result.value.json_body.find("\"makerAmount\":\"100000000\"") != std::string::npos);
    // takerAmount = size * price / kPriceScale = 100000000 * 5200 / 10000 = 52000000
    CHECK(result.value.json_body.find("\"takerAmount\":\"52000000\"") != std::string::npos);
}

TEST_CASE("negRisk flag included when set") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5000;
    intent.size = lt::qty_from_int(10);
    intent.neg_risk = true;

    auto result = builder.build(intent);
    REQUIRE(result.ok());
    CHECK(result.value.json_body.find("\"negRisk\":true") != std::string::npos);
}

TEST_CASE("negRisk flag absent when not set") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5000;
    intent.size = lt::qty_from_int(10);
    intent.neg_risk = false;

    auto result = builder.build(intent);
    REQUIRE(result.ok());
    CHECK(result.value.json_body.find("negRisk") == std::string::npos);
}

TEST_CASE("rejects invalid price") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 0;  // invalid
    intent.size = lt::qty_from_int(10);

    auto result = builder.build(intent);
    CHECK_FALSE(result.ok());
}

TEST_CASE("rejects cancel intent") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::CANCEL_ORDER;

    auto result = builder.build(intent);
    CHECK_FALSE(result.ok());
}

TEST_CASE("GTD order type") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5000;
    intent.size = lt::qty_from_int(10);
    intent.order_type = lt::OrderType::GTD;
    intent.expiration = 1700000000;

    auto result = builder.build(intent);
    REQUIRE(result.ok());
    CHECK(result.value.json_body.find("\"orderType\":\"GTD\"") != std::string::npos);
}

TEST_CASE("build_batch with 2 intents produces valid array JSON") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intents[2];
    intents[0].type = lt::ExecIntentType::PLACE_ORDER;
    intents[0].asset_id = lt::AssetId("12345");
    intents[0].side = lt::Side::BID;
    intents[0].price = 5200;
    intents[0].size = lt::qty_from_int(100);
    intents[0].order_type = lt::OrderType::GTC;
    intents[0].client_order_id = lt::OrderId("client-001");

    intents[1].type = lt::ExecIntentType::PLACE_ORDER;
    intents[1].asset_id = lt::AssetId("67890");
    intents[1].side = lt::Side::ASK;
    intents[1].price = 7500;
    intents[1].size = lt::qty_from_int(50);
    intents[1].order_type = lt::OrderType::GTC;
    intents[1].client_order_id = lt::OrderId("client-002");

    auto result = builder.build_batch(intents, 2);
    REQUIRE(result.ok());
    CHECK(result.value.count == 2);

    auto& json = result.value.json_body;
    CHECK(json.front() == '[');
    CHECK(json.back() == ']');

    // Both orders should be present
    CHECK(json.find("\"tokenId\":\"12345\"") != std::string::npos);
    CHECK(json.find("\"tokenId\":\"67890\"") != std::string::npos);
    CHECK(json.find("\"side\":\"BUY\"") != std::string::npos);
    CHECK(json.find("\"side\":\"SELL\"") != std::string::npos);

    // Each entry should have order/owner/orderType structure
    // Count occurrences of "order" key (should be 2 inner + structure)
    size_t order_count = 0;
    size_t pos = 0;
    while ((pos = json.find("\"order\":{", pos)) != std::string::npos) {
        ++order_count;
        ++pos;
    }
    CHECK(order_count == 2);

    // Client order IDs populated
    CHECK(result.value.client_order_ids[0].view() == "client-001");
    CHECK(result.value.client_order_ids[1].view() == "client-002");
}

TEST_CASE("build_batch with 1 intent still works") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intent;
    intent.type = lt::ExecIntentType::PLACE_ORDER;
    intent.asset_id = lt::AssetId("12345");
    intent.side = lt::Side::BID;
    intent.price = 5000;
    intent.size = lt::qty_from_int(10);
    intent.client_order_id = lt::OrderId("client-solo");

    auto result = builder.build_batch(&intent, 1);
    REQUIRE(result.ok());
    CHECK(result.value.count == 1);
    CHECK(result.value.json_body.front() == '[');
    CHECK(result.value.json_body.back() == ']');
    CHECK(result.value.client_order_ids[0].view() == "client-solo");
}

TEST_CASE("build_batch rejects count 0") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    auto result = builder.build_batch(nullptr, 0);
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::OUT_OF_RANGE);
}

TEST_CASE("build_batch rejects count > 15") {
    lt::Bytes32 pk{};
    lt::hex_decode_to_bytes32(TEST_PRIVATE_KEY_HEX, pk);
    lt::Secp256k1OrderSigner signer(pk.data());

    lt::OrderBuilder builder(signer, TEST_OWNER_UUID, EXPECTED_ADDRESS, EXPECTED_ADDRESS);

    lt::ExecIntent intents[16];
    auto result = builder.build_batch(intents, 16);
    CHECK_FALSE(result.ok());
    CHECK(result.error == lt::ErrorCode::OUT_OF_RANGE);
}

}  // TEST_SUITE
