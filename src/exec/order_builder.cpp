#include "exec/order_builder.h"
#include "crypto/hex_utils.h"
#include "crypto/keccak.h"
#include "crypto/eip712.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace lt {

OrderBuilder::OrderBuilder(OrderSigner& signer, std::string_view owner_uuid,
                           std::string_view maker_address, std::string_view signer_address,
                           const OrderBuilderConfig& cfg)
    : signer_(signer),
      owner_uuid_(owner_uuid),
      config_(cfg) {
    // Polymarket requires lowercase owner address
    std::transform(owner_uuid_.begin(), owner_uuid_.end(), owner_uuid_.begin(), ::tolower);
    if (!parse_eth_address(maker_address, maker_addr_bytes_)) {
        throw std::runtime_error("OrderBuilder: invalid maker_address");
    }
    if (!parse_eth_address(signer_address, signer_addr_bytes_)) {
        throw std::runtime_error("OrderBuilder: invalid signer_address");
    }

    // Compute EIP-55 checksummed addresses from raw bytes for JSON payload
    maker_address_ = eip55_checksum(maker_addr_bytes_);
    signer_address_ = eip55_checksum(signer_addr_bytes_);

    // Pre-compute domain separators
    domain_sep_normal_ = compute_domain_separator(CTF_EXCHANGE_ADDRESS);
    domain_sep_neg_risk_ = compute_domain_separator(NEG_RISK_CTF_EXCHANGE_ADDRESS);
}

uint64_t OrderBuilder::generate_salt() const {
    // Match official Polymarket clients: round(unix_seconds * random_float)
    // Result must be <= 2^53-1 (backend parses salt as IEEE 754 double)
    auto now = std::chrono::system_clock::now();
    double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
    // Simple pseudo-random: use fractional nanoseconds from high_resolution_clock
    auto hrc = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        hrc.time_since_epoch()).count();
    // Map lower bits to [0,1) range
    double rand_frac = static_cast<double>(ns & 0xFFFFFFFF) / 4294967296.0;
    uint64_t salt = static_cast<uint64_t>(seconds * rand_frac);
    // Mask to 53 bits for IEEE 754 safety
    return salt & ((1ULL << 53) - 1);
}

Expected<OrderPayload> OrderBuilder::build(const ExecIntent& intent) {
    if (intent.type != ExecIntentType::PLACE_ORDER &&
        intent.type != ExecIntentType::REPLACE_ORDER) {
        return ErrorCode::INVALID_FORMAT;
    }

    if (intent.price <= 0 || intent.price > kPriceMax || intent.size <= 0) {
        return ErrorCode::OUT_OF_RANGE;
    }

    // Amount calculation (matching official Polymarket clients):
    // intent.size is in micro-shares (kQtyScale = 10^6). USDC also uses 10^6.
    // raw_amount (shares in USDC units) = intent.size (already micro-shares)
    // price_amount = intent.size * price / kPriceScale

    // Round amounts to exchange-required precision:
    // Shares: max 2 decimals → round to nearest 10000 (0.01 shares)
    // USDC:   max 4 decimals → round to nearest 100 (0.0001 USDC)
    constexpr int64_t kShareRound = 10000;   // 2 decimal places in 6-decimal token
    constexpr int64_t kUsdcRound = 100;      // 4 decimal places in 6-decimal USDC
    int64_t raw_amount = (intent.size / kShareRound) * kShareRound;
    if (raw_amount <= 0) raw_amount = kShareRound;  // minimum 0.01 shares
    int64_t price_amount = ((raw_amount * static_cast<int64_t>(intent.price)) / kPriceScale
                            / kUsdcRound) * kUsdcRound;

    char maker_amount_buf[32];
    char taker_amount_buf[32];
    auto int_to_view = [](int64_t value, char* buf, std::size_t cap) -> std::string_view {
        auto [ptr, ec] = std::to_chars(buf, buf + cap, value);
        if (ec != std::errc()) return {};
        return std::string_view(buf, static_cast<std::size_t>(ptr - buf));
    };
    std::string_view maker_amount_sv;
    std::string_view taker_amount_sv;
    if (intent.side == Side::BID) {
        // BUY
        maker_amount_sv = int_to_view(price_amount, maker_amount_buf, sizeof(maker_amount_buf));
        taker_amount_sv = int_to_view(raw_amount, taker_amount_buf, sizeof(taker_amount_buf));
    } else {
        // SELL
        maker_amount_sv = int_to_view(raw_amount, maker_amount_buf, sizeof(maker_amount_buf));
        taker_amount_sv = int_to_view(price_amount, taker_amount_buf, sizeof(taker_amount_buf));
    }
    if (maker_amount_sv.empty() || taker_amount_sv.empty()) {
        return ErrorCode::OUT_OF_RANGE;
    }

    // Build OrderFields
    OrderFields fields;
    uint64_t salt_val = generate_salt();
    uint64_to_uint256_be(salt_val, fields.salt.data());
    std::memcpy(fields.maker, maker_addr_bytes_, 20);
    std::memcpy(fields.signer, signer_addr_bytes_, 20);
    std::memset(fields.taker, 0, 20);  // any taker

    // tokenId from asset_id (decimal string -> uint256)
    decimal_to_uint256(intent.asset_id.view(), fields.token_id);

    // amounts as uint256
    decimal_to_uint256(maker_amount_sv, fields.maker_amount);
    decimal_to_uint256(taker_amount_sv, fields.taker_amount);

    fields.expiration = static_cast<uint64_t>(intent.expiration);
    fields.nonce = 0;  // exchange assigns
    fields.fee_rate_bps = intent.fee_rate_bps;
    fields.side = (intent.side == Side::BID) ? 0 : 1;
    fields.signature_type = config_.signature_type;

    // Select domain separator
    const Bytes32& domain_sep = intent.neg_risk ? domain_sep_neg_risk_ : domain_sep_normal_;

    // Compute signing hash for debug logging
    Bytes32 struct_hash = hash_order_struct(fields);
    Bytes32 signing_hash = eip712_signing_hash(domain_sep, struct_hash);

    // Sign order
    uint8_t signature[65];
    if (!signer_.sign_order(fields, domain_sep, signature)) {
        return ErrorCode::INVALID_FORMAT;
    }

    std::string sig_hex = "0x" + hex_encode(signature, 65);
    // Determine order type string
    const char* order_type_str = "GTC";
    switch (intent.order_type) {
        case OrderType::GTC: order_type_str = "GTC"; break;
        case OrderType::GTD: order_type_str = "GTD"; break;
        case OrderType::FOK: order_type_str = "FOK"; break;
        case OrderType::FAK: order_type_str = "FAK"; break;
    }

    // Build JSON payload using a reserved string buffer.
    std::string json;
    json.reserve(768);
    auto append_i64 = [&](int64_t v) {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (ec == std::errc()) json.append(buf, static_cast<std::size_t>(ptr - buf));
    };
    auto append_u32 = [&](uint32_t v) {
        char buf[16];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        if (ec == std::errc()) json.append(buf, static_cast<std::size_t>(ptr - buf));
    };

    json += R"({"order":{)";
    json += R"("salt":)";
    append_i64(static_cast<int64_t>(salt_val));
    json += ',';
    json += R"("maker":")";
    json += maker_address_;
    json += R"(",)";
    json += R"("signer":")";
    json += signer_address_;
    json += R"(",)";
    json += R"("taker":"0x0000000000000000000000000000000000000000",)";
    json += R"("tokenId":")";
    json.append(intent.asset_id.data, intent.asset_id.len);
    json += R"(",)";
    json += R"("makerAmount":")";
    json.append(maker_amount_sv.data(), maker_amount_sv.size());
    json += R"(",)";
    json += R"("takerAmount":")";
    json.append(taker_amount_sv.data(), taker_amount_sv.size());
    json += R"(",)";
    json += R"("expiration":")";
    append_i64(intent.expiration);
    json += R"(",)";
    json += R"("nonce":"0",)";
    json += R"("feeRateBps":")";
    append_u32(static_cast<uint32_t>(intent.fee_rate_bps));
    json += R"(",)";
    json += R"("side":")";
    json += (intent.side == Side::BID ? "BUY" : "SELL");
    json += R"(",)";
    json += R"("signatureType":)";
    append_u32(static_cast<uint32_t>(config_.signature_type));
    json += R"(,)";
    json += R"("signature":")";
    json += sig_hex;
    json += R"(")";
    json += R"(},)";
    json += R"("owner":")";
    json += owner_uuid_;
    json += R"(",)";
    json += R"("orderType":")";
    json += order_type_str;
    json += R"(",)";
    json += R"("deferExec":)";
    json += (config_.defer_exec ? "true" : "false");

    if (config_.post_only &&
        (intent.order_type == OrderType::GTC || intent.order_type == OrderType::GTD)) {
        json += R"(,"postOnly":true)";
    }
    if (intent.neg_risk) json += R"(,"negRisk":true)";
    json += '}';

    OrderPayload payload;
    payload.json_body = std::move(json);
    payload.client_order_id = intent.client_order_id;
    payload.debug_signing_hash = hex_encode(signing_hash);
    // Predicted exchange order ID = "0x" + hex(signing_hash).
    // Polymarket uses the EIP-712 signing hash as the order identifier.
    // Available before REST send — enables pre-registration for FAK side resolution.
    std::string oid_hex = "0x" + payload.debug_signing_hash;
    payload.predicted_exchange_oid = OrderId(oid_hex);
    return Expected<OrderPayload>(std::move(payload));
}

Expected<BatchOrderPayload> OrderBuilder::build_batch(const ExecIntent* intents, int count) {
    if (count <= 0 || count > kBatchMaxOrders) {
        return ErrorCode::OUT_OF_RANGE;
    }

    BatchOrderPayload batch;
    batch.count = count;

    std::string json;
    json.reserve(static_cast<size_t>(768 * count + 16));
    json += '[';

    for (int i = 0; i < count; ++i) {
        auto result = build(intents[i]);
        if (!result.ok()) return result.error;

        if (i > 0) json += ',';
        json += result.value.json_body;
        batch.client_order_ids[i] = result.value.client_order_id;
        batch.predicted_exchange_oids[i] = result.value.predicted_exchange_oid;
    }

    json += ']';
    batch.json_body = std::move(json);
    return Expected<BatchOrderPayload>(std::move(batch));
}

}  // namespace lt
